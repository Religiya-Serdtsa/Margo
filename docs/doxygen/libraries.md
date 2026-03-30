# Margo Runtime/Library Reference

## Standard import mappings

- `std/io` -> `stdio.h`
- `std/mem` -> `string.h`
- `std/string` -> `string.h`
- `std/math` -> `math.h`
- `std/stdlib` -> `stdlib.h`
- `std/time` -> `time.h`
- `std/assert` -> `assert.h`
- `std/errno` -> `errno.h`
- `std/file` -> `stdio.h`

## Built-in helpers

- `print(...)`
- `Scan(...)`, `ScanLine(...)`
- `alloc(...)`, `alloc_and_init(...)`
- file/network helper family from `file/core`, `network/core`

## Runtime headers

- `runtime/include/margo_threads/core.h`
- `runtime/include/margo_process/core.h`
- `runtime/include/margo_matrix/core.h`

각 함수/구조체 상세는 헤더의 Doxygen 엔트리에서 자동 생성됩니다.
