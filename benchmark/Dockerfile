FROM ubuntu:24.04

# Pull mimalloc-bench
RUN apt-get update && apt-get install -y --no-install-recommends git gpg ca-certificates
RUN git clone https://github.com/daanx/mimalloc-bench &&\
    cd mimalloc-bench && \
    git reset --hard ffa530dbbe046532dfcb4a1b58ffc06e144aee60

WORKDIR /mimalloc-bench
# Install dependencies
RUN ./build-bench-env.sh packages

# Build benchmarks
RUN ./build-bench-env.sh bench

RUN ./build-bench-env.sh redis

RUN ./build-bench-env.sh rocksdb

RUN ./build-bench-env.sh lean

# Tidy up apt cache
RUN apt-get clean && rm -rf /var/lib/apt/lists/*

# Remove rocksdb .o files to compact image
RUN apt-get install findutils
RUN find /mimalloc-bench/extern/rocksdb-8.1.1 -name "*.o" -delete

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
WORKDIR /mimalloc-bench/out/bench
RUN ../../bench.sh --external=/allocs.txt $benchs

RUN du -m /mimalloc-bench