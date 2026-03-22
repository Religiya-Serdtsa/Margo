# Margo Tutorials

각 튜토리얼은 `Makefile`과 `example.margo`를 포함하며, `make`를 실행하면 동일한 예제를 바로 빌드/실행할 수 있다. 루트에서 `make`로 `build/margo`를 만든 뒤 아래 경로로 이동해 연습하면 된다.

| 순번 | 디렉터리 | 주요 주제 |
| --- | --- | --- |
| 01 | `tutorials/01_getting_started` | `@import`, `fn start`, `print`로 기본 실행 흐름 구성 |
| 02 | `tutorials/02_types_and_strings` | `string`, `auto`, `weird(T, n)`와 같은 타입 편의 기능 |
| 03 | `tutorials/03_control_flow` | 축약 `for`/`while`/`if` 문법과 증감 패턴 |
| 04 | `tutorials/04_memory_management` | `alloc`, `alloc_and_init`를 통한 메모리 확보와 점검 |
| 05 | `tutorials/05_std_io` | `@import std/io` 기반의 `Scan`, `ScanLine`, `print` 고급 옵션 |
| 06 | `tutorials/06_styles_and_decorators` | `@style`, `@set`, 함수 데코레이터 구문 |

필요 시 각 예제를 복사해 자신의 `.margo` 프로젝트의 출발점으로 활용하면 된다.
