#pragma once

typedef int bool;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef uint32_t size_t;

// 물리 메모리 주소를 나타내는 타입 (pysical memory address)
typedef uint32_t paddr_t;
// 가상 메모리 주소를 나타내는 타입 (virtual memory address)
typedef uint32_t vaddr_t;

#define PAGE_SIZE 4096
#define true 1
#define false 0
#define NULL ((void *)0) // NULL을 void 포인터의 0으로 설정

// value를 align의 배수로 맞춰 올림, 여기서 align은 2의 거듭제곱
#define align_up(value, align) __builtin_align_up(value, align)
// value가 align의 배수인지 확인
#define is_aligned(value, align) __builtin_is_aligned(value, align)
// 구조체 내에서 특정 멤버가 시작되는 위치(바이트 단위)를 반환
#define offsetof(type, member) __builtin_offsetof(type, member)

// 아래 매크로는 컴파일러의 내장 기능을 사용하여 가변 인자를 처리
#define va_list __builtin_va_list
#define va_start __builtin_va_start
#define va_end __builtin_va_end
#define va_arg __builtin_va_arg

// 메모리 조작 함수들
void *memset(void *buf, char c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

// 문자열 조작 함수들
char *strcpy(char *dst, const char *src);
int strcmp(const char *s1, const char *s2);
void printf(const char *fmt, ...);