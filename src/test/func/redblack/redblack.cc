#include "test/opt.h"
#include "test/setup.h"
#include "test/usage.h"
#include "test/xoroshiro.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <vector>

#define SNMALLOC_TRACING
// Redblack tree needs some libraries with trace enabled.
#include "ds/redblacktree.h"
#include "snmalloc.h"

struct Wrapper
{
  // The redblack tree is going to be used inside the pagemap,
  // and the redblack tree cannot use all the bits.  Applying an offset
  // to the stored value ensures that we have some abstraction over
  // the representation.
  static constexpr size_t offset = 10000;

  size_t value = offset << 1;
};

// Simple representation that is like the pagemap.
// Bottom bit of left is used to store the colour.
// We shift the fields up to make room for the colour.
struct node
{
  Wrapper left;
  Wrapper right;
};

inline static node array[2048];

class Rep
{
public:
  using key = size_t;

  static constexpr key null = 0;

  using Holder = Wrapper;
  using Contents = size_t;

  static void set(Holder* ptr, Contents r)
  {
    ptr->value = ((r + Wrapper::offset) << 1) + (ptr->value & 1);
  }

  static Contents get(Holder* ptr)
  {
    return (ptr->value >> 1) - Wrapper::offset;
  }

  static Holder& ref(bool direction, key k)
  {
    if (direction)
      return array[k].left;
    else
      return array[k].right;
  }

  static bool is_red(key k)
  {
    return (array[k].left.value & 1) == 1;
  }

  static void set_red(key k, bool new_is_red)
  {
    if (new_is_red != is_red(k))
      array[k].left.value ^= 1;
  }
};

template<bool TRACE>
void test(size_t size, unsigned int seed)
{
  /// Perform a pseudo-random series of
  /// additions and removals from the tree.

  xoroshiro::p64r32 rand(seed);
  snmalloc::RBTree<Rep, true, TRACE> tree;
  std::vector<Rep::key> entries;

  bool first = true;
  std::cout << "size: " << size << " seed: " << seed << std::endl;
  for (size_t i = 0; i < 20 * size; i++)
  {
    auto batch = 1 + rand.next() % (3 + (size / 2));
    auto op = rand.next() % 4;
    if (op < 2 || first)
    {
      first = false;
      for (auto j = batch; j > 0; j--)
      {
        auto index = 1 + rand.next() % size;
        if (tree.insert_elem(index))
        {
          entries.push_back(index);
        }
      }
    }
    else if (op == 3)
    {
      for (auto j = batch; j > 0; j--)
      {
        if (entries.size() == 0)
          continue;
        auto index = rand.next() % entries.size();
        auto elem = entries[index];
        if (!tree.remove_elem(elem))
        {
          std::cout << "Failed to remove element: " << elem << std::endl;
          abort();
        }
        entries.erase(entries.begin() + static_cast<int>(index));
      }
    }
    else
    {
      for (auto j = batch; j > 0; j--)
      {
        //   print();
        auto min = tree.remove_min();
        auto s = entries.size();
        if (min == 0)
          break;

        entries.erase(
          std::remove(entries.begin(), entries.end(), min), entries.end());
        if (s != entries.size() + 1)
        {
          std::cout << "Failed to remove min: " << min << std::endl;
          abort();
        }
      }
    }
    if (entries.size() == 0)
    {
      break;
    }
  }
}

int main(int argc, char** argv)
{
  setup();

  opt::Opt opt(argc, argv);

  auto seed = opt.is<unsigned int>("--seed", 0);
  auto size = opt.is<size_t>("--size", 0);

  if (seed == 0 && size == 0)
  {
    for (size = 1; size <= 300; size = size + 1 + (size >> 3))
      for (seed = 1; seed < 5 + (8 * size); seed++)
      {
        test<false>(size, seed);
      }

    return 0;
  }

  if (seed == 0 || size == 0)
  {
    std::cout << "Set both --seed and --size" << std::endl;
    return 1;
  }

  // Trace particular example
  test<true>(size, seed);
  return 0;
}