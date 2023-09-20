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

constexpr static bool use_malloc = false;
constexpr static bool be_chatty = false;

#include <chrono>
#include <iomanip>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <stdarg.h>
#include <thread>
#include <vector>

constexpr static size_t N_PRODUCER = 3;
constexpr static size_t N_CONSUMER = 3;
constexpr static size_t N_PROXY = 2;
constexpr static size_t N_QUEUE = N_CONSUMER + N_PROXY;

constexpr static size_t N_PRODUCER_BATCH = 256 * 1024;
constexpr static size_t N_MAX_OUTSTANDING = 4 * 1024;

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
FreeListMPSCQ<msgqueue_key, msgqueue_key_tweak> msgqueue[N_QUEUE];

std::atomic<bool> producers_live;
std::atomic<size_t> queue_gate;
std::atomic<size_t> messages_outstanding;

freelist::HeadPtr domesticate_nop(freelist::QueuePtr p)
{
  return freelist::HeadPtr::unsafe_from(p.unsafe_ptr());
};

void consumer(size_t qix)
{
  auto& a = ThreadAlloc::get();
  auto& myq = msgqueue[qix];

  chatty("Cl %zu q is %p\n", qix, &myq);

  do
  {
    size_t reap = 0;

    if (myq.can_dequeue(domesticate_nop, domesticate_nop))
    {
      myq.dequeue(
        domesticate_nop,
        domesticate_nop,
        [qix, &a, &reap](freelist::HeadPtr o) {
          UNUSED(qix);
          auto p = o.as_void().unsafe_ptr();
          chatty("Cl %zu free %p\n", qix, p);
          (use_malloc ? free(p) : a.dealloc(p));
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

  } while (myq.can_dequeue(domesticate_nop, domesticate_nop) ||
           producers_live || (queue_gate > N_CONSUMER));

  chatty("Cl %zu fini\n", qix);
  a.dealloc(myq.destroy().unsafe_ptr());
}

void proxy(size_t qix)
{
  auto& myq = msgqueue[qix];

  chatty("Px %zu q is %p\n", qix, &myq);

  xoroshiro::p128r32 r(1234 + qix, qix);
  do
  {
    if (myq.can_dequeue(domesticate_nop, domesticate_nop))
    {
      myq.dequeue(
        domesticate_nop, domesticate_nop, [qix, &r](freelist::HeadPtr o) {
          auto rcptqix = r.next() % qix;

          chatty(
            "Px %zu send %p to %zu\n", qix, o.as_void().unsafe_ptr(), rcptqix);

          msgqueue[rcptqix].enqueue(o, o, domesticate_nop);
          return true;
        });
    }

    std::this_thread::yield();
  } while (myq.can_dequeue(domesticate_nop, domesticate_nop) ||
           producers_live || (queue_gate > qix + 1));

  chatty("Px %zu fini\n", qix);

  ThreadAlloc::get().dealloc(myq.destroy().unsafe_ptr());
  queue_gate--;
}

void producer(size_t pix)
{
  auto& a = ThreadAlloc::get();
  static constexpr size_t msgsizes[] = {48, 64, 96, 128};
  static constexpr size_t nmsgsizes = sizeof(msgsizes) / sizeof(msgsizes[0]);

  xoroshiro::p128r32 r(5489 + pix, pix);

  freelist::Builder<false> batch;
  batch.init(0, msgqueue_key, msgqueue_key_tweak);

  for (size_t batchix = N_PRODUCER_BATCH; batchix > 0; batchix--)
  {
    while (messages_outstanding >= N_MAX_OUTSTANDING)
    {
      std::this_thread::yield();
    }

    size_t nmsg = (r.next() & 15) + 1;
    size_t msgsize = msgsizes[r.next() % nmsgsizes];

    /* Allocate batch and form list */
    for (size_t msgix = 0; msgix < nmsg; msgix++)
    {
      auto msg = (use_malloc ? malloc(msgsize) : a.alloc(msgsize));
      chatty("Pd %zu make %p\n", pix, msg);

      auto msgc = capptr::Alloc<void>::unsafe_from(msg)
                    .template as_reinterpret<freelist::Object::T<>>();
      batch.add(msgc, msgqueue_key, msgqueue_key_tweak);
    }

    /* Post to random queue */
    auto [bfirst, blast] =
      batch.extract_segment(msgqueue_key, msgqueue_key_tweak);
    auto rcptqix = r.next() % N_QUEUE;
    msgqueue[rcptqix].enqueue(bfirst, blast, domesticate_nop);
    messages_outstanding += nmsg;

    chatty("Pd %zu send %zu to %zu\n", pix, nmsg, rcptqix);

    /* Occasionally yield the CPU */
    if ((batchix & 0xF) == 1)
      std::this_thread::yield();
  }

  chatty("Pd %zu fini\n", pix);
}

int main()
{
  std::thread producer_threads[N_PRODUCER];
  std::thread queue_threads[N_QUEUE];

  for (size_t i = 0; i < N_QUEUE; i++)
  {
    msgqueue[i].init();
  }

  producers_live = true;
  queue_gate = N_QUEUE;
  messages_outstanding = 0;

  /* Spawn consumers */
  for (size_t i = 0; i < N_CONSUMER; i++)
  {
    queue_threads[i] = std::thread(consumer, i);
  }

  /* Spawn proxies */
  for (size_t i = N_CONSUMER; i < N_QUEUE; i++)
  {
    queue_threads[i] = std::thread(proxy, i);
  }

  /* Spawn producers */
  for (size_t i = 0; i < N_PRODUCER; i++)
  {
    producer_threads[i] = std::thread(producer, i);
  }

  /* Wait for producers to finish */
  for (size_t i = 0; i < N_PRODUCER; i++)
  {
    producer_threads[i].join();
  }
  producers_live = false;

  /* Wait for proxies and consumers to finish */
  for (size_t i = 0; i < N_QUEUE; i++)
  {
    queue_threads[N_QUEUE - 1 - i].join();
  }

  /* Ensure that we have not lost any allocations */
  debug_check_empty<snmalloc::StandardConfig>();

  return 0;
}
