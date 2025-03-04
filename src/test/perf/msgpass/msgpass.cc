/**
 * A simulation of a message-passing application workload for snmalloc.
 *
 * - N_PRODUCER producer threads allocate and queue spans of messages randomly,
 * - to N_CONSUMER consumer threads, which dequeue messages and free() them.
 *
 * Optionally, N_PROXY threads act as both producers and consumers, forwarding
 * received messages back to another queue rather than freeing them.
 */

#include "test/opt.h"
#include "test/setup.h"
#include "test/usage.h"
#include "test/xoroshiro.h"

constexpr static bool be_chatty = false;

#include <chrono>
#include <iomanip>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <stdarg.h>
#include <thread>
#include <vector>

using namespace snmalloc;

void chatty(const char* p, ...)
{
  if constexpr (be_chatty)
  {
    va_list va;
    va_start(va, p);
    vfprintf(stderr, p, va);
    va_end(va);
  }
}

/*
 * FreeListMPSCQ make for convenient MPSC queues, so we use those for sending
 * "messages".  Each consumer or proxy has its own (source) queue.
 */
static FreeListKey msgqueue_key{0xab2acada, 0xb2a01234, 0x56789abc};
static constexpr address_t msgqueue_key_tweak = 0xfedc'ba98;

struct params
{
  size_t N_PRODUCER;
  size_t N_CONSUMER;
  size_t N_PROXY;
  size_t N_QUEUE;
  size_t N_PRODUCER_BATCH;
  size_t N_MAX_OUTSTANDING;
  size_t N_MAX_BATCH_SIZE;
  FreeListMPSCQ<msgqueue_key, msgqueue_key_tweak>* msgqueue; // [N_QUEUE]
};

std::atomic<bool> producers_live;
std::atomic<size_t> queue_gate;
std::atomic<size_t> messages_outstanding;

freelist::HeadPtr domesticate_nop(freelist::QueuePtr p)
{
  return freelist::HeadPtr::unsafe_from(p.unsafe_ptr());
};

void consumer(const struct params* param, size_t qix)
{
  auto& myq = param->msgqueue[qix];

  chatty("Cl %zu q is %p\n", qix, &myq);

  do
  {
    size_t reap = 0;

    if (myq.can_dequeue())
    {
      myq.dequeue(
        domesticate_nop, domesticate_nop, [qix, &reap](freelist::HeadPtr o) {
          UNUSED(qix);
          auto p = o.as_void().unsafe_ptr();
          chatty("Cl %zu free %p\n", qix, p);
          snmalloc::dealloc(p);
          reap++;
          return true;
        });
    }

    messages_outstanding -= reap;

    if (reap == 0)
    {
      std::this_thread::yield();
    }
    else
    {
      chatty("Cl %zu reap %zu\n", qix, reap);
    }

  } while (myq.can_dequeue() || producers_live ||
           (queue_gate > param->N_CONSUMER));

  chatty("Cl %zu fini\n", qix);
  snmalloc::dealloc(myq.destroy().unsafe_ptr());
}

void proxy(const struct params* param, size_t qix)
{
  auto& myq = param->msgqueue[qix];
  auto& qs = param->msgqueue;

  chatty("Px %zu q is %p\n", qix, &myq);

  xoroshiro::p128r32 r(1234 + qix, qix);
  do
  {
    if (myq.can_dequeue())
    {
      myq.dequeue(
        domesticate_nop, domesticate_nop, [qs, qix, &r](freelist::HeadPtr o) {
          auto rcptqix = r.next() % qix;

          chatty(
            "Px %zu send %p to %zu\n", qix, o.as_void().unsafe_ptr(), rcptqix);

          qs[rcptqix].enqueue(o, o, domesticate_nop);
          return true;
        });
    }

    std::this_thread::yield();
  } while (myq.can_dequeue() || producers_live || (queue_gate > qix + 1));

  chatty("Px %zu fini\n", qix);

  snmalloc::dealloc(myq.destroy().unsafe_ptr());
  queue_gate--;
}

void producer(const struct params* param, size_t pix)
{
  static constexpr size_t msgsizes[] = {48, 64, 96, 128};
  static constexpr size_t nmsgsizes = sizeof(msgsizes) / sizeof(msgsizes[0]);

  xoroshiro::p128r32 r(5489 + pix, pix);

  freelist::Builder<false> batch;
  batch.init(0, msgqueue_key, msgqueue_key_tweak);

  for (size_t batchix = param->N_PRODUCER_BATCH; batchix > 0; batchix--)
  {
    while (messages_outstanding >= param->N_MAX_OUTSTANDING)
    {
      std::this_thread::yield();
    }

    size_t nmsg = (r.next() & 15) + 1;
    size_t msgsize = msgsizes[r.next() % nmsgsizes];

    /* Allocate batch and form list */
    for (size_t msgix = 0; msgix < nmsg; msgix++)
    {
      auto msg = snmalloc::alloc(msgsize);
      chatty("Pd %zu make %p\n", pix, msg);

      auto msgc = capptr::Alloc<void>::unsafe_from(msg)
                    .template as_reinterpret<freelist::Object::T<>>();
      batch.add(msgc, msgqueue_key, msgqueue_key_tweak);
    }

    /* Post to random queue */
    auto [bfirst, blast] =
      batch.extract_segment(msgqueue_key, msgqueue_key_tweak);
    auto rcptqix = r.next() % param->N_QUEUE;
    param->msgqueue[rcptqix].enqueue(bfirst, blast, domesticate_nop);
    messages_outstanding += nmsg;

    chatty("Pd %zu send %zu to %zu\n", pix, nmsg, rcptqix);

    /* Occasionally yield the CPU */
    if ((batchix & 0xF) == 1)
      std::this_thread::yield();
  }

  chatty("Pd %zu fini\n", pix);
}

int main(int argc, char** argv)
{
  struct params param;

  opt::Opt opt(argc, argv);
  param.N_PRODUCER = opt.is<size_t>("--producers", 3);
  param.N_CONSUMER = opt.is<size_t>("--consumers", 3);
  param.N_PROXY = opt.is<size_t>("--proxies", 2);
  param.N_PRODUCER_BATCH = opt.is<size_t>("--batches", 1024 * 1024);
  param.N_MAX_OUTSTANDING = opt.is<size_t>("--max-out", 4 * 1024);
  param.N_MAX_BATCH_SIZE = opt.is<size_t>("--max-batch", 16);

  std::cout << "msgpass --producers=" << param.N_PRODUCER
            << " --consumers=" << param.N_CONSUMER
            << " --proxies=" << param.N_PROXY
            << " --batches=" << param.N_PRODUCER_BATCH
            << " --max-out=" << param.N_MAX_OUTSTANDING
            << " --max-batch=" << param.N_MAX_BATCH_SIZE << std::endl;

  param.N_QUEUE = param.N_CONSUMER + param.N_PROXY;
  param.msgqueue =
    new FreeListMPSCQ<msgqueue_key, msgqueue_key_tweak>[param.N_QUEUE];

  auto* producer_threads = new std::thread[param.N_PRODUCER];
  auto* queue_threads = new std::thread[param.N_QUEUE];

  for (size_t i = 0; i < param.N_QUEUE; i++)
  {
    param.msgqueue[i].init();
  }

  producers_live = true;
  queue_gate = param.N_QUEUE;
  messages_outstanding = 0;

  /* Spawn consumers */
  for (size_t i = 0; i < param.N_CONSUMER; i++)
  {
    queue_threads[i] = std::thread(consumer, &param, i);
  }

  /* Spawn proxies */
  for (size_t i = param.N_CONSUMER; i < param.N_QUEUE; i++)
  {
    queue_threads[i] = std::thread(::proxy, &param, i);
  }

  /* Spawn producers */
  for (size_t i = 0; i < param.N_PRODUCER; i++)
  {
    producer_threads[i] = std::thread(producer, &param, i);
  }

  /* Wait for producers to finish */
  for (size_t i = 0; i < param.N_PRODUCER; i++)
  {
    producer_threads[i].join();
  }
  producers_live = false;

  /* Wait for proxies and consumers to finish */
  for (size_t i = 0; i < param.N_QUEUE; i++)
  {
    queue_threads[param.N_QUEUE - 1 - i].join();
  }

  delete[] producer_threads;
  delete[] queue_threads;

  /* Ensure that we have not lost any allocations */
  debug_check_empty<snmalloc::Alloc::Config>();

  return 0;
}
