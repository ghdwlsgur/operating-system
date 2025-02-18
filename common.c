#include "common.h"

void putchar(char ch);

/**
 * @brief 메모리 내용을 한 곳에서 다른 곳으로 복사 (src -> dst)
 *
 * @param dst 복사할 대상 위치
 * @param src 복사할 원본 데이터 위치
 * @param n 복사할 바이트 수
 * @return void* 복사된 메모리의 시작 주소
 */
void *memcpy(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;             // 목적지 포인터
  const uint8_t *s = (const uint8_t *)src; // 원본 포인터
  while (n--)
    *d++ = *s++; // 현재 위치 복사 후, s, d 포인터를 1씩 증가
  return dst;
}

/**
 * @brief 메모리의 특정 영역을 주어진 값으로 채움 (메모리 초기화)
 *
 * @param buf 값을 채울 메모리의 시작 주소
 * @param c 채울 값
 * @param n 채울 바이트 수
 * @return void* 초기화된 메모리의 시작 주소
 */
void *memset(void *buf, char c, size_t n) {
  uint8_t *p = (uint8_t *)buf;
  while (n--)
    *p++ = c; // 현재 위치에 c값을 채운 후, p 포인터를 1씩 증가
  return buf;
}

/**
 * @brief 문자열 복사
 *
 * @param dst 복사될 대상 문자열의 주소
 * @param src 원본 문자열의 주소
 * @return char* 복사된 문자열의 시작 주소
 */
char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while (*src)
    *d++ = *src++; // 현재 위치 복사 후, src, d 포인터를 1씩 증가
  *d = '\0';
  return dst;
}

/**
 * @brief 두 문자열 비교
 *
 * @param s1 비교할 문자열 1
 * @param s2 비교할 문자열 2
 * @return int 두 문자열이 같으면 0, 다르면 0이 아닌 값
 */
int strcmp(const char *s1, const char *s2) {
  while (*s1 && *s2) {
    if (*s1 != *s2)
      break;
    s1++;
    s2++;
  }

  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

void printf(const char *fmt, ...) {
  va_list vargs;
  va_start(vargs, fmt);

  while (*fmt) {
    if (*fmt == '%') {
      fmt++;
      switch (*fmt) {
      case '\0':
        putchar('%');
        goto end;
      case '%':
        putchar('%');
        break;
      // 문자열 출력
      case 's': {
        const char *s = va_arg(vargs, const char *);
        while (*s) {
          putchar(*s);
          s++;
        }
        break;
      }
      // 정수 출력
      case 'd': {
        int value = va_arg(vargs, int);
        if (value < 0) {
          putchar('-');
          value = -value;
        }

        int divisor = 1;
        while (value / divisor > 9)
          divisor *= 10;

        while (divisor > 0) {
          putchar('0' + value / divisor);
          value %= divisor;
          divisor /= 10;
        }

        break;
      }
      // 16진수 출력
      case 'x': {
        int value = va_arg(vargs, int);
        for (int i = 7; i >= 0; i--) {
          int nibble = (value >> (i * 4)) & 0xf;
          putchar("0123456789abcdef"[nibble]);
        }
      }
      }
    } else {
      putchar(*fmt);
    }

    fmt++;
  }

end:
  va_end(vargs);
}