FROM ubuntu:24.04

# Pull mimalloc-bench
RUN apt-get update && apt-get install -y --no-install-recommends git gpg ca-certificates
RUN git clone https://github.com/daanx/mimalloc-bench &&\
    cd mimalloc-bench && \
    git reset --hard ffa530dbbe046532dfcb4a1b58ffc06e144aee60

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

RUN apt-get install -y python3 python3-pip
RUN pip3 install --no-cache-dir numpy
RUN python3 /snmalloc/benchmark/bencher.dev.py /mimalloc-bench/out/benchres.csv > results.json