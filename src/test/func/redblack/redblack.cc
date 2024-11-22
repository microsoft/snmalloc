#include "test/opt.h"
#include "test/setup.h"
#include "test/usage.h"
#include "test/xoroshiro.h"

#include <algorithm>
#include <iostream>
#include <vector>

#ifndef SNMALLOC_TRACING
#  define SNMALLOC_TRACING
#endif
// Redblack tree needs some libraries with trace enabled.
#include "snmalloc/snmalloc.h"

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
