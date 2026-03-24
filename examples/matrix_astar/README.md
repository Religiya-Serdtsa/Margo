# Matrix A* Pathfinder

이 예제는 `matrix/core`에서 소개된 기초 함수(`matrix_fill`, `matrix_map`, `matrix_get`/`matrix_set`, `matrix_neighbor_with_filter`)를 활용해 7x9 격자에서 A* 경로 탐색을 수행한다. 지형은 0(통과 가능)과 1(벽)로 구성되어 있으며, 코드는 다음 요소를 강조한다.

- `matrix_map`으로 원본 지형을 즉시 가중치 행렬로 변환한다.
- `matrix_fill`로 점수/흔적 행렬을 초기화하고, 이후에는 `matrix_get`/`matrix_set`으로 값을 읽고 쓴다.
- `matrix_neighbor_with_filter`와 `neighbor_view_for_each`는 열린 집합에서 이웃 셀을 추출하고, 장애물을 제외하는 필터를 적용한다.
- 최종 경로는 행렬 마스크로 표현되어, 렌더러가 각 셀을 문자(`S`, `G`, `*`, `#`, `.`)로 치환한다.

## 실행 방법

```bash
cd examples/matrix_astar
make
./matrix_astar
```

열린 집합이 빌 때까지 A*를 수행하고, 발견된 경로 길이와 탐색된 노드 수를 출력한다. 벽 배치를 바꾸려면 `matrix_astar.margo` 상단의 `terrain` 행렬을 수정하면 된다.
