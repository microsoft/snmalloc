#!/usr/bin/env python3
"""
Verify: can we use a SINGLE bitmap with a per-mantissa mask
to do O(1) allocation lookups?

Key idea: lay out the 34 unique servable sets in a flat bitmap.
For each requested size class, compute a starting bit and a mask
that clears at most 1 bit. Then find-first-set gives the answer.
"""

ARENA = 256
B = 2  # intermediate bits

def gen_size_classes(max_size):
    classes = set()
    classes.add(1)
    classes.add(2)
    classes.add(3)
    e = 2
    while True:
        base = 1 << e
        step = 1 << (e - B)
        for m in range(1 << B):
            s = base + m * step
            if s > max_size:
                break
            classes.add(s)
        if base > max_size:
            break
        e += 1
    return sorted(classes)

def natural_align(x):
    if x == 0:
        return 1 << 30
    return x & (-x)

def can_serve(addr, block_size, sizeclass):
    A = natural_align(sizeclass)
    first_aligned = ((addr + A - 1) // A) * A
    return first_aligned + sizeclass <= addr + block_size

def get_exponent_mantissa(s):
    """Return (exponent, mantissa) for size class s with B=2."""
    if s == 1: return (0, 0)
    if s == 2: return (1, 0)
    if s == 3: return (1, 1)
    e = 2
    while True:
        base = 1 << e
        step = 1 << (e - B)
        for m in range(4):
            if base + m * step == s:
                return (e, m)
        e += 1
        if base > s * 2:
            return None

def main():
    size_classes = gen_size_classes(ARENA)
    print(f"Size classes: {size_classes}")
    print(f"Count: {len(size_classes)}\n")

    # ================================================================
    # Step 1: Compute ALL unique servable sets (exhaustive)
    # ================================================================
    all_sets = {}  # frozenset -> list of (addr, size) blocks
    for a in range(ARENA):
        for n in range(1, ARENA - a + 1):
            servable = frozenset(sc for sc in size_classes if can_serve(a, n, sc))
            if servable not in all_sets:
                all_sets[servable] = []
            all_sets[servable].append((a, n))

    # Sort by (|set|, max element)
    sorted_sets = sorted(all_sets.keys(), key=lambda s: (len(s), max(s) if s else 0))
    set_to_idx = {s: i for i, s in enumerate(sorted_sets)}
    
    print(f"Total unique servable sets: {len(sorted_sets)}\n")

    # ================================================================
    # Step 2: Analyze the structure per exponent
    # ================================================================
    print("=" * 80)
    print("STRUCTURE ANALYSIS: which size classes appear at each step?")
    print("=" * 80)
    
    for i, s in enumerate(sorted_sets):
        # What's NEW in this set compared to all strict subsets?
        subsets = [sorted_sets[j] for j in range(i) if sorted_sets[j] < s]
        if subsets:
            biggest_subset = max(subsets, key=len)
            new = sorted(s - biggest_subset)
        else:
            new = sorted(s)
        
        # Which sets is this incomparable with?
        incomparable = []
        for j in range(i):
            other = sorted_sets[j]
            if not (other < s) and not (other > s) and other != s:
                if len(other) == len(s):  # same cardinality, truly incomparable
                    incomparable.append(j)
        
        em_list = [(sc, get_exponent_mantissa(sc)) for sc in new]
        incomp_str = f"  INCOMPARABLE with {incomparable}" if incomparable else ""
        new_str = str(sorted(new))
        print(f"  idx {i:2d}: |{len(s):2d}| +{new_str:30s}{incomp_str}")

    # ================================================================
    # Step 3: Verify the 5-per-exponent structure
    # ================================================================
    print()
    print("=" * 80)
    print("BITMAP LAYOUT: 5 bits per exponent level")
    print("=" * 80)
    
    # For each exponent e >= 2, identify the 5 indices
    max_exp = 0
    for sc in size_classes:
        em = get_exponent_mantissa(sc)
        if em and em[0] > max_exp:
            max_exp = em[0]
    
    # Build mapping: for each exponent e, find:
    # - A-only: first index that adds m=0 but NOT m=1
    # - B-only: first index that adds m=1 but NOT m=0
    # - both: first index that has both m=0 and m=1
    # - +m2: first index that has m=2
    # - +m3: first index that has m=3
    
    print(f"\nPrefix (e=0,1): bits 0-2")
    for i in range(min(3, len(sorted_sets))):
        print(f"  bit {i}: {sorted(sorted_sets[i])}")
    
    bit_pos = 3  # next available bit
    exponent_bits = {}  # e -> {role: bit_position}
    
    for e in range(2, max_exp + 1):
        sizes_at_e = []
        for m in range(4):
            step = 1 << (e - B)
            s = (1 << e) + m * step
            if s <= ARENA:
                sizes_at_e.append((m, s))
        
        if not sizes_at_e:
            continue
        
        m0_size = (1 << e)               # m=0
        m1_size = 5 * (1 << (e - 2))     # m=1
        m2_size = 3 * (1 << (e - 1))     # m=2
        m3_size = 7 * (1 << (e - 2))     # m=3
        
        # Find the 5 indices for this exponent
        roles = {}
        for i, s in enumerate(sorted_sets):
            has_m0 = m0_size in s and m0_size <= ARENA
            has_m1 = m1_size in s and m1_size <= ARENA
            has_m2 = m2_size in s and m2_size <= ARENA
            has_m3 = m3_size in s and m3_size <= ARENA
            
            if has_m0 and not has_m1 and 'A-only' not in roles:
                roles['A-only'] = i
            if has_m1 and not has_m0 and 'B-only' not in roles:
                roles['B-only'] = i
            if has_m0 and has_m1 and not has_m2 and 'both' not in roles:
                roles['both'] = i
            if has_m2 and not has_m3 and '+m2' not in roles:
                roles['+m2'] = i
            if has_m3 and '+m3' not in roles:
                roles['+m3'] = i
        
        exponent_bits[e] = {}
        print(f"\nExponent e={e}: sizes {[s for _, s in sizes_at_e]}")
        for role in ['A-only', 'B-only', 'both', '+m2', '+m3']:
            if role in roles:
                idx = roles[role]
                exponent_bits[e][role] = bit_pos
                print(f"  bit {bit_pos:2d} ({role:7s}): idx {idx:2d} = {sorted(sorted_sets[idx])}")
                bit_pos += 1
            else:
                print(f"  ({role:7s}): not present (size > arena)")
    
    total_bits = bit_pos
    print(f"\nTotal bitmap bits: {total_bits}")

    # ================================================================
    # Step 4: The mask rule
    # ================================================================
    print()
    print("=" * 80)
    print("MASK RULE")
    print("=" * 80)
    print()
    print("For each size class S with exponent e and mantissa m:")
    print("  m=0: start at A-only bit(e).  Mask OUT the B-only bit(e).  [1 bit masked]")  
    print("  m=1: start at B-only bit(e).  No mask needed.             [0 bits masked]")
    print("  m=2: start at +m2 bit(e).     No mask needed.             [0 bits masked]")
    print("  m=3: start at +m3 bit(e).     No mask needed.             [0 bits masked]")
    print()
    print("Only m=0 needs a mask, and it's exactly 1 bit.")

    # ================================================================
    # Step 5: VERIFY the mask rule exhaustively
    # ================================================================
    print()
    print("=" * 80)
    print("EXHAUSTIVE VERIFICATION")
    print("=" * 80)
    
    # For each size class S, compute which indices' servable sets include S
    valid_indices_for = {}  # sizeclass -> set of indices
    for sc in size_classes:
        valid = set()
        for i, s in enumerate(sorted_sets):
            if sc in s:
                valid.add(i)
        valid_indices_for[sc] = valid

    # Build the bitmap assignment: index_i -> bit position
    # We need to map the 34 indices to the bit positions we assigned
    idx_to_bit = {}
    # Prefix bits
    idx_to_bit[0] = 0  # {1}
    idx_to_bit[1] = 1  # {1,2}
    idx_to_bit[2] = 2  # {1,2,3}
    
    for e, roles in exponent_bits.items():
        # Map from role -> index we found
        m0_size = (1 << e)
        m1_size = 5 * (1 << (e - 2))
        m2_size = 3 * (1 << (e - 1))
        m3_size = 7 * (1 << (e - 2))
        
        for i, s in enumerate(sorted_sets):
            has_m0 = m0_size in s and m0_size <= ARENA
            has_m1 = m1_size in s and m1_size <= ARENA
            has_m2 = m2_size in s and m2_size <= ARENA
            has_m3 = m3_size in s and m3_size <= ARENA
            
            if has_m0 and not has_m1 and i not in idx_to_bit:
                if 'A-only' in roles:
                    idx_to_bit[i] = roles['A-only']
            if has_m1 and not has_m0 and i not in idx_to_bit:
                if 'B-only' in roles:
                    idx_to_bit[i] = roles['B-only']
            if has_m0 and has_m1 and not has_m2 and i not in idx_to_bit:
                if 'both' in roles:
                    idx_to_bit[i] = roles['both']
            if has_m2 and not has_m3 and i not in idx_to_bit:
                if '+m2' in roles:
                    idx_to_bit[i] = roles['+m2']
            if has_m3 and i not in idx_to_bit:
                if '+m3' in roles:
                    idx_to_bit[i] = roles['+m3']

    # Check we mapped everything
    unmapped = [i for i in range(len(sorted_sets)) if i not in idx_to_bit]
    if unmapped:
        print(f"  WARNING: unmapped indices: {unmapped}")
    
    bit_to_idx = {v: k for k, v in idx_to_bit.items()}
    
    # Now verify: for each size class, the mask rule correctly identifies
    # all valid bits from the start position upward
    errors = 0
    for sc in size_classes:
        em = get_exponent_mantissa(sc)
        if em is None:
            continue
        e, m = em
        
        valid_bits = set()
        for idx in valid_indices_for[sc]:
            if idx in idx_to_bit:
                valid_bits.add(idx_to_bit[idx])
        
        # Compute start bit and mask
        if e <= 1:
            # Prefix: sizes 1,2,3
            start = {1: 0, 2: 1, 3: 2}[sc]
            mask_clear = set()  # no mask needed for prefix
        else:
            roles = exponent_bits.get(e, {})
            if m == 0:
                start = roles.get('A-only', -1)
                mask_clear = {roles.get('B-only', -1)}
            elif m == 1:
                start = roles.get('B-only', -1)
                mask_clear = set()  # no mask!
            elif m == 2:
                start = roles.get('+m2', -1)
                mask_clear = set()
            elif m == 3:
                start = roles.get('+m3', -1)
                mask_clear = set()
        
        if start == -1:
            continue
        
        # The set of bits we'd search: all bits >= start, except mask_clear
        search_bits = set(range(start, total_bits)) - mask_clear
        
        # Valid bits at or above start
        reachable = valid_bits & search_bits
        
        # Invalid bits that we'd incorrectly hit
        false_positives = search_bits - valid_bits
        # Check: are any false positives below the first valid bit?
        # (We only care about false positives that have a set bit in the bitmap
        #  and appear before a true positive.)
        # Actually, the REAL check: for every bit in search_bits,
        # if it's set in the bitmap, it should be in valid_bits.
        
        # Simpler check: search_bits should be a SUBSET of valid_bits ∪ {bits above all valid}
        # Actually: we need that for any bit b in search_bits where b has a block,
        # it's valid to allocate S from that block.
        # This means: every bit in search_bits must be in valid_bits.
        
        problematic = search_bits - valid_bits
        # These are bits where, if a block exists there, we'd incorrectly try to serve
        # size class S from it. Let's check if any of these are actually reachable.
        
        # For the verification to pass: search_bits ⊆ valid_bits
        # i.e., every bit at or above start (minus masked) should be a valid index for S
        
        if problematic:
            # Find the actual problematic servable set
            for pb in sorted(problematic):
                if pb in bit_to_idx:
                    idx = bit_to_idx[pb]
                    if sc not in sorted_sets[idx]:
                        errors += 1
                        if errors <= 10:
                            print(f"  ERROR: sc={sc} (e={e},m={m}), bit {pb} "
                                  f"(idx {idx}) does NOT contain {sc}")
                            print(f"    set = {sorted(sorted_sets[idx])}")
    
    if errors == 0:
        print("  ALL CHECKS PASSED! Single bitmap + 1-bit mask works correctly.")
    else:
        print(f"\n  {errors} total errors found.")

    # ================================================================
    # Step 6: Show the complete scheme
    # ================================================================
    print()
    print("=" * 80)
    print("COMPLETE ALLOCATION SCHEME")
    print("=" * 80)
    print()
    print(f"Bitmap: {total_bits} bits (one per unique servable set)")
    print()
    print("On FREE(addr, size):")
    print("  1. Maximally coalesce with neighbors")
    print("  2. Compute exact servable set for (addr, coalesced_size)")
    print("  3. Map to bit position, set bit in bitmap, add to free list")
    print()
    print("On ALLOCATE(sizeclass S):")
    print("  1. (e, m) = exponent_mantissa(S)")
    print("  2. start = start_bit[e][m]")
    print("  3. masked_bitmap = bitmap")
    print("     if m == 0: masked_bitmap &= ~(1 << b_only_bit[e])")
    print("  4. result = find_first_set(masked_bitmap >> start)")
    print("  5. Pop block from free list at (start + result)")
    print("  6. Carve S from block, reinsert remainders")
    print()
    
    # Print the lookup tables
    print("START BIT TABLE:")
    print(f"  {'SC':>4s}  {'(e,m)':>6s}  {'start':>5s}  {'mask_bit':>8s}")
    for sc in size_classes:
        em = get_exponent_mantissa(sc)
        if em is None:
            continue
        e, m = em
        if e <= 1:
            start = {1: 0, 2: 1, 3: 2}[sc]
            mask = "—"
        else:
            roles = exponent_bits.get(e, {})
            if m == 0:
                start = roles.get('A-only', -1)
                mask = str(roles.get('B-only', -1))
            elif m == 1:
                start = roles.get('B-only', -1)
                mask = "—"
            elif m == 2:
                start = roles.get('+m2', -1)
                mask = "—"
            elif m == 3:
                start = roles.get('+m3', -1)
                mask = "—"
        print(f"  {sc:>4d}  ({e},{m})   {start:>5d}  {mask:>8s}")

if __name__ == "__main__":
    main()
