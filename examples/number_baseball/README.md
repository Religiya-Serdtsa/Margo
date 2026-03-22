# Number Baseball (숫자 야구)

Margo 소스(`number_baseball.margo`)를 바로 빌드해 실행하는 첫 예제다. `@import c/...` 지시자를 사용해 표준 헤더를 불러오고, `fn`으로 함수 정의를 작성한다.

## 빌드 및 실행

```bash
make          # ../../build/margo 를 호출해 바이너리 생성
./number_baseball
```

최상위 `margo build` 명령은 `.margo` 파일을 C로 변환한 뒤 시스템 C 컴파일러(`cc`)로 빌드한다. 예제는 1~9 사이 서로 다른 숫자 3개를 맞히는 숫자 야구 규칙을 구현한다.
