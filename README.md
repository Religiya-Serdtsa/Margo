# 마고 언어

![Margo logo](logo.png)

마고(Margo)는 C ABI에 자연스럽게 연결되면서도 더 빠른 프로토타이핑 경험을 주기 위한 실험적 언어다. `concept.margo` 초기 사양에는 다음과 같은 축이 제시되어 있다.

- **C-Native / Zero-Grief**: C와 완벽히 호환되는 타입 레이아웃과 스코프 기반 자동 해제(RAII) 철학.
- **타입 시스템**: `auto` 타입 추론, `string` 편의 타입, `weird(T, n)` 포인터 축약 구문.
- **제어 흐름**: 암시적 루프 변수, 조건부 증감식 같은 하이브리드 문법.
- **지시자/도구**: `@import`, `@style`, `@set autocorrect` 등 메타 프로그래밍 지시자.

## 현재 리포지터리 구성

| 경로 | 설명 |
| --- | --- |
| `concept.margo` | 사양 초안 문서. |
| `README.md`, `README.en.md` | 국/영문 개요 문서. |
| `docs/spec-outline.md` | 사양의 구현 관점 요약. |
| `TODO.md` | 구현 단계별 작업 목록. |
| `deps/` | 외부 실험용 라이브러리(libttak 등).
| `examples/number_baseball/` | `fn`/`@import`만으로 작성한 첫 실행 예제. |
| `tutorials/` | 주제별 실습 세트. 각 폴더가 문법/함수를 설명하는 Markdown·예제 코드를 포함. |

## 진행 상황

1차 목표는 사양을 문서화하고 구현 단계를 정의하는 것이다. `TODO.md`에 다음 단계가 정리되어 있으며, 완료된 항목에는 `[v]`를 표시한다.

## 셀프 컴파일 최소 지원 (Self-Hosting Minimum)

Margo가 자기 자신을 컴파일하기 위해 필요한 최소 기능들이 구현되었다.

### RAII 스코프 해제 주입
`alloc` / `alloc_and_init`으로 할당된 변수는 블록(`{}`) 종료 시 컴파일러가 자동으로 `free()`를 삽입한다.

```margo
fn process() {
    string buf = alloc_and_init(256, "hello")
    // ... 처리 로직 ...
}  // ← 여기서 free(buf)가 자동 삽입됨
```

`return` 문 전에도 모든 소유 변수에 대해 `free()`가 주입된다. 반환되는 변수(소유권 이전)는 제외.

### `null` 키워드
C의 `NULL`로 직접 변환된다:
```margo
if ptr == null { ... }
```

### `@import godmode`
모든 표준 C 헤더를 한 번에 가져온다. 컴파일러 구현 같은 시스템 프로그램 작성 시 유용하다:
```margo
@import godmode
```

### `@import c++/...` 스텁
C++ 바인딩 지원은 향후 마일스톤이다. 지시자는 주석으로 보존된다.

### 확장된 `@import std/...` 모듈
| 지시자 | 매핑 |
| --- | --- |
| `@import std/io` | `<stdio.h>` |
| `@import std/mem` | `<string.h>` |
| `@import std/string` | `<string.h>` |
| `@import std/math` | `<math.h>` |
| `@import std/stdlib` | `<stdlib.h>` |
| `@import std/time` | `<time.h>` |
| `@import std/assert` | `<assert.h>` |
| `@import std/errno` | `<errno.h>` |
| `@import std/file` | `<stdio.h>` |
| `@import file/core` | `<stdio.h>` + 파일 패턴 검색 헬퍼 |

### 쓰레드/프로세스 런타임 (Experimental)
- `@import threads/core` — `threads_cluster_t`, `threads_mailbox_t`, `_pairs` 조합을 위한 헤더 온리 스케줄러를 노출한다. `pthread` 위에서 동작하며 `threads_spawn`, `threads_spawn_isolate`, `threads_mailbox_*` API를 제공한다.
- `@import process` / `@import process/core` — `process_channel_t`, `process_cluster_t`를 포함한 멀티프로세스 통신 도구. POSIX `fork` + 공유 메모리 + 채널을 묶어 Go 스타일 `send/recv`를 흉내 낸다.
- 두 런타임을 묶어 보여주는 최초의 벤치는 `examples/thread_process_bench`에 포함된다.

### 의미 분석 패스 (`src/sema.c`)
컴파일 전에 토큰 스트림을 분석한다:
- **함수 시그니처 레지스트리**: 모든 `fn` 선언 기록, 호출 위치에서 인자 수 검증.
- **`weird` 차원 검증**: 포인터 차원이 [0, 8] 범위를 벗어나면 오류.
- **RAII 소유권 테이블**: `ident = alloc(...)` 패턴 감지, 트랜스파일러에 전달.
- **다중 오류 누적**: 첫 번째 오류에서 멈추지 않고 모든 오류를 수집해 한꺼번에 출력.

## 빠른 시작 (LLVM 기반 빌드)

`build/margo` 바이너리는 `.margo` 소스에서 지시자(`@import`, `@set` 등)만 얇게 정규화한 뒤, 나머지 코드를 그대로 LLVM `clang` 프런트엔드로 넘겨 IR 혹은 네이티브 바이너리를 만든다. 추가 구문(`fn`, `auto`, `weird`)은 런타임 전처리 매크로로 확장되므로 별도의 C 소스 파일이 생성되지 않는다. 현재 지원되는 축약 문법은 다음과 같다.

- `@import c/<header>` → `#include <header>`
- `fn [return_type] name(args) { ... }` → C 함수 시그니처 (`return_type` 생략 시 `int`)
- `string` 편의 타입 (`typedef char* string;`로 치환)
- `auto` → `__auto_type`, `weird(T, n)` → `T` + `n`개의 `*`

```bash
make  # build/margo-c(bootstrap) + self-compiled build/margo 생성
./build/margo build examples/number_baseball/number_baseball.margo -o number_baseball
./number_baseball
```

`--clang <경로>` 또는 `CLANG=<경로>` 환경변수로 원하는 LLVM 툴체인을 지정할 수 있으며, IR을 확인하고 싶다면 `--emit-llvm number_baseball.ll` 옵션을 추가하면 된다.

### 셀프 컴파일 실험

`experiment/margo_compiler.margo`는 이제 `@style c`로 전체 컴파일러 코어를 포함하므로,
이 파일에서 생성된 바이너리는 bootstrap 실행 파일에 위임하지 않고도 스스로를 다시 빌드할 수 있다.
`make` 기본 타깃은 셀프 컴파일을 포함하며, 아래 명령은 같은 과정을 명시적으로 실행한다.

```bash
make margo-in-margo
```

생성 결과:
- `margo_build/margo_stage1` — `build/margo-c`로 빌드한 1단계
- `margo_build/margo` — `margo_stage1`이 다시 빌드하고 한 번 더 재검증한 최종 단계
- `build/margo` — 기본 `make`가 배치하는 self-compiled 최종 바이너리
- `build/margo-c` — C로 빌드한 bootstrap 컴파일러

## 내장 런타임 헬퍼

- `print(value1, value2, ..., sep = \" \", endl = \"\\n\")`  
  `_Generic` 기반으로 bool/정·부호 정수/부동소수/문자/문자열/포인터를 자동 감지해 출력한다. `sep`/`endl` 인자는 선택적으로 덮어쓸 수 있으며, positional 인자 뒤에 반드시 위치해야 한다.
- `Scan(&int_var)` / `ScanLine(buffer)`  
  `@import std/io`가 포함된 번역 단위에서만 사용할 수 있다. `Scan`은 공백 단위 토큰을 읽고 성공적으로 파싱한 인자 수를 반환하며, `ScanLine`은 개행까지 읽는다. 여러 인자를 넘기면 각각의 타입에 맞는 `%d`, `%f`, `%s` 등이 자동으로 연결된다.
- `alloc(size)` / `alloc_and_init(size, literal)`  
  힙에서 바이트 단위 블록을 확보하고, 필요 시 문자열 리터럴을 복사해 준다. 반환 값은 C의 `void*`와 호환되므로 `string`/`weird` 등을 통해 자유롭게 캐스팅하여 사용하면 된다.
- `seek_from_file(stream, pattern)`, `pos_from_file(stream, pattern)`, `jmp_from_file(stream, pattern)`, `return_all_bitmask_offsets(stream, pattern)`  
  `@import file/core`를 통해 노출되며, `FILE *` 스트림 안에서 바이트 패턴을 검색하거나 (존재 여부/오프셋) 확인하고, 필요 시 `fseek`으로 점프한다. `return_all_bitmask_offsets`는 전체 파일을 한 번 훑어 패턴이 시작되는 모든 위치를 bool 마스크로 돌려주며, `[0x7F, 'E', 'L', 'F']`처럼 정적 바이트 배열 리터럴을 즉시 인자로 넘길 수 있다. 컴파일러가 자동으로 상수 버퍼를 생성하고 `hits.indices()` 같은 도우미도 제공한다.

### 실행형 예제 (숫자 야구)

`examples/number_baseball`은 위 빌드 플로우를 자동화한 Makefile을 포함한다. `fn`, `@import`, `auto`/`weird` 등의 문법만 사용하며 `@style c` 블록이 없다.

```bash
cd examples/number_baseball
make
./number_baseball
```

이 경로는 “순수” `.margo` 파일을 직접 빌드 가능한 첫 사례다.

### 새 문법 실험 (파서/렉서 기반)

`margo` 전면부는 이제 토큰화/파서 단계를 거쳐 추가 문법 설탕을 이해한다. 현재 프로토타입에서는 다음과 같은 변환을 지원한다.

- `@import c/stdio`처럼 `.h` 확장자를 생략한 C 헤더 포함 (자동으로 `.h`를 붙인다).
- `for idx=0; <N; ++ { ... }` 형태의 암시적 루프 문법을 정규 C `for` 헤더로 재작성한다. 초기화식에서 추출한 변수 이름을 조건/증감식의 생략된 식별자 앞에 자동으로 붙여준다.
- `if v == 1 return 100`처럼 괄호를 생략한 조건문은 조건식을 자동으로 `()`로 감싸고, 이후에 이어지는 문장이나 블록 앞의 공백도 그대로 보존한다.
- 함수 앞에 붙는 `@decorator` 토큰은 주석으로 치환되어 C 컴파일러가 이해할 수 있도록 정리된다.
- `mat_find(arr, target, precision) { ... }`와 `mat_for(arr, fn_name)`는 행렬 차수를 직접 명시하지 않아도 전체 배열을 순회하거나 근사값을 찾을 수 있는 고수준 행렬 전용 구문이다. 일치 지점을 찾았을 때만 블록이 실행되며, 복수 차수 인덱싱은 언어가 자동으로 관리하고, 내부적으로는 선형대수학식 평탄화를 이용해 캐시 지역성을 확보한다.
- `mat_neighbor()`는 `mat_for` 안에서 암시적으로 현재 좌표의 반경 1 이웃을, 스코프 밖에서는 `mat_neighbor(arr, i, j, ..., n | seek_fn = fn)` 형태로 임의 반경/필터를 적용해 안전하게 잘린 이웃 뷰를 돌려준다.
- `[0x01, 0x02, ...]` 형태의 바이트 패턴 리터럴은 별도 선언 없이 정적 `uint8_t` 버퍼로 승격되어 파일 패턴 헬퍼나 기타 저수준 API에 즉시 전달할 수 있다.
- `for x in [1, 2, 3, 4] { ... }`처럼 Python 스타일의 리스트 열거 구문을 지원한다. 내부적으로는 숨김 상수 배열을 만든 뒤 기존 `for` 문으로 내린다.

숫자 야구 예제는 위 문법을 사용해 콘셉트 문서와 유사한 스타일을 미리 체험할 수 있다.

### Showcase 예시

```margo
@import file/core
@import std/io

fn dump_zip_offsets(string path) {
    weird(FILE, 1) fp = fopen(path, "rb")
    if !fp return

    bool hits[] = return_all_bitmask_offsets(fp, [0x50, 0x4B, 0x03, 0x04])
    for slot in [0, 32, 64, 96] {
        print("slot ", slot, ": ", hits[slot])
    }
    for pos in hits.indices() {
        if hits[pos] print("central dir? @", pos)
    }
    fclose(fp)
}
```

```margo
fn sum_hotspots(auto_matrix field) {
    mat_find(field, 10, 0.001) {
        print("exact 10 at", mat_index[0], mat_index[1])
    }
    mat_for(field, fn(i, j, cell) bool {
        auto neigh = mat_neighbor()
        auto spikes = mat_neighbor(field, i, j, 2, seek_fn = fn(nc) bool { return nc.value > cell })
        neigh.for_each(fn(nc) {
            if nc.value > cell * 2 print("spike", nc.index[0], nc.index[1])
        })
        if spikes.count == 0 && cell > 0 {
            print("isolated hot cell", i, j)
        }
        return true
    })
}
```

## 튜토리얼 모음

`tutorials/README.md`에는 다음과 같은 실습 폴더가 준비되어 있다. 각 폴더는 `README.md`, `Makefile`, `example.margo`를 제공하므로 루트에서 `make`로 `build/margo`만 만들어두면 바로 따라 할 수 있다.

| 순번 | 디렉터리 | 주요 주제 |
| --- | --- | --- |
| 01 | `tutorials/01_getting_started` | `@import`, `fn start`, `print`로 기본 실행 흐름 구성 |
| 02 | `tutorials/02_types_and_strings` | `string`, `auto`, `weird(T, n)`와 같은 타입 편의 기능 |
| 03 | `tutorials/03_control_flow` | 축약 `for`/`while`/`if` 문법과 증감 패턴 |
| 04 | `tutorials/04_memory_management` | `alloc`, `alloc_and_init`를 통한 메모리 확보와 점검 |
| 05 | `tutorials/05_std_io` | `@import std/io` 기반의 `Scan`, `ScanLine`, `print` 고급 옵션 |
| 06 | `tutorials/06_styles_and_decorators` | `@style`, `@set`, 함수 데코레이터 구문 |

## 기여 가이드 초안

- 모든 새로운 개념은 먼저 문서화(`docs/`) 후 코드에 반영한다.
- 영어 주석을 강제한다.
- LLVM 기반 백엔드를 사용할 계획이므로, IR 설계와 타입/메모리 분석은 이를 염두에 둔다.

자세한 일정과 기능별 요구사항은 `TODO.md`에서 확인할 수 있다.
