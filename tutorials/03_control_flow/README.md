# 03. Control Flow Sugar

`for`/`while`/`if` 구문의 축약형을 통해 가독성을 높이는 방법을 소개한다.

## 학습 목표
- 초기화식에서 추출한 루프 변수를 조건/증감식에 자동으로 붙이는 `for` 축약 구문을 이해한다.
- 괄호 없이 `while size > 0 { ... }` 같은 패턴을 작성한다.
- `if value == 42 print("...")` 처럼 괄호와 중괄호를 생략하는 문법을 확인한다.
- `if cond` 뒤 개행으로 시작해 `else`/`else if`/`endif`로 닫는 blocked if를 사용한다.

## 참고
- blocked if의 조건 뒤에는 바로 개행이 와야 하며, 본문은 임의 개수의 문장을 가질 수 있다.
- `else`/`else if`/`endif`는 반드시 줄의 첫 토큰이어야 한다. 블록 안에서 C 스타일 `} else {`는 그대로 쓸 수 있다.
- `endif`를 빠뜨리면 "if block is missing endif" 오류가 발생한다.
- 브랜치 본문은 실제 스코프처럼 동작한다 (`alloc` RAII 해제, `defer` 모두 브랜치 끝에서 처리).

## 실행 방법
```bash
cd tutorials/03_control_flow
make && ./example
```
