# 06. Styles & Decorators

`@style`, `@set`, 함수 데코레이터를 활용해 메타 지시자를 다루는 법을 익힌다.

## 학습 목표
- `@style c { ... }` 블록을 통해 C 코드를 그대로 삽입한다.
- `@style c` 블록 안에서 raw C와 Margo 문법(`fn`, `print`, 괄호 없는 `if` 등)을 자유롭게 섞어 쓴다.
- 세미콜론은 블록 안팎 어디서든 생략 가능하다 (멀티라인 `#define`이나 Allman 스타일 헤더 포함).
- `@set` 지시자로 빌드 힌트를 남긴다 (현재는 주석 처리되지만, 도구화 기반으로 예약되어 있다).
- `fn @decorator foo()` 구문을 통해 함수 수준의 어노테이션을 남긴다.

## 실행 방법
```bash
cd tutorials/06_styles_and_decorators
make && ./example
```
