#pragma once

// 아래 매크로는 컴파일러의 내장 기능을 사용하여 가변 인자를 처리
#define va_list __builtin_va_list
#define va_start __builtin_va_start
#define va_end __builtin_va_end
#define va_arg __builtin_va_arg

void printf(const char *fmt, ...);