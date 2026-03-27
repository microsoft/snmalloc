#include <atomic>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <test/opt.h>
#include <test/setup.h>
#include <unordered_set>
#include <vector>

using namespace snmalloc;

struct Node
{
  Node* next;
};

class Queue
{
  Node* head;
  Node* tail;

  Node* new_node(size_t size)
  {
    auto result = (Node*)snmalloc::alloc(size);
    result->next = nullptr;
    return result;
  }

public:
  Queue()
  {
    head = new_node(1);
    tail = head;
  }

  void add(size_t size)
  {
    tail->next = new_node(size);
    tail = tail->next;
  }

  bool try_remove()
  {
    if (head->next == nullptr)
      return false;

    Node* next = head->next;
    snmalloc::dealloc(head);
    head = next;
    return true;
  }
};

std::atomic<uint64_t> global_epoch = 0;

void advance(PalNotificationObject* unused)
{
  UNUSED(unused);
  global_epoch++;
}

PalNotificationObject update_epoch{&advance};

bool has_pressure()
{
  static thread_local uint64_t epoch = 0;

  bool result = epoch != global_epoch;
  epoch = global_epoch;
  return result;
}

void reach_pressure(Queue& allocations)
{
  size_t size = 4096;

  while (!has_pressure())
  {
    allocations.add(size);
    allocations.try_remove();
    allocations.add(size);
    allocations.add(size);
  }
}

void reduce_pressure(Queue& allocations)
{
  size_t size = 4096;
  for (size_t n = 0; n < 10000; n++)
  {
    allocations.try_remove();
    allocations.try_remove();
    allocations.add(size);
  }
}

/**
 * Wrapper to handle Pals that don't have the method.
 * Template parameter required to handle `if constexpr` always evaluating both
 * sides.
 */
template<typename MemoryProvider>
void register_for_pal_notifications()
{
  MemoryProvider::Pal::register_for_low_memory_callback(&update_epoch);
}

int main(int argc, char** argv)
{
  opt::Opt opt(argc, argv);

  // TODO reinstate

  //   if constexpr (pal_supports<LowMemoryNotification, GlobalVirtual::Pal>)
  //   {
  //     register_for_pal_notifications<GlobalVirtual>();
  //   }
  //   else
  //   {
  //     std::cout << "Pal does not support low-memory notification! Test not
  //     run"
  //               << std::endl;
  //     return 0;
  //   }

  // #ifdef NDEBUG
  // #  if defined(WIN32) && !defined(SNMALLOC_VA_BITS_64)
  //   std::cout << "32-bit windows not supported for this test." << std::endl;
  // #  else

  //   bool interactive = opt.has("--interactive");

  //   Queue allocations;

  //   std::cout
  //     << "Expected use:" << std::endl
  //     << "  run first instances with --interactive. Wait for first to print "
  //     << std::endl
  //     << "   'No allocations left. Press any key to terminate'" << std::endl
  //     << "watch working set, and start second instance working set of first "
  //     << "should drop to almost zero," << std::endl
  //     << "and second should climb to physical ram." << std::endl
  //     << std::endl;

  //   setup();

  //   for (size_t i = 0; i < 10; i++)
  //   {
  //     reach_pressure(allocations);
  //     std::cout << "Pressure " << i << std::endl;

  //     reduce_pressure(allocations);
  //   }

  //   // Deallocate everything
  //   while (allocations.try_remove())
  //     ;

  //   if (interactive)
  //   {
  //     std::cout << "No allocations left. Press any key to terminate" <<
  //     std::endl; getchar();
  //   }
  // #  endif
  // #else
  //   std::cout << "Release test only." << std::endl;
  // #endif
  return 0;
}
