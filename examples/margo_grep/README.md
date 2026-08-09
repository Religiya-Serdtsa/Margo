# margo-grep

Margo로 작성한 작고 실용적인 Unix `grep` 스타일 CLI다. 정규식 대신 빠르고 예측 가능한 **리터럴 부분 문자열** 검색을 제공한다. 파일을 여러 개 받거나 표준 입력을 읽을 수 있고, 대소문자 무시와 줄 번호 표기를 지원한다.

## 빌드

```bash
make
```

## 사용법

```bash
./margo-grep [-i] [-n] PATTERN [FILE ...]
```

- `-i`: ASCII 대소문자를 구분하지 않는다.
- `-n`: 일치한 줄 앞에 줄 번호를 출력한다.
- `-`: 파일 위치에 쓰면 표준 입력을 읽는다.
- 파일을 두 개 이상 검색하면 각 결과에 파일 이름을 붙인다.

예시:

```bash
./margo-grep -in TODO README.md docs/spec-outline.md
printf 'Alpha\nbeta\n' | ./margo-grep -i alpha
```

일치 항목이 없으면 `1`, 인자 또는 파일 오류가 있으면 `2`를 반환하므로 셸 스크립트에서도 사용할 수 있다.

## 구현에서 보는 Margo

- `@import c/...`로 기존 C 표준 라이브러리를 사용한다.
- `fn`, `string`, `weird(FILE, 1)`, 배열 및 괄호 없는 제어 흐름으로 파일 검색기를 구성한다.
- `start(argc, argv)`는 자동 생성되는 C `main`과 연결되어 일반 Unix CLI처럼 인자를 받는다.
