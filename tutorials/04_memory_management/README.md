# 04. Memory Management Helpers

표준 C 힙 할당 루틴 대신 `alloc()`/`alloc_and_init()` 매크로를 이용해 실험적인 RAII 스타일을 구현하는 방법을 보여준다.

## 학습 목표
- 고정 크기 버퍼를 확보하고 실패 여부를 검사한다.
- 문자열 리터럴을 복사한 버퍼를 반환하는 `alloc_and_init()`를 사용한다.
- `print()`를 이용해 메모리 확보 상태를 사용자에게 안내한다.

## 실행 방법
```bash
cd tutorials/04_memory_management
make && ./example
```
