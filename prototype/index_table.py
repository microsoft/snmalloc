#!/usr/bin/env python3
"""
Exhaustively test all blocks in a 256-unit arena.
For each block (address, size), determine which size classes it can serve
with natural alignment. Group blocks by their unique servable set.
"""

ARENA = 256
B = 2  # intermediate bits

def gen_size_classes(max_size):
    """Generate all valid size classes up to max_size with B=2."""
    classes = set()
    # e=0: S = 1
    # e=1: S = 2, 3
    # e>=2: S = 2^e + m * 2^(e-2) for m in 0..3
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

def align(x):
    """Natural alignment: largest power of 2 dividing x. align(0) = infinity."""
    if x == 0:
        return 1 << 30  # effectively infinite
    return x & (-x)

def can_serve(addr, block_size, sizeclass):
    """Can block [addr, addr+block_size) serve a naturally-aligned allocation of sizeclass?"""
    A = align(sizeclass)
    # First A-aligned address >= addr
    first_aligned = ((addr + A - 1) // A) * A
    # Need first_aligned + sizeclass <= addr + block_size
    return first_aligned + sizeclass <= addr + block_size

def main():
    size_classes = gen_size_classes(ARENA)
    print(f"Size classes (B={B}, up to {ARENA}): {size_classes}")
    print(f"Count: {len(size_classes)}")
    print()

    # For each (addr, block_size), compute the set of servable size classes
    # Key: frozenset of servable classes -> list of (addr, size) blocks
    groups = {}
    
    for a in range(ARENA):
        for n in range(1, ARENA - a + 1):
            servable = frozenset(
                sc for sc in size_classes if can_serve(a, n, sc)
            )
            if servable not in groups:
                groups[servable] = []
            groups[servable].append((a, n))

    # Sort groups by the minimum block size that achieves this set
    sorted_groups = sorted(groups.items(), key=lambda kv: (len(kv[0]), min(kv[0]) if kv[0] else 0))

    print(f"Total unique servable sets: {len(groups)}")
    print()

    # Now the key question: does the servable set depend only on (block_size, align(addr))?
    # Check this empirically
    print("=" * 80)
    print("CHECKING: does servable set depend only on (block_size, align(addr))?")
    print("=" * 80)
    
    by_size_align = {}
    conflicts = 0
    for a in range(ARENA):
        for n in range(1, ARENA - a + 1):
            servable = frozenset(
                sc for sc in size_classes if can_serve(a, n, sc)
            )
            alpha = min(align(a), ARENA)  # cap alignment
            key = (n, alpha)
            if key not in by_size_align:
                by_size_align[key] = servable
            elif by_size_align[key] != servable:
                conflicts += 1
                if conflicts <= 5:
                    print(f"  CONFLICT: (size={n}, align={alpha})")
                    print(f"    existing: {sorted(by_size_align[key])}")
                    print(f"    new (a={a}): {sorted(servable)}")

    if conflicts == 0:
        print("  NO CONFLICTS! Servable set depends only on (block_size, align(addr)).")
    else:
        print(f"  {conflicts} total conflicts found.")
    print()

    # Now build the flattened index: unique sets indexed by (block_size, align(addr))
    # Group by unique servable set, showing which (size, align) pairs map to it
    print("=" * 80)
    print("FLATTENED INDEX: unique servable sets ordered by inclusion")
    print("=" * 80)
    
    # Collect unique sets from the (size, align) perspective
    unique_sets = {}
    for (n, alpha), servable in sorted(by_size_align.items()):
        if servable not in unique_sets:
            unique_sets[servable] = []
        unique_sets[servable].append((n, alpha))

    # Sort by set size then min element
    sorted_sets = sorted(unique_sets.items(), 
                         key=lambda kv: (len(kv[0]), max(kv[0]) if kv[0] else 0))

    for idx, (servable, pairs) in enumerate(sorted_sets):
        sc_list = sorted(servable)
        # Find the minimum block size across all (n, alpha) pairs
        min_n = min(n for n, _ in pairs)
        max_n = max(n for n, _ in pairs)
        # Show a compact representation of which (size, align) pairs give this set
        print(f"\nIndex {idx}: servable = {sc_list}")
        print(f"  |servable| = {len(servable)}, block sizes [{min_n}..{max_n}]")
        # Show a few representative (n, alpha) pairs
        pairs_sorted = sorted(pairs)
        if len(pairs_sorted) <= 10:
            for n, alpha in pairs_sorted:
                print(f"    (size={n}, align={alpha})")
        else:
            for n, alpha in pairs_sorted[:5]:
                print(f"    (size={n}, align={alpha})")
            print(f"    ... ({len(pairs_sorted)} total pairs)")
            for n, alpha in pairs_sorted[-3:]:
                print(f"    (size={n}, align={alpha})")

    print()
    print("=" * 80)
    print(f"SUMMARY: {len(unique_sets)} unique servable sets (= flat index entries)")
    print("=" * 80)

    # Now show just the index by (block_size, block_align) -> index
    print()
    print("=" * 80)
    print("INDEX TABLE: (block_size, block_align) -> index")
    print("Showing for block sizes 1..64 and aligns 1,2,4,8,16,32,64")
    print("=" * 80)
    
    # Assign index numbers
    set_to_idx = {}
    for idx, (servable, _) in enumerate(sorted_sets):
        set_to_idx[servable] = idx

    # Print header
    aligns = [1, 2, 4, 8, 16, 32, 64, 128, 256]
    print(f"{'size':>6}", end="")
    for alpha in aligns:
        print(f" α={alpha:>3}", end="")
    print("   servable max (at high α)")
    
    for n in range(1, 65):
        print(f"{n:>6}", end="")
        for alpha in aligns:
            key = (n, alpha)
            if key in by_size_align:
                idx = set_to_idx[by_size_align[key]]
                print(f" {idx:>5}", end="")
            else:
                print(f"   {'—':>3}", end="")
        # Show max servable sizeclass for highest alpha
        best_alpha = max(a for a in aligns if (n, a) in by_size_align)
        best_set = by_size_align[(n, best_alpha)]
        print(f"   max_sc={max(best_set) if best_set else 0}")

    # Show how many indexes are actually distinct per alignment tier
    print()
    for alpha in aligns:
        indices_at_tier = set()
        for n in range(1, ARENA + 1):
            key = (n, alpha)
            if key in by_size_align:
                indices_at_tier.add(set_to_idx[by_size_align[key]])
        print(f"  Tier α={alpha:>3}: {len(indices_at_tier)} distinct indexes used")

    # Show the progression: for each block_size n (at max alignment),
    # what's the index and what new size class was unlocked?
    print()
    print("=" * 80)
    print("PROGRESSION at max alignment (block at address 0):")
    print("As block size grows, which size classes become servable?")
    print("=" * 80)
    prev_set = frozenset()
    for n in range(1, 129):
        # At address 0, alignment is infinite, so alpha is huge
        key = (n, min(align(0), ARENA))
        # Actually address 0 won't appear with all sizes. Use highest alpha.
        # Let's compute directly
        servable = frozenset(
            sc for sc in size_classes if n >= sc  # at perfect alignment, just need n >= sc
        )
        if servable != prev_set:
            new = sorted(servable - prev_set)
            print(f"  n={n:>4}: +{new}  (total {len(servable)} classes)")
            prev_set = servable

    print()
    print("=" * 80)
    print("PROGRESSION at alignment 1 (worst case):")
    print("=" * 80)
    prev_set = frozenset()
    for n in range(1, 257):
        key = (n, 1)
        if key not in by_size_align:
            continue
        servable = by_size_align[key]
        if servable != prev_set:
            new = sorted(servable - prev_set)
            print(f"  n={n:>4}: +{new}  (total {len(servable)} classes)")
            prev_set = servable

    print()
    print("=" * 80)
    print("PROGRESSION at alignment 4:")
    print("=" * 80)
    prev_set = frozenset()
    for n in range(1, 257):
        key = (n, 4)
        if key not in by_size_align:
            continue
        servable = by_size_align[key]
        if servable != prev_set:
            new = sorted(servable - prev_set)
            print(f"  n={n:>4}: +{new}  (total {len(servable)} classes)")
            prev_set = servable

if __name__ == "__main__":
    main()
