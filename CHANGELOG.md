## Changelog

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
