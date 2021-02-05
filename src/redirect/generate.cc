#include "snmalloc.h"

#include <fstream>
#include <iostream>

int main(int argc, char* argv[])
{
  if (argc != 2)
  {
    std::cerr << "Call with output file name" << std::endl;
    return 1;
  }

  // open a file in write mode.
  ofstream outfile;
  outfile.open(argv[1]);

  for (size_t size = 1024; size > 0; size -= 16)
  {
    auto sizeclass = snmalloc::size_to_sizeclass(size);
    auto rsize = snmalloc::sizeclass_to_size(sizeclass);
    if (rsize == size)
    {
      outfile << "DEFINE_MALLOC_SIZE(malloc_size_" << size << ", " << size
              << ");" << std::endl;
    }
    else
    {
      outfile << "REDIRECT_MALLOC_SIZE(malloc_size_" << size << ", malloc_size_"
              << rsize << ");" << std::endl;
    }
  }

  outfile.close();
}