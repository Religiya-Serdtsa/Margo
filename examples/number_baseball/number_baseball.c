#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

static void generate_secret(int secret[3]) {
    int used[10] = {0};
    int idx = 0;
    while (idx < 3) {
        int digit = (rand() % 9) + 1; /* 1-9 */
        if (used[digit]) {
            continue;
        }
        used[digit] = 1;
        secret[idx++] = digit;
    }
}

static bool parse_guess(const char *line, int guess[3]) {
    int used[10] = {0};
    int count = 0;
    for (const char *p = line; *p && *p != '\n'; ++p) {
        if (*p < '0' || *p > '9') {
            continue;
        }
        int digit = *p - '0';
        if (digit == 0 || used[digit]) {
            return false;
        }
        used[digit] = 1;
        guess[count++] = digit;
        if (count == 3) {
            break;
        }
    }
    return count == 3;
}

static void score_guess(const int secret[3], const int guess[3], int *strike, int *ball) {
    *strike = 0;
    *ball = 0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (secret[i] != guess[j]) {
                continue;
            }
            if (i == j) {
                (*strike)++;
            } else {
                (*ball)++;
            }
        }
    }
}

int main(void) {
    srand((unsigned int)time(NULL));
    int secret[3];
    generate_secret(secret);

    printf("=== 숫자 야구 (Margo Example) ===\n");
    printf("1~9 사이의 서로 다른 숫자 3개를 맞혀보세요.\n");

    char buffer[128];
    int attempts = 0;
    while (1) {
        printf("Guess %d > ", attempts + 1);
        if (!fgets(buffer, sizeof(buffer), stdin)) {
            puts("입력을 읽을 수 없습니다. 종료합니다.");
            return 1;
        }
        int guess[3];
        if (!parse_guess(buffer, guess)) {
            puts("서로 다른 1~9 숫자 3개를 입력하세요.");
            continue;
        }
        attempts++;
        int strike = 0;
        int ball = 0;
        score_guess(secret, guess, &strike, &ball);
        if (strike == 3) {
            printf("정답! %d번 만에 성공했습니다.\n", attempts);
            break;
        }
        if (strike == 0 && ball == 0) {
            puts("아웃!");
        } else {
            printf("%dS %dB\n", strike, ball);
        }
    }

    return 0;
}
