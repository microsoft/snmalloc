/**
 * Tests that snmalloc's probabilistic mitigations detect several
 * classes of memory-safety violation:
 *
 *   - double-free of a small allocation (local-thread path),
 *   - use-after-free that corrupts the intra-slab free list,
 *   - out-of-bounds writes that spill into a freed neighbour,
 *   - double-free crossing thread boundaries (the second free goes
 *     down the remote-message-queue path),
 *   - use-after-free of a slot that has been freed remotely (i.e.
 *     written through the dangling pointer while the slot sits on
 *     the owning allocator's pending-remote queue),
 *   - double-free of a large allocation that does not fit in any
 *     small sizeclass and is therefore handled by the chunk
 *     allocator and metadata path rather than the slab free list.
 *
 * snmalloc detects free-list corruption by checking the integrity
 * of the obfuscated forward and backward edges of the intra-slab
 * free list when the list is later consumed (allocated from), and
 * detects double-free of large allocations via the
 * `is_backend_owned()` check on the per-chunk metadata in
 * `dealloc_remote` (gated on the `sanity_checks` mitigation).
 * Detection is therefore probabilistic per round, but deterministic
 * at the scale used here: each scenario performs many rounds across
 * many slabs, and at least one of them is overwhelmingly likely to
 * traverse the corrupted edge or hit the metadata check before the
 * test would otherwise complete.
 *
 * Each scenario runs in a forked child so that the expected abort
 * does not kill the test harness. Detection is reported as
 * `WIFSIGNALED && WTERMSIG ∈ {SIGABRT, SIGSEGV, SIGBUS, SIGILL}`;
 * a clean exit means the corruption was *not* detected and the test
 * fails.
 *
 * The test is Linux-only (uses `fork()`/`waitpid()`). It is a no-op
 * when `SNMALLOC_CHECK_CLIENT` is not defined, because none of the
 * mitigations these tests rely on are compiled in.
 */

#include <snmalloc/snmalloc_core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>
#include <thread>

#if defined(__linux__)
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>

// Forward declarations of clang's source-based-coverage runtime
// entry points. Declared as weak symbols so the test still links
// against builds without `-fprofile-instr-generate -fcoverage-mapping`.
// Gated to Linux because (a) the entire fork-based test harness is
// Linux-only, and (b) `__attribute__((weak))` is not portable to
// MSVC and there is no equivalent `SNMALLOC_WEAK` macro.
//
// `__llvm_profile_set_filename` is needed because the LLVM profile
// runtime resolves `%p` in `LLVM_PROFILE_FILE` exactly once at
// startup. Forked children inherit the parent's resolved filename
// and so all write to the same file, overwriting each other. Each
// child has to set its own filename (with its own pid) before
// calling `__llvm_profile_write_file`.
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));
extern "C" void __llvm_profile_set_filename(const char*) __attribute__((weak));
#endif

using namespace snmalloc;

#if defined(__linux__)
namespace
{
  // Per-scenario knobs. ROUNDS amplifies the per-round detection
  // probability; N is the number of objects allocated per round; SIZE
  // picks a small sizeclass so a few KiB of slab is exercised per
  // round.
  constexpr size_t ROUNDS = 1024;
  constexpr size_t N = 64;
  constexpr size_t SMALL_SIZE = 32;
  constexpr size_t TINY_SIZE = 16;
  // Cross-thread scenarios use fewer rounds; each round pays a
  // thread-create/join cost and we still need only one detection.
  constexpr size_t REMOTE_ROUNDS = 64;
  // A size that is guaranteed to fall outside every small sizeclass
  // and therefore exercises the chunk-allocator/metadata dealloc
  // path rather than the slab free list. Using `MAX_SMALL_SIZECLASS_SIZE`
  // directly would still produce a small allocation (it is the upper
  // bound, inclusive), so use twice that.
  constexpr size_t LARGE_SIZE = MAX_SMALL_SIZECLASS_SIZE * 2;

  void try_double_free()
  {
    for (size_t r = 0; r < ROUNDS; r++)
    {
      void* ps[N];
      for (size_t i = 0; i < N; i++)
        ps[i] = snmalloc::alloc(SMALL_SIZE);

      // Double-free a single slot. With sanity_checks, the second
      // dealloc may fire immediately. With freelist_backward_edge
      // alone, the resulting cycle in the doubly-linked free list is
      // detected when the list is later traversed.
      snmalloc::dealloc(ps[N / 2]);
      snmalloc::dealloc(ps[N / 2]);

      // Free the rest (skipping the double-freed slot to avoid
      // freeing an unrelated live allocation that happens to have
      // been handed out from the same address) and reallocate to
      // drive freelist consumption.
      for (size_t i = 0; i < N; i++)
        if (i != N / 2)
          snmalloc::dealloc(ps[i]);

      for (size_t i = 0; i < N; i++)
        ps[i] = snmalloc::alloc(SMALL_SIZE);
      for (size_t i = 0; i < N; i++)
        snmalloc::dealloc(ps[i]);
    }
  }

  void try_uaf_freelist_corruption()
  {
    for (size_t r = 0; r < ROUNDS; r++)
    {
      void* ps[N];
      for (size_t i = 0; i < N; i++)
        ps[i] = snmalloc::alloc(SMALL_SIZE);
      for (size_t i = 0; i < N; i++)
        snmalloc::dealloc(ps[i]);

      // UAF: write into a freed slot. The first two pointer-sized
      // words of a freed slot hold the obfuscated forward edge (and,
      // with freelist_backward_edge enabled, a backward edge).
      // Either de-obfuscation produces a wild pointer that fails
      // domestication, or the doubly-linked invariant breaks.
      // Use literals that fit in 32-bit uintptr_t too, so MSVC
      // doesn't warn about narrowing on 32-bit Windows builds.
      auto* victim = static_cast<uintptr_t*>(ps[N / 2]);
      victim[0] = static_cast<uintptr_t>(0xDEADBEEFu);
      victim[1] = static_cast<uintptr_t>(0xBADC0FFEu);

      // Drive the freelist by reallocating from the same sizeclass.
      void* qs[N];
      for (size_t i = 0; i < N; i++)
        qs[i] = snmalloc::alloc(SMALL_SIZE);
      for (size_t i = 0; i < N; i++)
        snmalloc::dealloc(qs[i]);
    }
  }

  // Free `p` from a freshly created thread, so the dealloc takes the
  // remote-message-queue path rather than the local-freelist path.
  // The thread is joined before returning, so `p` has definitely
  // been handed off to the owning allocator's pending-remote queue
  // (or already drained from it) by the time we return.
  void remote_dealloc(void* p)
  {
    std::thread t([p]() { snmalloc::dealloc(p); });
    t.join();
  }

  void try_remote_double_free()
  {
    for (size_t r = 0; r < REMOTE_ROUNDS; r++)
    {
      void* ps[N];
      for (size_t i = 0; i < N; i++)
        ps[i] = snmalloc::alloc(SMALL_SIZE);

      // First free is local (this thread allocated). Second free is
      // from a different thread, so it goes through the remote
      // message queue and ends up being inserted onto the owning
      // allocator's free list a second time. The resulting cycle is
      // detected on the next traversal.
      void* victim = ps[N / 2];
      snmalloc::dealloc(victim);
      remote_dealloc(victim);

      for (size_t i = 0; i < N; i++)
        if (i != N / 2)
          snmalloc::dealloc(ps[i]);
      // Drive freelist consumption so the corruption is observed.
      for (size_t i = 0; i < N; i++)
        ps[i] = snmalloc::alloc(SMALL_SIZE);
      for (size_t i = 0; i < N; i++)
        snmalloc::dealloc(ps[i]);
    }
  }

  void try_remote_uaf()
  {
    for (size_t r = 0; r < REMOTE_ROUNDS; r++)
    {
      void* ps[N];
      for (size_t i = 0; i < N; i++)
        ps[i] = snmalloc::alloc(SMALL_SIZE);

      // Free everything via a different thread. The slots travel
      // through the remote message queue back to this allocator and
      // end up on its free list. While in flight (or once parked on
      // the free list) the obfuscated next/prev fields live in the
      // first words of the slot.
      for (size_t i = 0; i < N; i++)
        remote_dealloc(ps[i]);

      // UAF write through the now-dangling pointer. This corrupts
      // the freelist node that the owning allocator will traverse
      // when it next allocates from this slab.
      auto* victim = static_cast<uintptr_t*>(ps[N / 2]);
      victim[0] = static_cast<uintptr_t>(0xDEADBEEFu);
      victim[1] = static_cast<uintptr_t>(0xBADC0FFEu);

      void* qs[N];
      for (size_t i = 0; i < N; i++)
        qs[i] = snmalloc::alloc(SMALL_SIZE);
      for (size_t i = 0; i < N; i++)
        snmalloc::dealloc(qs[i]);
    }
  }

  void try_large_double_free()
  {
    // Large allocations bypass the slab free list. The second
    // dealloc reaches `dealloc_remote` with a metaentry that
    // `claim_for_backend()` has marked `is_backend_owned()`, and
    // the `sanity_checks` mitigation flags this directly.
    // One round is normally enough, but loop a few times so a
    // single missed detection still trips on a later round.
    for (size_t r = 0; r < 16; r++)
    {
      void* p = snmalloc::alloc(LARGE_SIZE);
      snmalloc::dealloc(p);
      snmalloc::dealloc(p);
    }
  }

  void try_oob_into_neighbor()
  {
    for (size_t r = 0; r < ROUNDS; r++)
    {
      void* ps[N];
      for (size_t i = 0; i < N; i++)
        ps[i] = snmalloc::alloc(TINY_SIZE);

      // Free even-indexed slots so their freelist headers occupy
      // their first bytes.
      for (size_t i = 0; i < N; i += 2)
        snmalloc::dealloc(ps[i]);

      // From an odd (still-allocated) slot, write a generous overrun
      // past its bounds. The exact layout of adjacent slots within a
      // slab is implementation-defined, so we splatter several
      // sizeclass-widths of garbage to ensure we land on at least one
      // freed neighbour's freelist node header regardless of layout.
      auto* p = static_cast<unsigned char*>(ps[1]);
      // Use a volatile write loop rather than memset so the compiler
      // does not emit a -Wstringop-overflow diagnostic on the
      // intentionally out-of-bounds write.
      for (size_t k = TINY_SIZE; k < TINY_SIZE * 4; k++)
        const_cast<volatile unsigned char*>(p)[k] = 0xAB;

      // Free the surviving slots and reallocate to drive freelist
      // traversal; the corrupted neighbour will be encountered.
      for (size_t i = 1; i < N; i += 2)
        snmalloc::dealloc(ps[i]);
      for (size_t i = 0; i < N; i++)
        ps[i] = snmalloc::alloc(TINY_SIZE);
      for (size_t i = 0; i < N; i++)
        snmalloc::dealloc(ps[i]);
    }
  }

  // Signal handler that runs in the forked child when snmalloc's
  // mitigation paths abort/segfault. It flushes coverage data (if the
  // process is instrumented) and then re-raises the signal with its
  // default disposition so the parent observes WIFSIGNALED. Without
  // this the abort kills the child before the LLVM profile runtime
  // gets a chance to write its .profraw, so the detection paths show
  // up as uncovered.
  extern "C" void corruption_signal_handler(int sig)
  {
    if (&__llvm_profile_write_file != nullptr)
      __llvm_profile_write_file();
    signal(sig, SIG_DFL);
    raise(sig);
  }

  // Run `fn` in a forked child and return 0 if the child died with a
  // fatal signal (corruption detected) or 1 otherwise (corruption
  // missed, or unexpected exit).
  int run_in_child(const char* name, void (*fn)())
  {
    pid_t pid = fork();
    if (pid < 0)
    {
      perror("fork");
      return 1;
    }
    if (pid == 0)
    {
      // Re-evaluate the LLVM profile filename so this child's
      // .profraw doesn't collide with its siblings' or its parent's.
      // The parent's `LLVM_PROFILE_FILE` (with `%p`) was resolved at
      // startup using the parent's pid; without resetting it here,
      // every fork+abort writes to the same path. Substitute `%p`
      // with the child's pid explicitly because the profile runtime
      // may have already cached the parent's expansion.
      if (
        &__llvm_profile_set_filename != nullptr &&
        getenv("LLVM_PROFILE_FILE") != nullptr)
      {
        char buf[1024];
        const char* tmpl = getenv("LLVM_PROFILE_FILE");
        size_t out = 0;
        for (size_t i = 0; tmpl[i] != '\0' && out + 16 < sizeof(buf); i++)
        {
          if (tmpl[i] == '%' && tmpl[i + 1] == 'p')
          {
            int n = snprintf(
              buf + out, sizeof(buf) - out, "%d", static_cast<int>(getpid()));
            out += static_cast<size_t>(n);
            i++;
          }
          else
          {
            buf[out++] = tmpl[i];
          }
        }
        buf[out] = '\0';
        __llvm_profile_set_filename(buf);
      }
      // Install a coverage-flushing handler for the signals snmalloc
      // raises on detected corruption. The handler re-raises with
      // default disposition so the parent still sees WIFSIGNALED.
      for (int s : {SIGABRT, SIGSEGV, SIGBUS, SIGILL})
        signal(s, corruption_signal_handler);
      fn();
      // If we get here, none of the mitigations fired across all
      // rounds. The parent will treat a clean exit as a test failure.
      fprintf(stderr, "%s: corruption NOT detected after all rounds\n", name);
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
    {
      int sig = WTERMSIG(status);
      if (sig == SIGABRT || sig == SIGSEGV || sig == SIGBUS || sig == SIGILL)
      {
        printf("%s: detected (signal %d)\n", name, sig);
        return 0;
      }
      fprintf(stderr, "%s: child died with unexpected signal %d\n", name, sig);
      return 1;
    }
    if (WIFEXITED(status))
    {
      fprintf(
        stderr,
        "%s: child exited normally (corruption not detected, exit %d)\n",
        name,
        WEXITSTATUS(status));
      return 1;
    }
    fprintf(stderr, "%s: unexpected child wait status 0x%x\n", name, status);
    return 1;
  }
} // namespace
#endif

int main()
{
  setup();

#if !defined(__linux__)
  printf(
    "Skipping corruption-detection test: requires Linux fork()/waitpid()\n");
  return 0;
#else
  if constexpr (!CHECK_CLIENT)
  {
    printf(
      "Skipping corruption-detection test: SNMALLOC_CHECK_CLIENT off\n");
    return 0;
  }

  int failures = 0;
  failures += run_in_child("double_free", try_double_free);
  failures += run_in_child("uaf_freelist", try_uaf_freelist_corruption);
  failures += run_in_child("oob_into_neighbor", try_oob_into_neighbor);
  failures += run_in_child("remote_double_free", try_remote_double_free);
  failures += run_in_child("remote_uaf", try_remote_uaf);
  failures += run_in_child("large_double_free", try_large_double_free);

  if (failures != 0)
  {
    fprintf(
      stderr,
      "FAILED: %d corruption-detection sub-test(s) reported the corruption "
      "was not caught by the allocator's mitigations.\n",
      failures);
    return 1;
  }
  printf("PASSED\n");
  return 0;
#endif
}
