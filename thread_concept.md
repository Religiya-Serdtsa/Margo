## Threading & Multiprocessing Concept (Draft v0.2)

쓰레드와 프로세스 런타임의 설계 목표는 *구조적으로 단순한 문법*을 유지한 채, 클러스터 단위로 작업을 선언하고 `init → join → sync` 흐름을 강제하는 것이다. 장식적인 조합이나 별도 DSL 없이도 눈에 보이는 순서로 구성할 수 있어야 한다.

---

### 1. Thread Cluster (`threads.<name>`)

```margo
@import threads/core

threads t_cluster_1 {
    locked_var {
        var x;
        var y;
        var z;
    }
    thread1 @chantype int { ... }
    thread2 @chantype int { ... }
    thread3 @chantype int { ... }
    thread4 @nochan @independent { print("hello world") }
    thread5 @lazyjoin { print("lazy join") }
}
```

- **네임스페이스:** `threads <name>` 블록은 고정된 클러스터 이름(`t_cluster_1`)을 만들고, 내부 정의는 `t_cluster_1.thread1`, `t_cluster_1.thread5`처럼 접두사로 접근한다.
- **Locked Vars:** `locked_var { ... }` 안의 `var` 선언은 클러스터 전용 공유 상태다. 런타임이 단일 뮤텍스로 보호하며, 스레드에서는 `locked.x` 식으로 접근한다.
- **Thread Bodies:** 각 `threadN { ... }` 블록은 `init()` 호출 시 시작되는 실제 함수 본문이다.

#### 1.1 채널과 기본 인자
- `@chantype T`는 해당 스레드가 `chan T`를 주입받는다는 뜻이다. `threadN.init(chan_handle)`처럼 동일 타입의 채널을 넘기면 런타임이 인자를 고정해 준다.
- `@nochan`은 채널을 받지 않는 스레드임을 나타내며, `init()`에 추가 인자가 없다.
- 클러스터에서 사용하는 채널은 `chan int inbox = make channel()`처럼 `make channel()` 팩토리로 만든다. 버퍼 크기 등 세부 설정은 선택적 인자로 확장 예정이다.

#### 1.2 데코레이터
- `@independent`: `join_all()`과 무관하게 독립적으로 띄우고, `threadN.join()`으로 개별 합류시킨다. 종료 후 `threadN.init()`을 다시 호출해 재시작할 수 있다.
- `@lazyjoin`: 스레드가 끝나더라도 즉시 `join`을 요구하지 않는다. 대신 `threads.<cluster>.sync()` 호출 때까지 핸들이 보류되고, 그 시점에 한꺼번에 회수된다.
- `@chantype`, `@nochan`, `@independent`, `@lazyjoin` 외의 추가 장식자는 도입하지 않는다. CPU affinity나 인라인 여부 같은 정책은 향후 `threads/core` C API에서 직접 결정한다.

---

### 2. 실행 수명주기

```margo
fn int main(void) {
    chan int c = make channel()
    t_cluster_1.thread1.init(c)
    t_cluster_1.thread2.init(c)
    t_cluster_1.thread3.init(c)
    t_cluster_1.thread4.init()

    t_cluster_1.join_all(c)   // 같은 chan을 쓰는 스레드(1~3) 한번에 join
    t_cluster_1.thread4.join()
    t_cluster_1.thread4.init()  // independent 스레드는 필요할 때 재시작

    auto err = t_cluster_1.sync()  // lazy join 및 잔여 작업 정리
    return err
}
```

- **`threadN.init(...)`:** 스레드를 띄운다. `@chantype` 스레드는 채널을, `@nochan` 스레드는 인자를 받지 않는다. 필요 시 추가 일반 인자를 선언할 수 있지만, 채널 타입만 데코레이터로 지정한다.
- **`join_all(chan)`:** 동일 채널을 공유하며 `@chantype`로 선언된 스레드들을 한 번에 기다린다. 채널별로 호출하면 순서 걱정 없이 안전하게 정리된다.
- **`threadN.join()`:** 독립 스레드를 기다릴 때 사용한다. `@lazyjoin`이 아닌 스레드에 대해 호출하면 즉시 종료 상태를 돌려준다.
- **`threadN.init()` 반복 호출:** `@independent`이면서 `@nochan`인 스레드는 blocking 없이 반복 실행 시나리오에 적합하다. `join()`이 끝난 뒤 언제든 다시 `init()`할 수 있다.
- **`threads.<cluster>.sync()`:** 아직 `join`되지 않은 `@lazyjoin` 스레드, 채널 정리, locked 변수 해제 등을 모두 처리한다. 실패 시 런타임 전용 에러 코드(`thread_err_t`)를 돌려주며, 성공하면 `null`/`0`이다.

`sync()` 이후에는 같은 클러스터 이름으로 다시 `init()`을 호출할 수 있지만, 이전에 사용한 채널을 재사용하려면 사용자가 직접 `chan.close()` 또는 다른 종료 처리를 호출해야 한다.

---

### 3. 프로세스 클러스터 (`process.<name>`)

쓰레드 디자인과 동일하게, 프로세스도 하나의 블록에 묶어 선언한다.

```margo
@import process

process ingest_cluster {
    process_worker(chan int inbox) {
        auto value = <-inbox
        // ...
    }
}
```

- 각 `process_*` 정의는 독립 `fork()` 컨텍스트다.
- 채널은 `chan` 타입만 허용하며, `chan[N] T`로 버퍼를 지정한다.
- `process.ingest_cluster.spawn(process_worker, init_args...)`로 실행하고, `process.ingest_cluster.wait_all()` 혹은 `process.ingest_cluster.supervise(handler)`로 종료를 관리한다.
- 채널 송수신은 Go 스타일(`chan <- value`, `value <- chan`)을 그대로 따른다.

---

### 4. 스레드 + 프로세스 파이프라인 예시

```margo
threads pipeline {
    locked_var { var checksum; }
    reader  @chantype int { ... }
    parser  @chantype int { ... }
    sink    @nochan @lazyjoin { ... }
}

process ingest_cluster {
    generator(chan[64] int outbox) {
        for value = 0; <100; ++ {
            outbox <- value
        }
    }
}

fn void start_pipeline(void) {
    chan int pipe = make channel()
    ingest_cluster.generator.init(pipe)
    pipeline.reader.init(pipe)
    pipeline.parser.init(pipe)
    pipeline.join_all(pipe)
    pipeline.sync()
}
```

이 조합은 다음과 같은 흐름을 따른다.

1. 프로세스가 데이터를 `chan int pipe`로 밀어 넣는다.
2. 동일 채널을 사용하는 `reader`, `parser` 스레드는 `join_all(pipe)` 한 번으로 정리된다.
3. `sink`는 `@lazyjoin`이므로 `sync()` 단계에서 뒤늦게 합류하며, `locked_var`에 기록된 `checksum`을 안전하게 업데이트한다.

---

향후 실제 구현 시에도 위 구조를 벗어나지 않는다. `threads/core`는 `threadN.init/join`, `join_all`, `sync`, `locked_var`에 대응하는 얇은 C API만 제공하며, 불필요한 mailbox나 `_pairs` 같은 추가 개념은 런타임에 포함되지 않는다. 프로세스 런타임(`process`) 역시 채널/클러스터/대기 함수만 노출하여 Margo 언어가 C++/Rust식 재설계를 반복하지 않도록 한다.
