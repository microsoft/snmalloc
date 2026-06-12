#include "test/opt.h"
#include "test/setup.h"
#include "test/usage.h"
#include "test/xoroshiro.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <vector>

#ifndef SNMALLOC_TRACING
#  define SNMALLOC_TRACING
#endif
// Redblack tree needs some libraries with trace enabled.
#include "test/snmalloc_testlib.h"

#include <snmalloc/ds_core/redblacktree.h>

struct NodeRef
{
  // The redblack tree is going to be used inside the pagemap,
  // and the redblack tree cannot use all the bits.  Applying an offset
  // to the stored value ensures that we have some abstraction over
  // the representation.
  static constexpr size_t offset = 10000;

  size_t* ptr;

  constexpr NodeRef(size_t* p) : ptr(p) {}

  constexpr NodeRef() : ptr(nullptr) {}

  constexpr NodeRef(const NodeRef& other) : ptr(other.ptr) {}

  constexpr NodeRef(NodeRef&& other) : ptr(other.ptr) {}

  bool operator!=(const NodeRef& other) const
  {
    return ptr != other.ptr;
  }

  NodeRef& operator=(const NodeRef& other)
  {
    ptr = other.ptr;
    return *this;
  }

  void set(uint16_t val)
  {
    *ptr = ((size_t(val) + offset) << 1) + (*ptr & 1);
  }

  explicit operator uint16_t()
  {
    return uint16_t((*ptr >> 1) - offset);
  }

  explicit operator size_t*()
  {
    return ptr;
  }
};

// Simple representation that is like the pagemap.
// Bottom bit of left is used to store the colour.
// We shift the fields up to make room for the colour.
struct node
{
  size_t left;
  size_t right;
};

inline static node array[2048];

class Rep
{
public:
  using key = uint16_t;

  static constexpr key null = 0;
  static constexpr size_t root{NodeRef::offset << 1};

  using Handle = NodeRef;
  using Contents = uint16_t;

  static void set(Handle ptr, Contents r)
  {
    ptr.set(r);
  }

  static Contents get(Handle ptr)
  {
    return static_cast<Contents>(ptr);
  }

  static Handle ref(bool direction, key k)
  {
    if (direction)
      return {&array[k].left};
    else
      return {&array[k].right};
  }

  static bool is_red(key k)
  {
    return (array[k].left & 1) == 1;
  }

  static void set_red(key k, bool new_is_red)
  {
    if (new_is_red != is_red(k))
      array[k].left ^= 1;
  }

  static bool compare(key k1, key k2)
  {
    return k1 > k2;
  }

  static bool equal(key k1, key k2)
  {
    return k1 == k2;
  }

  static size_t printable(key k)
  {
    return k;
  }

  static size_t* printable(NodeRef k)
  {
    return static_cast<size_t*>(k);
  }

  static const char* name()
  {
    return "TestRep";
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
        if (tree.insert_elem(Rep::key(index)))
        {
          entries.push_back(Rep::key(index));
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

template<bool TRACE>
void test_neighbours(size_t size, unsigned int seed)
{
  xoroshiro::p64r32 rand(seed);
  snmalloc::RBTree<Rep, true, TRACE> tree;
  std::set<Rep::key> oracle;
  // Parallel vector keeps random-pick on remove O(1) instead of paying
  // O(n) for std::advance over a std::set iterator.
  std::vector<Rep::key> entries;

  auto probe = [&](Rep::key k_probe) {
    auto result = tree.neighbours(k_probe);

    Rep::key expected_pred = Rep::null;
    Rep::key expected_succ = Rep::null;
    auto it = oracle.lower_bound(k_probe);
    if (it != oracle.begin())
    {
      auto prev = it;
      --prev;
      expected_pred = *prev;
    }
    if (it != oracle.end())
      expected_succ = *it;

    if (result.first != expected_pred || result.second != expected_succ)
    {
      std::cout << "neighbours(" << k_probe << ") mismatch:"
                << " got (" << result.first << ", " << result.second << ")"
                << " expected (" << expected_pred << ", " << expected_succ
                << ")" << std::endl;
      abort();
    }
  };

  auto do_probes = [&]() {
    // Boundary probes. Key 0 is Rep::null and is never inserted (insert
    // keys are 1 + rand % size), and size + 1 is one above the maximum
    // possible insert; both are guaranteed not to be in the tree.
    probe(Rep::key(0));
    if (size + 1 <= 0xFFFF)
      probe(Rep::key(size + 1));
    // Two random probes, skipping any that collide with the tree.
    for (size_t p = 0; p < 2; p++)
    {
      Rep::key k = Rep::key(rand.next() % (size + 2));
      if (oracle.count(k) == 0)
        probe(k);
    }
  };

  // Empty tree: every probe must report (null, null).
  do_probes();

  bool first = true;
  for (size_t i = 0; i < 20 * size; i++)
  {
    auto batch = 1 + rand.next() % (3 + (size / 2));
    auto op = rand.next() % 4;
    if (op < 2 || first)
    {
      first = false;
      for (auto j = batch; j > 0; j--)
      {
        auto k = Rep::key(1 + rand.next() % size);
        if (tree.insert_elem(k))
        {
          oracle.insert(k);
          entries.push_back(k);
        }
      }
    }
    else if (op == 3)
    {
      for (auto j = batch; j > 0; j--)
      {
        if (entries.empty())
          break;
        auto index = rand.next() % entries.size();
        Rep::key elem = entries[index];
        if (!tree.remove_elem(elem))
        {
          std::cout << "Failed to remove element: " << elem << std::endl;
          abort();
        }
        entries.erase(entries.begin() + static_cast<int>(index));
        oracle.erase(elem);
      }
    }
    else
    {
      for (auto j = batch; j > 0; j--)
      {
        if (entries.empty())
          break;
        auto min = tree.remove_min();
        Rep::key expected = *oracle.begin();
        if (min != expected)
        {
          std::cout << "remove_min mismatch: tree=" << min
                    << " oracle=" << expected << std::endl;
          abort();
        }
        oracle.erase(oracle.begin());
        entries.erase(
          std::remove(entries.begin(), entries.end(), min), entries.end());
      }
    }

    do_probes();

    if (entries.empty())
      break;
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
        // Run the neighbours oracle on a handful of seeds per size: the
        // full size range gives good tree-shape coverage, the seed cap
        // keeps the extra cost from blowing the per-test time budget.
        if (seed < 5)
          test_neighbours<false>(size, seed);
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
  test_neighbours<true>(size, seed);
  return 0;
}
