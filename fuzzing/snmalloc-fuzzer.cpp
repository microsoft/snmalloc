#include "fuzztest/fuzztest.h"
#include "snmalloc/snmalloc.h"

#include <cstddef>
#include <cstdlib>
#include <execution>
#include <new>
#include <string_view>
#include <vector>

void simple_memcpy(std::vector<char> data)
{
  std::vector<char> dest(data.size());
  snmalloc::memcpy<true>(dest.data(), data.data(), data.size());
  if (data != dest)
    abort();
}

void memcpy_with_align_offset(
  size_t source_alignment,
  size_t source_offset,
  size_t dest_alignment,
  size_t dest_offset,
  std::string data)
{
  source_alignment = 1 << source_alignment;
  dest_alignment = 1 << dest_alignment;
  source_offset = source_offset % source_alignment;
  dest_offset = dest_offset % dest_alignment;
  auto src_ = ::operator new(
    data.size() + source_offset, std::align_val_t{source_alignment});
  auto dst_ =
    ::operator new(data.size() + dest_offset, std::align_val_t{dest_alignment});
  auto src = static_cast<char*>(src_) + source_offset;
  auto dst = static_cast<char*>(dst_) + dest_offset;
  snmalloc::memcpy<true>(src, data.data(), data.size());
  snmalloc::memcpy<true>(dst, src, data.size());
  if (std::string_view(dst, data.size()) != data)
    abort();
  ::operator delete(src_, std::align_val_t{source_alignment});
  ::operator delete(dst_, std::align_val_t{dest_alignment});
}

/*
 * disable memmove tests for now
void simple_memmove(std::vector<char> data)
{
  std::vector<char> dest(data.size());
  snmalloc::memmove<true>(dest.data(), data.data(), data.size());
  if (data != dest)
    abort();
}

void forward_memmove(std::string data, size_t offset)
{
  std::string to_move = data;
  offset = std::min(offset, data.size());
  snmalloc::memmove<true>(
    to_move.data() + offset, to_move.data(), to_move.size() - offset);
  size_t after_move = to_move.size() - offset;
  if (
    std::string_view(data.data(), after_move) !=
    std::string_view(to_move.data() + offset, after_move))
    abort();
}

void backward_memmove(std::string data, size_t offset)
{
  std::string to_move = data;
  offset = std::min(offset, data.size());
  snmalloc::memmove<true>(
    to_move.data(), to_move.data() + offset, to_move.size() - offset);
  size_t after_move = to_move.size() - offset;
  if (
    std::string_view(data.data() + offset, after_move) !=
    std::string_view(to_move.data(), after_move))
    abort();
}
*/

constexpr static size_t size_limit = 16384;

enum class EventKind : unsigned
{
  AllocZero = 0,
  AllocNoZero = 1,
  Free = 2,
  Check = 3,
  ReFill = 4,
};

struct Event
{
  EventKind kind;
  size_t size_or_index;
  char filler;

  Event(std::tuple<unsigned, size_t, char> payload)
  : kind(static_cast<EventKind>(std::get<0>(payload) % 5)),
    size_or_index(std::get<1>(payload)),
    filler(std::get<2>(payload))
  {
    if (kind == EventKind::AllocZero || kind == EventKind::AllocNoZero)
    {
      size_or_index %= size_limit;
    }
  }
};

struct Result
{
  char filler;
  char* ptr;
  size_t size;

  Result(char filler, char* ptr, size_t size) : filler(filler), ptr(ptr), size(size) {}
  Result(Result&& other) noexcept : filler(other.filler), ptr(other.ptr), size(other.size)
  {
    other.ptr = nullptr;
  }
  Result &operator=(Result&& other) noexcept
  {
    if (this != &other)
    {
      filler = other.filler;
      ptr = other.ptr;
      size = other.size;
      other.ptr = nullptr;
    }
    return *this;
  }

  void check()
  {
    auto res = std::reduce(
      std::execution::unseq,
      ptr,
      ptr + size,
      static_cast<unsigned char>(0),
      [&](unsigned char acc, char c) -> unsigned char {
        return acc | static_cast<unsigned char>(c != filler);
      });
    if (res)
      abort();
  }

  ~Result()
  {
    auto alloc = snmalloc::get_scoped_allocator();
    if (ptr)
      alloc->dealloc(ptr);
    ptr = nullptr;
  }
};

void snmalloc_random_walk(
  std::vector<std::tuple<unsigned, size_t, char>> payload)
{
  std::vector<Result> results;
  for (auto& p : payload)
  {
    Event e(p);
    auto scoped = snmalloc::get_scoped_allocator();
    switch (e.kind)
    {
      case EventKind::AllocZero:
      {
        auto ptr =
          static_cast<char*>(scoped->alloc<snmalloc::Zero>(e.size_or_index));
        results.emplace_back(0, ptr, e.size_or_index);
        break;
      }

      case EventKind::AllocNoZero:
      {
        auto ptr =
          static_cast<char*>(scoped->alloc<snmalloc::Uninit>(e.size_or_index));
        std::fill(ptr, ptr + e.size_or_index, e.filler);
        results.emplace_back(e.filler, ptr, e.size_or_index);
        break;
      }

      case EventKind::Free:
      {
        if (results.empty())
          break;
        auto index = e.size_or_index % results.size();
        results.erase(results.begin() + static_cast<ptrdiff_t>(index));
        break;
      }

      case EventKind::Check:
      {
        for (auto& r : results)
          r.check();
        break;
      }

      case EventKind::ReFill:
      {
        if (results.empty())
          break;
        auto index = e.size_or_index % results.size();
        std::fill(
          results[index].ptr,
          results[index].ptr + results[index].size,
          e.filler);
        results[index].filler = e.filler;
        break;
      }
    }
  }
}

FUZZ_TEST(snmalloc_fuzzing, simple_memcpy);
/*
 * disable memmove tests for now
FUZZ_TEST(snmalloc_fuzzing, simple_memmove);
FUZZ_TEST(snmalloc_fuzzing, forward_memmove);
FUZZ_TEST(snmalloc_fuzzing, backward_memmove);
*/
FUZZ_TEST(snmalloc_fuzzing, memcpy_with_align_offset)
  .WithDomains(
    fuzztest::InRange(0, 6),
    fuzztest::Positive<size_t>(),
    fuzztest::InRange(0, 6),
    fuzztest::Positive<size_t>(),
    fuzztest::String());
FUZZ_TEST(snmalloc_fuzzing, snmalloc_random_walk);
