#pragma once
#include "bounds_checks.h"

namespace snmalloc
{
  /**
   * Copy a single element of a specified size.  Uses a compiler builtin that
   * expands to a single load and store.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH_INLINE void copy_one(void* dst, const void* src)
  {
#if __has_builtin(__builtin_memcpy_inline)
    __builtin_memcpy_inline(dst, src, Size);
#else
    // Define a structure of size `Size` that has alignment 1 and a default
    // copy-assignment operator.  We can then copy the data as this type.  The
    // compiler knows the exact width and so will generate the correct wide
    // instruction for us (clang 10 and gcc 12 both generate movups for the
    // 16-byte version of this when targeting SSE.
    struct Block
    {
      char data[Size];
    };

    auto* d = static_cast<Block*>(dst);
    auto* s = static_cast<const Block*>(src);
    *d = *s;
#endif
  }

  /**
   * Copy a single element where source and destination may overlap.
   * Uses __builtin_memmove which the compiler can optimize to register-width
   * loads/stores while correctly handling overlap.  We cannot use
   * __builtin_memcpy_inline (ASan treats it as memcpy and flags overlap)
   * or struct copy (compiler lowers *d = *s to a memcpy call, same problem).
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH_INLINE void copy_one_move(void* dst, const void* src)
  {
#if __has_builtin(__builtin_memmove)
    __builtin_memmove(dst, src, Size);
#else
    // Fallback: byte-by-byte copy through a temporary buffer to avoid
    // the compiler generating a memcpy call for struct assignment.
    char tmp[Size];
    for (size_t i = 0; i < Size; ++i)
      tmp[i] = static_cast<const char*>(src)[i];
    for (size_t i = 0; i < Size; ++i)
      static_cast<char*>(dst)[i] = tmp[i];
#endif
  }

  /**
   * Copy a block using the specified size.  This copies as many complete
   * chunks of size `Size` as are possible from `len`.
   */
  template<size_t Size, size_t PrefetchOffset = 0>
  SNMALLOC_FAST_PATH_INLINE void
  block_copy(void* dst, const void* src, size_t len)
  {
    for (size_t i = 0; (i + Size) <= len; i += Size)
    {
      copy_one<Size>(pointer_offset(dst, i), pointer_offset(src, i));
    }
  }

  /**
   * Copy a block where source and destination may overlap, using the
   * overlap-safe copy_one_move.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH_INLINE void
  block_copy_move(void* dst, const void* src, size_t len)
  {
    for (size_t i = 0; (i + Size) <= len; i += Size)
    {
      copy_one_move<Size>(pointer_offset(dst, i), pointer_offset(src, i));
    }
  }

  /**
   * Reverse-copy a block using the specified chunk size.  Copies as many
   * complete chunks of `Size` bytes as possible from end to start.
   * After the loop, bytes [0, len % Size) remain uncopied.
   * Uses copy_one_move because source and destination overlap.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH_INLINE void
  block_reverse_copy(void* dst, const void* src, size_t len)
  {
    size_t i = len;
    while (i >= Size)
    {
      i -= Size;
      copy_one_move<Size>(pointer_offset(dst, i), pointer_offset(src, i));
    }
  }

  /**
   * Perform an overlapping copy of the end.  This will copy one (potentially
   * unaligned) `T` from the end of the source to the end of the destination.
   * This may overlap other bits of the copy.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH_INLINE void
  copy_end(void* dst, const void* src, size_t len)
  {
    copy_one<Size>(
      pointer_offset(dst, len - Size), pointer_offset(src, len - Size));
  }

  /**
   * Predicate indicating whether the source and destination are sufficiently
   * aligned to be copied as aligned chunks of `Size` bytes.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH_INLINE bool is_aligned_memcpy(void* dst, const void* src)
  {
    return (pointer_align_down<Size>(const_cast<void*>(src)) == src) &&
      (pointer_align_down<Size>(dst) == dst);
  }

  /**
   * Copy a small size (`Size` bytes) as a sequence of power-of-two-sized loads
   * and stores of decreasing size.  `Word` is the largest size to attempt for a
   * single copy.
   */
  template<size_t Size, size_t Word>
  SNMALLOC_FAST_PATH_INLINE void small_copy(void* dst, const void* src)
  {
    static_assert(bits::is_pow2(Word), "Word size must be a power of two!");
    if constexpr (Size != 0)
    {
      if constexpr (Size >= Word)
      {
        copy_one<Word>(dst, src);
        small_copy<Size - Word, Word>(
          pointer_offset(dst, Word), pointer_offset(src, Word));
      }
      else
      {
        small_copy<Size, Word / 2>(dst, src);
      }
    }
    else
    {
      UNUSED(src);
      UNUSED(dst);
    }
  }

  /**
   * Generate small copies for all sizes up to `Size`, using `WordSize` as the
   * largest size to copy in a single operation.
   */
  template<size_t Size, size_t WordSize = Size>
  SNMALLOC_FAST_PATH_INLINE void
  small_copies(void* dst, const void* src, size_t len)
  {
    if (len == Size)
    {
      small_copy<Size, WordSize>(dst, src);
    }
    if constexpr (Size > 0)
    {
      small_copies<Size - 1, WordSize>(dst, src, len);
    }
  }

  /**
   * If the source and destination are the same displacement away from being
   * aligned on a `BlockSize` boundary, do a small copy to ensure alignment and
   * update `src`, `dst`, and `len` to reflect the remainder that needs
   * copying.
   *
   * Note that this, like memcpy, requires that the source and destination do
   * not overlap.  It unconditionally copies `BlockSize` bytes, so a subsequent
   * copy may not do the right thing.
   */
  template<size_t BlockSize, size_t WordSize>
  SNMALLOC_FAST_PATH_INLINE void
  unaligned_start(void*& dst, const void*& src, size_t& len)
  {
    constexpr size_t block_mask = BlockSize - 1;
    size_t src_addr = static_cast<size_t>(reinterpret_cast<uintptr_t>(src));
    size_t dst_addr = static_cast<size_t>(reinterpret_cast<uintptr_t>(dst));
    size_t src_offset = src_addr & block_mask;
    if ((src_offset > 0) && (src_offset == (dst_addr & block_mask)))
    {
      size_t disp = BlockSize - src_offset;
      small_copies<BlockSize, WordSize>(dst, src, disp);
      src = pointer_offset(src, disp);
      dst = pointer_offset(dst, disp);
      len -= disp;
    }
  }

  /**
   * Default architecture definition.  Provides sane defaults.
   */
  struct GenericArch
  {
    /**
     * The largest register size that we can use for loads and stores.  These
     * types are expected to work for overlapping copies: we can always load
     * them into a register and store them.  Note that this is at the C abstract
     * machine level: the compiler may spill temporaries to the stack, just not
     * to the source or destination object.
     */
    SNMALLOC_UNUSED_FUNCTION
    static constexpr size_t LargestRegisterSize =
      bits::max(sizeof(uint64_t), sizeof(void*));

    /**
     * Hook for architecture-specific optimisations.
     */
    static void* copy(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      // If this is a small size, use a jump table for small sizes.
      if (len <= LargestRegisterSize)
      {
        small_copies<LargestRegisterSize>(dst, src, len);
      }
      // Otherwise do a simple bulk copy loop.
      else
      {
        block_copy<LargestRegisterSize>(dst, src, len);
        copy_end<LargestRegisterSize>(dst, src, len);
      }
      return orig_dst;
    }

    /**
     * Forward copy for overlapping memmove where dst < src.
     * Uses block_copy_move (overlap-safe) without copy_end to avoid
     * re-reading overwritten bytes.
     * Caller guarantees len > 0 and buffers overlap.
     */
    static void* forward_move(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      block_copy_move<LargestRegisterSize>(dst, src, len);
      size_t remainder = len % LargestRegisterSize;
      if (remainder > 0)
      {
        size_t offset = len - remainder;
        block_copy_move<1>(
          pointer_offset(dst, offset), pointer_offset(src, offset), remainder);
      }
      return orig_dst;
    }

    /**
     * Reverse copy for overlapping memmove where dst > src.
     * Caller guarantees len > 0 and dst != src.
     */
    static void* move(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      block_reverse_copy<LargestRegisterSize>(dst, src, len);
      size_t remainder = len % LargestRegisterSize;
      if (remainder > 0)
        block_reverse_copy<1>(dst, src, remainder);
      return orig_dst;
    }
  };

  /**
   * StrictProvenance architectures are prickly about their pointers.  In
   * particular, they may not permit misaligned loads and stores of
   * pointer-sized data, even if they can have non-pointers in their
   * pointer registers.  On the other hand, pointers might be hiding anywhere
   * they are architecturally permitted!
   */
  struct GenericStrictProvenance
  {
    static_assert(bits::is_pow2(sizeof(void*)));
    /*
     * It's not entirely clear what we would do if this were not the case.
     * Best not think too hard about it now.
     */
    static_assert(
      alignof(void*) == sizeof(void*)); // NOLINT(misc-redundant-expression)

    static constexpr size_t LargestRegisterSize = 16;

    static void* copy(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      /*
       * As a function of misalignment relative to pointers, how big do we need
       * to be such that the span could contain an aligned pointer?  We'd need
       * to be big enough to contain the pointer and would need an additional
       * however many bytes it would take to get us up to alignment.  That is,
       * (sizeof(void*) - src_misalign) except in the case that src_misalign is
       * 0, when the answer is 0, which we can get with some bit-twiddling.
       *
       * Below that threshold, just use a jump table to move bytes around.
       */
      if (
        len < sizeof(void*) +
          (static_cast<size_t>(-static_cast<ptrdiff_t>(address_cast(src))) &
           (alignof(void*) - 1)))
      {
        small_copies<2 * sizeof(void*) - 1, LargestRegisterSize>(dst, src, len);
      }
      /*
       * Equally-misaligned segments could be holding pointers internally,
       * assuming they're sufficiently large.  In this case, perform unaligned
       * operations at the top and bottom of the range.  This check also
       * suffices to include the case where both segments are
       * alignof(void*)-aligned.
       */
      else if (
        address_misalignment<alignof(void*)>(address_cast(src)) ==
        address_misalignment<alignof(void*)>(address_cast(dst)))
      {
        /*
         * Find the buffers' ends.  Do this before the unaligned_start so that
         * there are fewer dependencies in the instruction stream; it would be
         * functionally equivalent to do so below.
         */
        auto dep = pointer_offset(dst, len);
        auto sep = pointer_offset(src, len);

        /*
         * Come up to alignof(void*)-alignment using a jump table.  This
         * operation will move no pointers, since it serves to get us up to
         * alignof(void*).  Recall that unaligned_start takes its arguments by
         * reference, so they will be aligned hereafter.
         */
        unaligned_start<alignof(void*), sizeof(long)>(dst, src, len);

        /*
         * Move aligned pointer *pairs* for as long as we can (possibly none).
         * This generates load-pair/store-pair operations where we have them,
         * and should be benign where we don't, looking like just a bit of loop
         * unrolling with two loads and stores.
         */
        {
          struct Ptr2
          {
            void* p[2];
          };

          if (sizeof(Ptr2) <= len)
          {
            auto dp = static_cast<Ptr2*>(dst);
            auto sp = static_cast<const Ptr2*>(src);
            for (size_t i = 0; i <= len - sizeof(Ptr2); i += sizeof(Ptr2))
            {
              *dp++ = *sp++;
            }
          }
        }

        /*
         * After that copy loop, there can be at most one pointer-aligned and
         * -sized region left.  If there is one, copy it.
         */
        len = len & (2 * sizeof(void*) - 1);
        if (sizeof(void*) <= len)
        {
          ptrdiff_t o = -static_cast<ptrdiff_t>(sizeof(void*));
          auto dp =
            pointer_align_down<alignof(void*)>(pointer_offset_signed(dep, o));
          auto sp =
            pointer_align_down<alignof(void*)>(pointer_offset_signed(sep, o));
          *static_cast<void**>(dp) = *static_cast<void* const*>(sp);
        }

        /*
         * There are up to sizeof(void*)-1 bytes left at the end, aligned at
         * alignof(void*).  Figure out where and how many...
         */
        len = len & (sizeof(void*) - 1);
        dst = pointer_align_down<alignof(void*)>(dep);
        src = pointer_align_down<alignof(void*)>(sep);
        /*
         * ... and use a jump table at the end, too.  If we did the copy_end
         * overlapping store backwards trick, we'd risk damaging the capability
         * in the cell behind us.
         */
        small_copies<sizeof(void*), sizeof(long)>(dst, src, len);
      }
      /*
       * Otherwise, we cannot use pointer-width operations because one of
       * the load or store is going to be misaligned and so will trap.
       * So, same dance, but with integer registers only.
       */
      else
      {
        block_copy<LargestRegisterSize>(dst, src, len);
        copy_end<LargestRegisterSize>(dst, src, len);
      }
      return orig_dst;
    }

    /**
     * Forward copy for overlapping memmove where dst < src.
     * Uses the same three-case structure as copy() but avoids copy_end
     * and unaligned_start tricks that re-read already-written bytes.
     * Caller guarantees len > 0 and buffers overlap.
     */
    static void* forward_move(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;

      /* Tiny case: no aligned pointer can fit, byte-by-byte is safe. */
      if (
        len < sizeof(void*) +
          (static_cast<size_t>(-static_cast<ptrdiff_t>(address_cast(src))) &
           (alignof(void*) - 1)))
      {
        block_copy_move<1>(dst, src, len);
      }
      /* Equally-misaligned: use pointer-width forward operations. */
      else if (
        address_misalignment<alignof(void*)>(address_cast(src)) ==
        address_misalignment<alignof(void*)>(address_cast(dst)))
      {
        size_t src_misalign =
          address_misalignment<alignof(void*)>(address_cast(src));
        size_t head_pad =
          src_misalign == 0 ? 0 : (alignof(void*) - src_misalign);
        size_t tail_pad = (len - head_pad) % alignof(void*);
        size_t aligned_len = len - head_pad - tail_pad;

        /* Copy head sub-pointer bytes forward (byte-by-byte). */
        if (head_pad > 0)
          block_copy_move<1>(dst, src, head_pad);

        /* Forward copy aligned middle using pointer-pair operations. */
        if (aligned_len > 0)
        {
          struct Ptr2
          {
            void* p[2];
          };

          void* aligned_dst = pointer_offset(dst, head_pad);
          const void* aligned_src = pointer_offset(src, head_pad);

          /* Forward copy pairs of pointers */
          size_t i = 0;
          for (; i + sizeof(Ptr2) <= aligned_len; i += sizeof(Ptr2))
          {
            auto* dp = static_cast<Ptr2*>(pointer_offset(aligned_dst, i));
            auto* sp = static_cast<const Ptr2*>(pointer_offset(aligned_src, i));
            *dp = *sp;
          }

          /* Handle a remaining single pointer */
          if (i + sizeof(void*) <= aligned_len)
          {
            auto* dp = static_cast<void**>(pointer_offset(aligned_dst, i));
            auto* sp =
              static_cast<void* const*>(pointer_offset(aligned_src, i));
            *dp = *sp;
          }
        }

        /* Copy tail sub-pointer bytes forward (byte-by-byte). */
        if (tail_pad > 0)
        {
          size_t tail_off = len - tail_pad;
          block_copy_move<1>(
            pointer_offset(dst, tail_off),
            pointer_offset(src, tail_off),
            tail_pad);
        }
      }
      /* Differently misaligned: integer-only forward copy is safe. */
      else
      {
        block_copy_move<LargestRegisterSize>(dst, src, len);
        size_t remainder = len % LargestRegisterSize;
        if (remainder > 0)
        {
          size_t offset = len - remainder;
          block_copy_move<1>(
            pointer_offset(dst, offset),
            pointer_offset(src, offset),
            remainder);
        }
      }
      return orig_dst;
    }

    /**
     * Reverse copy for overlapping memmove where dst > src.
     * Caller guarantees len > 0 and dst != src.
     *
     * Three cases, mirroring the forward copy() structure:
     *
     * 1. Tiny: buffer can't contain an aligned pointer, so byte-by-byte
     *    reverse is safe.
     *
     * 2. Equally misaligned: pointers could be at aligned positions.
     *    Use pointer-sized operations on the aligned interior, and
     *    byte-by-byte for the unaligned head/tail.
     *
     * 3. Differently misaligned: no pointer-aligned address in one maps
     *    to a pointer-aligned address in the other, so integer-only
     *    reverse copy is safe.
     */
    static void* move(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;

      /*
       * Tiny case: the buffer cannot contain a pointer-aligned, pointer-
       * sized region, so byte-by-byte reverse is safe.
       */
      if (
        len < sizeof(void*) +
          (static_cast<size_t>(-static_cast<ptrdiff_t>(address_cast(src))) &
           (alignof(void*) - 1)))
      {
        block_reverse_copy<1>(dst, src, len);
      }
      /*
       * Equally-misaligned: pointers could be hiding at aligned positions.
       * Must use pointer-width operations on the aligned interior.
       */
      else if (
        address_misalignment<alignof(void*)>(address_cast(src)) ==
        address_misalignment<alignof(void*)>(address_cast(dst)))
      {
        /*
         * Compute alignment boundaries.  head_pad is the number of bytes
         * before the first aligned position; tail_pad is the number of
         * bytes after the last aligned position.
         */
        size_t src_misalign =
          address_misalignment<alignof(void*)>(address_cast(src));
        size_t head_pad =
          src_misalign == 0 ? 0 : (alignof(void*) - src_misalign);
        size_t tail_pad = (len - head_pad) % alignof(void*);
        size_t aligned_len = len - head_pad - tail_pad;

        /*
         * Reverse copy the tail sub-pointer bytes (byte-by-byte).
         * These are past the last aligned position so no pointers here.
         */
        if (tail_pad > 0)
        {
          size_t tail_off = len - tail_pad;
          block_reverse_copy<1>(
            pointer_offset(dst, tail_off),
            pointer_offset(src, tail_off),
            tail_pad);
        }

        /*
         * Reverse copy the aligned middle using pointer-pair operations
         * to preserve capability tags.
         */
        if (aligned_len > 0)
        {
          struct Ptr2
          {
            void* p[2];
          };

          void* aligned_dst = pointer_offset(dst, head_pad);
          const void* aligned_src = pointer_offset(src, head_pad);

          /* Reverse copy pairs of pointers */
          size_t i = aligned_len;
          while (i >= sizeof(Ptr2))
          {
            i -= sizeof(Ptr2);
            auto* dp = static_cast<Ptr2*>(pointer_offset(aligned_dst, i));
            auto* sp = static_cast<const Ptr2*>(pointer_offset(aligned_src, i));
            *dp = *sp;
          }

          /* Handle a remaining single pointer if odd alignment */
          if (i >= sizeof(void*))
          {
            i -= sizeof(void*);
            auto* dp = static_cast<void**>(pointer_offset(aligned_dst, i));
            auto* sp =
              static_cast<void* const*>(pointer_offset(aligned_src, i));
            *dp = *sp;
          }
        }

        /*
         * Reverse copy the head sub-pointer bytes (byte-by-byte).
         * These are before the first aligned position so no pointers.
         */
        if (head_pad > 0)
        {
          block_reverse_copy<1>(dst, src, head_pad);
        }
      }
      /*
       * Differently misaligned: no pointer-aligned address in src maps to
       * a pointer-aligned address in dst, so integer-only operations are
       * safe (no capability tags to preserve).
       */
      else
      {
        block_reverse_copy<LargestRegisterSize>(dst, src, len);
        size_t remainder = len % LargestRegisterSize;
        if (remainder > 0)
          block_reverse_copy<1>(dst, src, remainder);
      }
      return orig_dst;
    }
  };

#if defined(__x86_64__) || defined(_M_X64)
  /**
   * x86-64 architecture.  Prefers SSE registers for small and medium copies
   * and uses `rep movsb` for large ones.
   */
  struct X86_64Arch
  {
    /**
     * The largest register size that we can use for loads and stores.  These
     * types are expected to work for overlapping copies: we can always load
     * them into a register and store them.  Note that this is at the C abstract
     * machine level: the compiler may spill temporaries to the stack, just not
     * to the source or destination object.
     *
     * We set this to 16 unconditionally for now because using AVX registers
     * imposes stronger alignment requirements that seem to not be a net win.
     */
    static constexpr size_t LargestRegisterSize = 16;

    /**
     * Platform-specific copy hook.  For large copies, use `rep movsb`.
     */
    static inline void* copy(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      // If this is a small size, use a jump table for small sizes, like on the
      // generic architecture case above.
      if (len <= LargestRegisterSize)
      {
        small_copies<LargestRegisterSize>(dst, src, len);
      }

      // The Intel optimisation manual recommends doing this for sizes >256
      // bytes on modern systems and for all sizes on very modern systems.
      // Testing shows that this is somewhat overly optimistic.
      else if (SNMALLOC_UNLIKELY(len >= 512))
      {
        // Align to cache-line boundaries if possible.
        unaligned_start<64, LargestRegisterSize>(dst, src, len);
        // Bulk copy.  This is aggressively optimised on modern x86 cores.
#  ifdef __GNUC__
        asm volatile("rep movsb"
                     : "+S"(src), "+D"(dst), "+c"(len)
                     :
                     : "memory");
#  elif defined(_MSC_VER)
        __movsb(
          static_cast<unsigned char*>(dst),
          static_cast<const unsigned char*>(src),
          len);
#  else
#    error No inline assembly or rep movsb intrinsic for this compiler.
#  endif
      }

      // Otherwise do a simple bulk copy loop.
      else
      {
        block_copy<LargestRegisterSize>(dst, src, len);
        copy_end<LargestRegisterSize>(dst, src, len);
      }
      return orig_dst;
    }

    /**
     * Forward copy for overlapping memmove where dst < src.
     */
    static void* forward_move(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      block_copy_move<LargestRegisterSize>(dst, src, len);
      size_t remainder = len % LargestRegisterSize;
      if (remainder > 0)
      {
        size_t offset = len - remainder;
        block_copy_move<1>(
          pointer_offset(dst, offset), pointer_offset(src, offset), remainder);
      }
      return orig_dst;
    }

    /**
     * Reverse copy for overlapping memmove where dst > src.
     * No rep movsb in reverse (ERMS doesn't support DF=1), so use
     * SSE-width block_reverse_copy.
     */
    static void* move(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      block_reverse_copy<LargestRegisterSize>(dst, src, len);
      size_t remainder = len % LargestRegisterSize;
      if (remainder > 0)
        block_reverse_copy<1>(dst, src, remainder);
      return orig_dst;
    }
  };
#endif

#if defined(__powerpc64__)
  struct PPC64Arch
  {
    /**
     * Modern POWER machines have vector registers
     */
    static constexpr size_t LargestRegisterSize = 16;

    /**
     * For large copies (128 bytes or above), use a copy loop that moves up to
     * 128 bytes at once with pre-loop alignment up to 64 bytes.
     */
    static void* copy(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      if (len < LargestRegisterSize)
      {
        block_copy<1>(dst, src, len);
      }
      else if (SNMALLOC_UNLIKELY(len >= 128))
      {
        // Eight vector operations per loop
        static constexpr size_t block_size = 128;

        // Cache-line align first
        unaligned_start<64, LargestRegisterSize>(dst, src, len);
        block_copy<block_size>(dst, src, len);
        copy_end<block_size>(dst, src, len);
      }
      else
      {
        block_copy<LargestRegisterSize>(dst, src, len);
        copy_end<LargestRegisterSize>(dst, src, len);
      }
      return orig_dst;
    }

    /**
     * Forward copy for overlapping memmove where dst < src.
     */
    static void* forward_move(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      block_copy_move<LargestRegisterSize>(dst, src, len);
      size_t remainder = len % LargestRegisterSize;
      if (remainder > 0)
      {
        size_t offset = len - remainder;
        block_copy_move<1>(
          pointer_offset(dst, offset), pointer_offset(src, offset), remainder);
      }
      return orig_dst;
    }

    /**
     * Reverse copy for overlapping memmove where dst > src.
     */
    static void* move(void* dst, const void* src, size_t len)
    {
      auto orig_dst = dst;
      block_reverse_copy<LargestRegisterSize>(dst, src, len);
      size_t remainder = len % LargestRegisterSize;
      if (remainder > 0)
        block_reverse_copy<1>(dst, src, remainder);
      return orig_dst;
    }
  };
#endif

  using DefaultArch =
#ifdef __x86_64__
    X86_64Arch
#elif defined(__powerpc64__)
    PPC64Arch
#else
    stl::conditional_t<
      aal_supports<StrictProvenance>,
      GenericStrictProvenance,
      GenericArch>
#endif
    ;

  /**
   * Snmalloc checked memcpy.  The `Arch` parameter must provide:
   *
   *  - A `size_t` value `LargestRegisterSize`, describing the largest size to
   *    use for single copies.
   *  - A `copy` function that takes (optionally, references to) the arguments
   *    of `memcpy` and returns `true` if it performs a copy, `false`
   *    otherwise.  This can be used to special-case some or all sizes for a
   *    particular architecture.
   */
  template<
    bool Checked,
    bool ReadsChecked = CheckReads,
    typename Arch = DefaultArch>
  SNMALLOC_FAST_PATH_INLINE void* memcpy(void* dst, const void* src, size_t len)
  {
    return check_bound<(Checked && ReadsChecked)>(
      src, len, "memcpy with source out of bounds of heap allocation", [&]() {
        return check_bound<Checked>(
          dst,
          len,
          "memcpy with destination out of bounds of heap allocation",
          [&]() { return Arch::copy(dst, src, len); });
      });
  }

  /**
   * Snmalloc checked memmove.  Handles overlapping source and destination
   * by selecting forward copy (Arch::copy) or reverse copy (Arch::move)
   * as appropriate.
   */
  template<
    bool Checked,
    bool ReadsChecked = CheckReads,
    typename Arch = DefaultArch>
  SNMALLOC_FAST_PATH_INLINE void*
  memmove(void* dst, const void* src, size_t len)
  {
    if (SNMALLOC_UNLIKELY(len == 0 || dst == src))
      return dst;

    return check_bound<(Checked && ReadsChecked)>(
      src, len, "memmove with source out of bounds of heap allocation", [&]() {
        return check_bound<Checked>(
          dst,
          len,
          "memmove with destination out of bounds of heap allocation",
          [&]() {
            auto dst_addr = address_cast(dst);
            auto src_addr = address_cast(src);
            if (dst_addr > src_addr)
            {
              if ((dst_addr - src_addr) >= len)
                return Arch::copy(dst, src, len); // no overlap
              return Arch::move(dst, src, len); // reverse copy
            }
            // dst_addr < src_addr (dst == src already handled)
            if ((src_addr - dst_addr) >= len)
              return Arch::copy(dst, src, len); // no overlap
            return Arch::forward_move(dst, src, len); // safe forward
          });
      });
  }
} // namespace snmalloc
