# 05. Standard IO Helpers

`@import std/io`를 통해 고급 입출력 도움 함수를 활성화한다. `print()`의 `sep`/`endl` 옵션과 `Scan`/`ScanLine`의 자동 타입 판별을 연습한다.

## 학습 목표
- `print(value, sep = "", endl = " ")`와 같이 키워드 인자를 전달한다.
- `Scan(int_var)`를 호출해 공백 단위 토큰을 읽는다.
- `ScanLine(buffer)`로 개행까지 통째로 받아온다.

## 실행 방법
```bash
cd tutorials/05_std_io
make && ./example
```
실행 중 사용자 입력이 필요하다.
