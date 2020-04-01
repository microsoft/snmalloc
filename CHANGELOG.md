## Changelog
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
