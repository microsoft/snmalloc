#include <snmalloc/snmalloc.h>
#include <test/measuretime.h>
#include <test/opt.h>
#include <vector>

using namespace snmalloc;

struct Shape
{
  void* object;
  void* dst;
};

size_t my_random()
{
#ifndef __OpenBSD__
  return (size_t)rand();
#else
  // OpenBSD complains on rand() usage
  // we let it know we purposely want
  // deterministic randomness here
  return (size_t)lrand48();
#endif
}

std::vector<Shape> allocs;

void shape(size_t size)
{
  for (size_t i = 0; i < 1000; i++)
  {
    auto rsize = size * 2;
    auto offset = 0;
    // Uncomment the next two lines to introduce some randomness to the start of
    // the memcpys. constexpr size_t alignment = 16; offset = (my_random() %
    // size / alignment) * alignment;
    Shape s;
    s.object = snmalloc::alloc(rsize);
    s.dst = static_cast<unsigned char*>(s.object) + offset;
    // Bring into cache the destination of the copy.
    memset(s.dst, 0xFF, size);
    allocs.push_back(s);
  }
}

void unshape()
{
  for (auto& s : allocs)
  {
    snmalloc::dealloc(s.object);
  }
  allocs.clear();
}

template<typename Memcpy>
void test_memcpy(size_t size, void* src, Memcpy mc)
{
  for (auto& s : allocs)
  {
    auto* dst = static_cast<unsigned char*>(s.dst);
    mc(dst, src, size);
  }
}

template<typename Memcpy>
void test(
  size_t size,
  Memcpy mc,
  std::vector<std::pair<size_t, std::chrono::nanoseconds>>& stats)
{
  auto src = snmalloc::alloc(size);
  shape(size);
  for (size_t i = 0; i < 10; i++)
  {
    MeasureTime m(true);
    test_memcpy(size, src, mc);
    auto time = m.get_time();
    stats.push_back({size, time});
  }
  snmalloc::dealloc(src);
  unshape();
}

NOINLINE
void memcpy_checked(void* dst, const void* src, size_t size)
{
  memcpy<true>(dst, src, size);
}

NOINLINE
void memcpy_unchecked(void* dst, const void* src, size_t size)
{
  memcpy<false>(dst, src, size);
}

NOINLINE
void* memcpy_platform_checked(void* dst, const void* src, size_t size)
{
  return check_bound(
    dst,
    size,
    "memcpy with destination out of bounds of heap allocation",
    [&]() { return memcpy(dst, src, size); });
}

int main(int argc, char** argv)
{
  opt::Opt opt(argc, argv);
  bool full_test = opt.has("--full_test");

  //  size_t size = 0;
  auto mc_platform_checked = [](void* dst, const void* src, size_t len) {
    memcpy_platform_checked(dst, src, len);
  };
  auto mc_sn = [](void* dst, const void* src, size_t len) {
    memcpy_unchecked(dst, src, len);
  };
  auto mc_platform = [](void* dst, const void* src, size_t len) {
    memcpy(dst, src, len);
  };
  auto mc_sn_checked = [](void* dst, const void* src, size_t len) {
    memcpy_checked(dst, src, len);
  };

  std::vector<size_t> sizes;
  for (size_t size = 0; size < 64; size++)
  {
    sizes.push_back(size);
  }
  for (size_t size = 64; size < 256; size += 16)
  {
    sizes.push_back(size);
    sizes.push_back(size + 5);
  }
  for (size_t size = 256; size < 1024; size += 64)
  {
    sizes.push_back(size);
    sizes.push_back(size + 5);
  }
  for (size_t size = 1024; size < 8192; size += 256)
  {
    sizes.push_back(size);
    sizes.push_back(size + 5);
  }
  for (size_t size = 8192; size < bits::one_at_bit(18); size <<= 1)
  {
    sizes.push_back(size);
    sizes.push_back(size + 5);
  }

  std::vector<std::pair<size_t, std::chrono::nanoseconds>> stats_sn,
    stats_sn_checked, stats_platform, stats_platform_checked;

  printf("size, sn, sn-checked, platform, platform-checked\n");

  size_t repeats = full_test ? 80 : 1;

  for (auto repeat = repeats; 0 < repeat; repeat--)
  {
    for (auto copy_size : sizes)
    {
      test(copy_size, mc_platform_checked, stats_platform_checked);
      test(copy_size, mc_sn, stats_sn);
      test(copy_size, mc_platform, stats_platform);
      test(copy_size, mc_sn_checked, stats_sn_checked);
    }
    for (size_t i = 0; i < stats_sn.size(); i++)
    {
      auto& s1 = stats_sn[i];
      auto& s2 = stats_sn_checked[i];
      auto& s3 = stats_platform[i];
      auto& s4 = stats_platform_checked[i];
      std::cout << s1.first << ", " << s1.second.count() << ", "
                << s2.second.count() << ", " << s3.second.count() << ", "
                << s4.second.count() << std::endl;
    }
    stats_sn.clear();
    stats_sn_checked.clear();
    stats_platform.clear();
    stats_platform_checked.clear();
  }
  return 0;
}
