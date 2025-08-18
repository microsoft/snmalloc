FROM ubuntu:24.04

# Pull mimalloc-bench
RUN apt-get update && apt-get install -y --no-install-recommends git gpg ca-certificates python3-numpy
RUN git clone https://github.com/daanx/mimalloc-bench &&\
    cd mimalloc-bench && \
    git reset --hard a4ce904286365c7adfba54f0eea3a2df3fc95bd1

WORKDIR /mimalloc-bench
# Install dependencies
RUN ./build-bench-env.sh packages

# Tidy up apt cache
RUN apt-get clean && rm -rf /var/lib/apt/lists/*

# Build benchmarks
RUN ./build-bench-env.sh bench

RUN ./build-bench-env.sh redis

RUN ./build-bench-env.sh rocksdb \
  && find /mimalloc-bench/extern/rocksdb-8.1.1 -name "*.o" -delete

RUN ./build-bench-env.sh lean \
  && find /mimalloc-bench/extern/lean -name "*.o" -delete

RUN echo "sn /snmalloc/build/libsnmallocshim.so" > /allocs.txt

# Build allocator
RUN mkdir -p /snmalloc
COPY . /snmalloc

RUN mkdir -p /snmalloc/build
WORKDIR /snmalloc/build
RUN cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
RUN ninja libsnmallocshim.so

# Run benchmarks
ARG benchs=allt
ARG repeats=1
WORKDIR /mimalloc-bench/out/bench
RUN ../../bench.sh --external=/allocs.txt $benchs -r=$repeats

WORKDIR /
RUN python3 /mimalloc-bench/scripts/bencher.dev.py /mimalloc-bench/out/bench/benchres.csv