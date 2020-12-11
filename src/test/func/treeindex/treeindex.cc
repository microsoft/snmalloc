#include <ds/bits.h>
#include <ds/treeindex.h>
#include <iostream>
#include <pal/pal.h>
#include <test/setup.h>

void* alloc_block_16bit()
{
  return malloc(65536);
}

void* alloc_block_4bit()
{
  return malloc(16);
}

constexpr size_t LARGE_RANGE = 1ULL << (snmalloc::bits::is64() ? 30 : 24);

constexpr size_t RANGE = 1ULL << 20;
constexpr size_t SUB_RANGE = 1ULL << 18;

using std_tree = snmalloc::TreeIndex<uint8_t, RANGE, alloc_block_16bit, 65536>;
using flat_tree = snmalloc::TreeIndex<uint8_t, RANGE>;
using fine_tree = snmalloc::TreeIndex<uint8_t, RANGE, alloc_block_4bit, 16>;

using std_tree_l =
  snmalloc::TreeIndex<uint8_t, LARGE_RANGE, alloc_block_16bit, 65536>;
using flat_tree_l = snmalloc::TreeIndex<uint8_t, LARGE_RANGE>;
using fine_tree_l =
  snmalloc::TreeIndex<uint8_t, LARGE_RANGE, alloc_block_4bit, 16>;

using std_tree_u64 =
  snmalloc::TreeIndex<uint64_t, RANGE, alloc_block_16bit, 65536>;
using flat_tree_u64 = snmalloc::TreeIndex<uint64_t, RANGE>;
using fine_tree_u64 =
  snmalloc::TreeIndex<uint64_t, RANGE, alloc_block_4bit, 16>;

std_tree tree1{};
std_tree_l tree1_l{};
std_tree_u64 tree1_u64{};

fine_tree tree2{};
fine_tree_l tree2_l{};
fine_tree_u64 tree2_u64{};

// If platforms don't support lazy commit, don't test the flat map.
using flat_tree_test_u64 = std::conditional_t<
  snmalloc::pal_supports<snmalloc::LazyCommit>,
  flat_tree_u64,
  std_tree_u64>;

// If platforms don't support lazy commit, don't test the flat map.
using flat_tree_test = std::conditional_t<
  snmalloc::pal_supports<snmalloc::LazyCommit>,
  flat_tree,
  std_tree>;

using flat_tree_l_test = std::conditional_t<
  snmalloc::pal_supports<snmalloc::LazyCommit>,
  flat_tree_l,
  std_tree_l>;

flat_tree_test tree3{};
flat_tree_l_test tree3_l{};
flat_tree_test_u64 tree3_u64{};

/**
 * Add noinline indirection to ASM can be inspected easily for quality
 */
template<typename T>
NOINLINE uint8_t treeget(T& tree, size_t index)
{
  return (uint8_t)tree.get(index);
}

/**
 * Print the shape of tree for debugging.
 */
template<typename T>
void print_tree_shape(int level = 0)
{
  if constexpr (T::is_leaf)
  {
    UNUSED(level);
    std::cout << "Leaf ";
  }
  else if (level == 0)
  {
    std::cout << "Root ";
  }
  else
  {
    std::cout << "Node ";
  }

  std::cout << "entries: " << T::entries << "  " << (void*)T::original()
            << std::endl;

  if constexpr (!T::is_leaf)
  {
    print_tree_shape<typename T::SubT>(level + 1);
  }
}

/**
 * Run a few simple patterns through the tree.
 */
template<typename T>
void test(T& tree)
{
  tree.initial_invariant();

  print_tree_shape<T>(0);

  // Check can read whole range
  for (size_t i = 0; i < RANGE; i++)
  {
    if (treeget(tree, i) != 0)
    {
      abort();
    }
  }

  for (size_t i = 0; i < SUB_RANGE; i++)
  {
    tree.set(i, 1);
    if (tree.get(i + 1) != 0)
    {
      abort();
    }
  }

  for (size_t i = 0; i < RANGE; i++)
  {
    if (tree.get(i) != ((i < SUB_RANGE) ? 1 : 0))
    {
      abort();
    }
  }

  for (size_t i = 0; i < SUB_RANGE; i += 2)
  {
    tree.set(i, 0);
  }

  for (size_t i = 0; i < RANGE; i++)
  {
    if (tree.get(i) != (i < (SUB_RANGE)) && ((i % 2) == 1) ? 1 : 0)
    {
      abort();
    }
  }
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  test(tree1);
  test(tree1_l);
  test(tree1_u64);
  test(tree2);
  test(tree2_l);
  test(tree2_u64);

  if constexpr (snmalloc::pal_supports<snmalloc::LazyCommit>)
  {
    test(tree3);
    test(tree3_l);
    test(tree3_u64);
  }
}
