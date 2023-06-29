## Changelog

### 0.3.4
- Tracking upstream to match version 0.6.2

### 0.3.3
- Tracking upstream to fix Linux PAL typo.

### 0.3.2
- Tracking upstream to enable old Linux variants.

### 0.3.1 
- Fixes `build_cc` feature (broken in 0.3.0 release).
- Fixes `native-cpu` feature (broken in 0.3.0 release).

### 0.3.0 
- Release to follow upstream 0.6.0
- **upstream** Major redesign of the code to improve performance and 
  enable a mode that provides strong checks against corruption.

### 0.3.0-beta.1

- Beta release to support snmalloc 2

### 0.2.28
- Deprecation of `cache-friendly`
- Use exposed `alloc_zeroed` from `snmalloc`
- **upstream** changes of remote communication, corruption detection and compilation flag detection.

### 0.2.27

- Reduction of libc dependency
- **upstream** Windows 7 and windows 8 compatibility added
- **upstream** Option to use C++20 standards if available
- **upstream** Preparations of cherification (heavy refactors of the structure)
- **upstream** Cold routine annotations

### 0.2.26

- **upstream** Building adjustment
- option of cc crate as build feature, only c compiler needed, no cmake required
- Addition of dynamic local TLS option

### 0.2.25

- **upstream** Apple M1 support
- **upstream** Building adjust            
- non-allocation tracking functions 

### 0.2.24

- **upstream** update to use a more efficient power of 2 check
- fix msvc support w/ crt-static

### 0.2.23

- **upstream** fix external pagemap usage

### 0.2.22

- **upstream** avoid amplification when routing
- **upstream** remotely store sizeclass
- **upstream** limit flat pagemap size
- **upstream** limit medium slab header
- **upstream** solaris support fix

### 0.2.21

- **upstream** bug fix for using failing to initialise meta-data

### 0.2.20

- **upstream** pass through Haiku build fix. 
- **upstream** fix typo in macro definition for 16MiB shared library shim.
- **upstream** DragonFly support (userland).
- **upstream** natural alignment for USE_MALLOC
- **upstream** fix bug in pagemap when index has many level
- **upstream** add constexpr annotation to align_up/down.

### 0.2.19

- **upstream** stats
- **upstream** PAL updates and concepts
- **upstream** ddd constexpr annotation to align_up/down
- change macOS CI to follow xcode 12

### 0.2.18

- add msvc flag /EHsc to fix warning C4530

### 0.2.17

- **upstream** add backoff for large reservation
- **upstream** default chunk configuration to 1mib
- add new feature flags

### 0.2.16

- **upstream** New implementation of address space reservation leading to
  - better integration with transparent huge pages; and
  - lower address space requirements and fragmentation.
- Notice MinGW broken state

### 0.2.15

- **upstream** fix VS2019 build
- **upstream** fix wrong realloc behavior and performance issue

### 0.2.14

- **upstream** refactor ptr representation.
- **upstream** improve for more targets and architectures.
- seperate native CPU feature

### 0.2.13

- **upstream** large realloc fix and minor updates

### 0.2.12

- improve mingw support

### 0.2.11

- add android support
- **upstream** support x86
- **upstream** support android
- **upstream** fix callback

### 0.2.10

- follow upstream 0.4.0
- **upstream** defense TLS teardown
- **upstream** adjust GCC warning
- **upstream** other release optimizations

### 0.2.9

- **upstream** fix OpenEnclave
- **upstream** adjust remote batch size (performance improved dramatically, see [benchmark](https://github.com/microsoft/snmalloc/pull/158#issuecomment-605816017)
- **upstream** improve slow path performance for allocation

### 0.2.8

- More CI (**ARM64 on QEMU**)
- **upstream** ARM(32/64) support
- **upstream** x86-SGX support

### 0.2.7

- partially fixed `mingw`
- **upstream** remote dealloc refactor (higher performance)
- **upstream** remove extra assertions

### 0.2.6

- fix `macos`/`freebsd ` support
- add more ci tests
- mark the `mingw` problem
