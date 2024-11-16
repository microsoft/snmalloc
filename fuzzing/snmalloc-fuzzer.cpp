#include "fuzztest/fuzztest.h"
#include "snmalloc/snmalloc.h"

#include <array>
#include <cstdlib>
#include <new>
#include <string_view>

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

FUZZ_TEST(snmalloc_fuzzing, simple_memcpy);
FUZZ_TEST(snmalloc_fuzzing, simple_memmove);
FUZZ_TEST(snmalloc_fuzzing, forward_memmove);
FUZZ_TEST(snmalloc_fuzzing, backward_memmove);
FUZZ_TEST(snmalloc_fuzzing, memcpy_with_align_offset)
  .WithDomains(
    fuzztest::InRange(0, 6),
    fuzztest::Positive<size_t>(),
    fuzztest::InRange(0, 6),
    fuzztest::Positive<size_t>(),
    fuzztest::String());
