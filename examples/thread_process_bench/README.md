# Thread + Process Bench

이 예제는 새롭게 추가된 `@import threads/core`, `@import process` 런타임을 함께 사용하여 데이터를 읽어서 파이프라인으로 가공하는 벤치마크다.

- 쓰레드 파트: `threads` 클러스터를 선언해 reader/parser/sink 세 작업을 하나의 `chan int`으로 연결하고, `join_all(chan)`/`sync()` 흐름으로 정리한다.
- 프로세스 파트: `process_channel_t`를 이용해 별도 프로세스가 난수를 생성하고 부모 프로세스가 이를 집계한다. `process_cluster_wait_all`로 종료를 감시하고, 채널은 `process_channel_close()`로 EOF를 브로드캐스트한다.

```bash
make       # ../../build/margo가 준비되어 있어야 함
./bench    # 쓰레드/프로세스 두 파이프라인의 처리량을 출력
```
