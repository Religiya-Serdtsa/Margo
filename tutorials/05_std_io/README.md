# 05. Standard IO Helpers

`@import std/io`를 통해 고급 입출력 도움 함수를 활성화한다. `print()`의 `sep`/`endl` 옵션, `Scan`/`ScanLine`의 자동 타입 판별, 그리고 iostream 스타일의 `out`/`in` 문을 연습한다.

## 학습 목표
- `print(value, sep = "", endl = " ")`와 같이 키워드 인자를 전달한다.
- `Scan(int_var)`를 호출해 공백 단위 토큰을 읽는다.
- `ScanLine(buffer)`로 개행까지 통째로 받아온다.
- `out "%i %i\n" % x % y`처럼 포맷 리터럴 + `%` 체인으로 `printf` 스타일 출력을 만든다.
- `out "x=" % x % "\n"`처럼 포맷 없이 값을 cout 스타일로 이어 붙인다 (구분자·개행 자동 추가 없음).
- `in x y`로 여러 변수를 타입에 맞춰 한 번에 읽는다 (`cin >> x >> y`에 대응).

## 참고
- `out` 문 안에서 최상위 `%`는 체인 연산자다. 나머지 연산은 `(a % b)`처럼 괄호로 감싼다.
- 첫 피연산자가 `%` 변환 지정자를 포함한 문자열 리터럴이면 `printf` 형태로, 아니면 값 나열 형태로 하강한다.
- `obj.out`, `ctx->in` 같은 멤버 접근이나 `out`/`in`이라는 이름의 변수는 그대로 둔다.

## 실행 방법
```bash
cd tutorials/05_std_io
make && ./example
```
실행 중 사용자 입력이 필요하다.
