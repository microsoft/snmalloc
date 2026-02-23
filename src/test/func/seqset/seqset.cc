/**
 * Test that demonstrates how SeqSet::iterate detects cyclic corruption.
 *
 * A double-free of small allocations can corrupt the slab metadata's
 * doubly-linked list node, creating a cycle that would cause iterate()
 * to loop forever.  This test creates that corruption by double-inserting
 * an element into a SeqSet and verifies that the production SNMALLOC_CHECK
 * fires (aborting the process).
 *
 * The corruption case is run in a forked child so that the expected abort
 * does not kill the test harness itself.
 */
#include <snmalloc/snmalloc.h>
#include <test/helpers.h>
#include <test/setup.h>

#ifndef _WIN32
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

using namespace snmalloc;

/**
 * A minimal element that can live in a SeqSet.
 * The `node` field must be the first member (offset 0).
 */
struct Element
{
  SeqSet<Element>::Node node;
  int value;
};

/**
 * Iterate a corrupted SeqSet and expect the process to abort.
 * Called in a forked child process.
 */
void iterate_corrupted_seqset()
{
  // After inserting a, b, c the list is:
  //   head <-> c <-> b <-> a <-> head
  //
  // Inserting b a second time re-splices it at the front:
  //   head->next = &b,  b->next = old head->next (c),  c->next still == &b
  //   This creates a cycle: b <-> c <-> b ...
  //   b->prev = &head,  but c->prev is still &b from the first insert
  //
  // When iterate reaches c it checks c->next->prev == c:
  //   c->next = &b,  b->prev = &head,  &head != &c  → CHECK fires.
  //
  // This mirrors what happens when a double-free causes the same slab
  // metadata to be inserted into the available list twice.
  SeqSet<Element> set;
  Element a{}, b{}, c{};
  a.value = 1;
  b.value = 2;
  c.value = 3;

  set.insert(&a);
  set.insert(&b);
  set.insert(&c);

  // Double-insert b — simulates a double-free reinserting metadata.
  set.insert(&b);

  int count = 0;
  set.iterate([&](Element*) { ++count; });

  // Should never be reached.
  _exit(1);
}

int main()
{
  setup();

  START_TEST("SeqSet cycle detection");

  // ---- Normal operation: insert three elements and iterate safely ----
  {
    SeqSet<Element> set;
    Element a{}, b{}, c{};
    a.value = 1;
    b.value = 2;
    c.value = 3;

    set.insert(&a);
    set.insert(&b);
    set.insert(&c);

    int count = 0;
    set.iterate([&](Element*) { ++count; });
    EXPECT(count == 3, "Expected 3 elements, got {}", count);
  }

  // ---- Double-insert: expect the child to be killed by SIGABRT ----
#ifndef _WIN32
  {
    pid_t pid = fork();
    if (pid < 0)
    {
      // fork() failed; report this as a test failure.
      EXPECT(false, "fork() failed");
    }
    else if (pid == 0)
    {
      // Child — will abort inside iterate().
      iterate_corrupted_seqset();
      // unreachable
    }

    int status = 0;
    waitpid(pid, &status, 0);

    EXPECT(
      WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
      "Expected child to abort (SIGABRT), got status {}",
      status);
  }
#else
  printf("Skipping corruption sub-test on Windows\n");
#endif

  printf("PASSED\n");
  return 0;
}
