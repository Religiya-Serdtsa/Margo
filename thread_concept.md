## Threading & Multiprocessing Concept (Draft v0.1)

마고 런타임이 제공할 쓰레드/프로세스 추상화 초안이다. `concept.margo` 스타일에 맞춰 문법, 지시자, 실행 모델을 우선 설계하고, 이후 구현은 `threads/core`, `process/core` 런타임 라이브러리가 제공한다.

---

### 1. Thread Cluster (`threads.th_cluster`)

```margo
@import threads/core

threads.th_cluster {
    _isolate fn worker(int id, char *dat) { ... }
    _pairs fn router(int id, char *dat) shares broker logger { ... }
    _unsafe fn native_passthrough(int id, char *dat) { ... }
}
```

- **네임스페이스:** `threads.<cluster_name>` 블록은 동일한 생성/스케줄링 파라미터를 공유하는 스레드 집합을 정의한다.
- **생성:** `threads.th_cluster.spawn(worker, args...)` 혹은 `threads.th_cluster.worker.spawn(args...)`로 실행한다. 생성 시 `threads/core` runtime이 `pthread` 기반 worker pool을 할당하며, `_pairs` 함수는 내부적으로 최대 3개의 동시 실행 컨텍스트를 예약한다.
- **종료:** `th_cluster.worker.join()` 혹은 `th_cluster.worker.wait(timeout_ms)`을 사용한다. 모든 함수는 `th_cluster.worker.concurrency.join(pool)` 형태의 풀 기반 조인도 허용한다.

#### 1.1 `_isolate` 함수
- 동일 클러스터에서 단독으로 캐시 라인과 힙을 사용해야 하는 작업에 지정한다.
- runtime은 `_isolate` 함수마다 별도 arena를 붙여주고, 실행 중 다른 `_isolate` 작업을 스케줄하지 않는다.
- 파라미터는 read-only 복사본이 주어지며, 공유 데이터는 명시적 RAII 구조(`threads::isolated<T>`)를 통해 주입한다.

#### 1.2 `_pairs` 함수 + `shares`
- `_pairs fn router(...) shares broker logger` 형태에서는 `router`가 시작될 때 `broker`, `logger`가 함께 준비된다.
- `shares` 리스트에 오른 함수를 작은 락프리 메일박스(mailbox)로 연결하여, 동일 인자를 참조하거나 `threads.share<T>(ref)` 핸들을 사용해 전달한다.
- 실행 순서는 `(router -> broker -> logger)` 순환 큐를 이루며, `router`가 완료되면 `broker`, `logger`가 동시에 종료된다. 실패 시 전체 share 그룹이 중단된다.

#### 1.3 `_unsafe` 함수
- `libttak` 대신 원시 `pthread_*` 심볼에 직접 접근하는 낮은 수준 함수다.
- `_unsafe` 함수 내부에서는 RAII 해제가 자동으로 끊기므로, 사용자가 수동으로 자원을 회수해야 한다.
- `threads/core`는 `_unsafe` 함수 호출을 `@warn(thread-unsafe)` 주석과 함께 C 코드로 내린다.

#### 1.4 Decorators
- `@aggressive`는 클러스터 전역 혹은 함수별로 붙여 공격적 인라인, CPU affinity 설정을 허용한다.
- `@cozy`는 동시성 안전성을 최우선으로 하여, 스케줄러가 preemption-safe 구간만 배치하도록 강제한다.

#### 1.5 Diagnostics
- `threads.th_cluster`는 `status()` API를 노출하여 활성 스레드 수, 실패 이력, 누적 대기 시간을 조사할 수 있다.
- 런타임 오류는 `threads/core`가 `panic` 대신 `th_cluster.<fn>.fault(err_code)` 형태의 재시도를 지원한다.

---

### 2. Multiprocessing Cluster (`process.process_cluster`)

```margo
@import process

process.process_cluster {
    process_1(chan int) {
        // ...
        chan <- val
    }
}
```

- **`process.process_cluster`:** POSIX `fork` + shared-memory 채널을 래핑한 구조다.
- 각 `process_n` 정의는 채널과 shared segment를 명시한다. 채널 타입(`chan int`, `chan string`, `chan weird(char, 2)`)은 단일 타입만 허용하며, 버퍼 크기는 `chan[N] T` 구문으로 지정한다.
- 프로세스 간 통신은 Go 스타일 `chan <- val`, `val <- chan` 문법을 사용한다. 런타임은 lock-free ring buffer 위에 `futex`/`eventfd` wake-up을 얹어 busy-wait를 지양한다.

#### 2.1 Channel Semantics
- `chan <- val`: 송신. 블로킹이 기본이며, `chan.try_send(val)`로 논블로킹 전송을 요청할 수 있다.
- `val <- chan`: 수신. `chan.recv()`는 튜플 `(ok, value)`를 반환하여 EOF와 오류를 구분한다.
- `@locked` 데코레이터를 채널 혹은 프로세스에 붙이면 내부적으로 `pthread_mutex` 기반 락으로 강제한다. 기본은 락-프리 지향이다.

#### 2.2 Lifecycle
- `process_cluster.spawn(process_1, { .chan = chan_make<int>(128) })` 처럼 정의된 프로세스를 실행한다.
- `process_cluster.wait_all()`은 모든 하위 프로세스가 종료될 때까지 대기하고, `process_cluster.supervise(handler)`는 비정상 종료 시 핸들러를 호출한다.
- `chan.close()` 호출 시 모든 리스너에게 EOF를 브로드캐스트하며, 채널이 닫힌 뒤 송신을 시도하면 컴파일러가 경고한다.

#### 2.3 Shared Memory Helpers
- `process.shared<T>(size_t count)`가 제공되어, 투명한 double-mapped 메모리를 이용한 구조체 공유를 지원한다.
- `process.map_file(path, flags)`는 파일 매핑 기반 IPC를 손쉽게 만들어 준다.

---

### 3. Combined Example

```margo
@import threads/core
@import process

threads.pipeline {
    @cozy _pairs fn reader(int id, char *dat) shares parser sink { ... }
    @aggressive _isolate fn sink(int id, char *dat) { ... }
}

process.ingest_cluster {
    process_extract(chan[64] string) {
        auto msg = <-chan
        threads.pipeline.reader.spawn(id = 0, dat = msg.data)
    }
}
```

1. `process.ingest_cluster`는 외부 데이터를 읽어 채널을 통해 문자열을 방출한다.
2. 채널 리시버는 `threads.pipeline.reader`를 `_pairs` 그룹으로 띄워 파서/싱크 작업을 동시에 처리한다.
3. `_isolate`로 선언된 `sink`는 다른 쓰레드와 자원을 공유하지 않아, 캐시 간섭 없는 고성능 처리가 가능하다.

---

향후 실제 구현 시 `threads/core`, `process` 모듈은 빌더 단계에서 자동으로 C 런타임 라이브러리에 연결되고, `threads`/`process` 전용 진단 메시지가 `sema`/`transpiler`에 추가된다.
