#include "user.h"

extern char __stack_top[];

/**
 * @brief 시스템 콜을 호출
 * 1. 함수 인자들을 RISC-V의 인자 레지스터에 로드
 * 2. 시스템 콜 번호(sysno)를 a3 레지스터에 로드
 * 3. ecall 명령어를 실행하여 운영체제 커널로 제어를 전달
 * 4. 커널이 요청된 시스템 콜을 실행하고 결과를 a0 레지스터에 저장
 * 5. 함수는 a0 레지스터의 값을 반환
 *
 * @param sysno 실행할 시스템 콜의 번호
 * @param arg0 시스템 콜에 전달할 인자0
 * @param arg1 시스템 콜에 전달할 인자1
 * @param arg2 시스템 콜에 전달할 인자2
 * @return int
 */
int syscall(int sysno, int arg0, int arg1, int arg2) {
  // 레지스터 할당
  register int a0 __asm__("a0") = arg0;
  register int a1 __asm__("a1") = arg1;
  register int a2 __asm__("a2") = arg2;
  // 시스템 콜 번호를 저장
  register int a3 __asm__("a3") = sysno;

  __asm__ __volatile__("ecall" // 예외 핸들러 호출, 제어권을 커널로 넘김
                       : "=r"(a0)
                       : "r"(a0), "r"(a1), "r"(a2), "r"(a3)
                       : "memory");

  return a0;
}

// 단일 문자 출력
void putchar(char ch) { syscall(SYS_PUTCHAR, ch, 0, 0); }

int getchar(void) { return syscall(SYS_GETCHAR, 0, 0, 0); }

__attribute__((noreturn)) void exit(void) {
  syscall(SYS_EXIT, 0, 0, 0);
  for (;;)
    ;
}

__attribute__((section(".text.start"))) __attribute__((naked)) void
start(void) {
  __asm__ __volatile__("mv sp, %[stack_top] \n"
                       "call main \n"
                       "call exit \n" ::[stack_top] "r"(__stack_top));
}
