#include "kernel.h"
#include "common.h"

typedef unsigned char uint8_t; // 0 ~ 2^8-1 (255)
typedef unsigned int uint32_t; // 0 ~ 2^32-1
typedef uint32_t size_t;

/* 외부 심볼 선언
 * __bss ~ __bss_end: 초기화되지 않은 전역 변수가 저장될 메모리 영역
 * __stack_top: 스택의 최상단 주소
 * 실제로는 "해당 심볼이 가리키는 주소"가 필요하기 때문에 []를 사용하여 주소
 * 형식으로 사용 */
extern char __bss[], __bss_end[], __stack_top[];

/* SBI(Supervisor Binary Interface) 호출 함수
 * SBI 호출을 위한 레지스터 설정 및 ecall 명령어 실행
 * 8개의 인자(arg0 ~ arg5, fid, eid)를 받아서 RISC-V의 레지스터에 매핑
 * ecall 명령어를 통해 supervisor 모드로 진입하여 SEE(Supervisor Extension
 * Environment)에 요청 SEE는 일반적으로 더 높은 권한 수준(machine mode)에서
 * 실행되는 환경
 * ============================================= 레지스터 사용 규약
 * a7: SBI 확장 ID(EID)를 저장
 * a6: SBI 기능 ID(FID)를 저장
 * a0 ~ a5: SBI 호출 인자를 저장
 * ============================================= 반환 값 규약
 * OpenSBI 쪽에서 a0와 a1을 제외한 레지스터 값(a2 ~ a7)은 변경해서는 안됨
 * 커널 입장에서는 SBI 호출 후에도 a2 ~ a7 값이 유지됨을 보장받을 수 있음
 * a0: 에러 코드를 저장
 * a1: 반환 값(성공 시)을 저장 */
struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
                       long arg5, long fid, long eid) {

  // 인자들을 RISC-V의 a0~a7 레지스터에 할당
  // 아래와 같은 형식의 지시문은 시스템 콜에서도 유사하게 사용
  register long a0 __asm__("a0") = arg0;
  register long a1 __asm__("a1") = arg1;
  register long a2 __asm__("a2") = arg2;
  register long a3 __asm__("a3") = arg3;
  register long a4 __asm__("a4") = arg4;
  register long a5 __asm__("a5") = arg5;
  register long a6 __asm__("a6") = fid;
  register long a7 __asm__("a7") = eid;

  // 애플리케이션이 커널에 시스템 콜을 호출할 때도 ecall 사용
  // 상위 권한 레벨로의 함수를 호출하는 것과 비슷한 역할
  __asm__ __volatile__("ecall"
                       : "=r"(a0), "=r"(a1)
                       : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5),
                         "r"(a6), "r"(a7)
                       : "memory");

  // 결과값은 a0(error code), a1(return value)에 저장
  return (struct sbiret){.error = a0, .value = a1};
}

// SBI 콘솔 출력
void putchar(char ch) {
  sbi_call(ch, 0, 0, 0, 0, 0, 0, 1 /* Console Putchar */);
}

void handle_trap(struct trap_frame *f) {
  uint32_t scause = READ_CSR(scause); // 어떤 이유로 예외가 발생했는지
  uint32_t stval = READ_CSR(stval);   // 예외 부가정보 (잘못된 메모리 주소...)
  uint32_t user_pc = READ_CSR(sepc);  // 예외가 일어난 시점의 PC

  PANIC("unexpected trap scause=%x, stval=%x, sepc=%x\n", scause, stval,
        user_pc);
}

/**
 * @brief 예외 처리 핸들러
 * 1. 현재 실행 컨텍스트를 모두 저장
 * 2. 예외 처리 함수 실행
 * 3. 원래 컨텍스트 복원
 * 4. 프로그램 재개
 */
__attribute__((naked)) __attribute__((aligned(4))) void kernel_entry(void) {
  __asm__ __volatile__(
      "csrw sscratch, sp\n"    // 현재 스택 포인터를 sscratch CSR에 저장
      "addi sp, sp, -4 * 31\n" // 스택에 31개 레지스터를 저장할 공간 확보
      "sw ra,  4 * 0(sp)\n"    // 리턴 주소 저장
      "sw gp,  4 * 1(sp)\n"    // 전역 포인터 저장
      "sw tp,  4 * 2(sp)\n"    // 스레드 포인터 저장
      "sw t0,  4 * 3(sp)\n"    // =================== t0 ~ t6 (임시 레지스터)
      "sw t1,  4 * 4(sp)\n"
      "sw t2,  4 * 5(sp)\n"
      "sw t3,  4 * 6(sp)\n"
      "sw t4,  4 * 7(sp)\n"
      "sw t5,  4 * 8(sp)\n"
      "sw t6,  4 * 9(sp)\n"
      "sw a0,  4 * 10(sp)\n" // ================== a0 ~ a7 (인자 레지스터)
      "sw a1,  4 * 11(sp)\n"
      "sw a2,  4 * 12(sp)\n"
      "sw a3,  4 * 13(sp)\n"
      "sw a4,  4 * 14(sp)\n"
      "sw a5,  4 * 15(sp)\n"
      "sw a6,  4 * 16(sp)\n"
      "sw a7,  4 * 17(sp)\n"
      "sw s0,  4 * 18(sp)\n" // ================== s0 ~ s11 (저장 레지스터)
      "sw s1,  4 * 19(sp)\n"
      "sw s2,  4 * 20(sp)\n"
      "sw s3,  4 * 21(sp)\n"
      "sw s4,  4 * 22(sp)\n"
      "sw s5,  4 * 23(sp)\n"
      "sw s6,  4 * 24(sp)\n"
      "sw s7,  4 * 25(sp)\n"
      "sw s8,  4 * 26(sp)\n"
      "sw s9,  4 * 27(sp)\n"
      "sw s10, 4 * 28(sp)\n"
      "sw s11, 4 * 29(sp)\n"

      "csrr a0, sscratch\n" // sscratch에서 원래 sp 값 읽기
      "sw a0, 4 * 30(sp)\n" // 스택에 저장

      "mv a0, sp\n"        // 현재 스택 포인터를 인자로 전달
      "call handle_trap\n" // ! 실제 예외 처리 함수 호출

      "lw ra,  4 * 0(sp)\n" // 리턴 주소 복원
      "lw gp,  4 * 1(sp)\n" // 전역 포인터 복원
      "lw tp,  4 * 2(sp)\n"
      "lw t0,  4 * 3(sp)\n"
      "lw t1,  4 * 4(sp)\n"
      "lw t2,  4 * 5(sp)\n"
      "lw t3,  4 * 6(sp)\n"
      "lw t4,  4 * 7(sp)\n"
      "lw t5,  4 * 8(sp)\n"
      "lw t6,  4 * 9(sp)\n"
      "lw a0,  4 * 10(sp)\n"
      "lw a1,  4 * 11(sp)\n"
      "lw a2,  4 * 12(sp)\n"
      "lw a3,  4 * 13(sp)\n"
      "lw a4,  4 * 14(sp)\n"
      "lw a5,  4 * 15(sp)\n"
      "lw a6,  4 * 16(sp)\n"
      "lw a7,  4 * 17(sp)\n"
      "lw s0,  4 * 18(sp)\n"
      "lw s1,  4 * 19(sp)\n"
      "lw s2,  4 * 20(sp)\n"
      "lw s3,  4 * 21(sp)\n"
      "lw s4,  4 * 22(sp)\n"
      "lw s5,  4 * 23(sp)\n"
      "lw s6,  4 * 24(sp)\n"
      "lw s7,  4 * 25(sp)\n"
      "lw s8,  4 * 26(sp)\n"
      "lw s9,  4 * 27(sp)\n"
      "lw s10, 4 * 28(sp)\n"
      "lw s11, 4 * 29(sp)\n"
      "lw sp,  4 * 30(sp)\n" // 원래 스택 포인터 복원
      "sret\n");             // 예외 처리 완료, 원래 실행 지점으로 복귀
}

// 커널 메인 함수
void kernel_main(void) {
  printf("\n\nHello %s\n", "World!");
  printf("1 + 2 = %d, %x\n", 1 + 2, 0x1234abcd);

  /* BSS 영역 초기화 (BSS 영역만큼 버퍼를 0으로 채움)
   * 일부 부트로더가 .bss를 클리어해주기도 하지만 여러 환경에서 확실히 동작하게
   * 하려면 수동으로 초기화 하는 것이 안전 */
  memset(__bss, 0, (size_t)__bss_end - (size_t)__bss);

  /*
   * 1. stvec 레지스터에 kernel_entry 함수의 주소(예외 핸들러)를 저장
   * 2. unimp 명령어 실행
   * 3. 예외 발생
   * 4. CPU가 stvec에 저장된 주소(kernel_entry)로 점프하여 예외 핸들러 실행
   * 5. 예외 처리 시작
   */
  WRITE_CSR(stvec, (uint32_t)kernel_entry);
  __asm__ __volatile__("unimp"); // unimplemented instruction

  /*
    Hello World 메시지가 화면에 출력되는 과정 SBI 호출 시, 문자는 다음과 같이
  표시

  * 1. 커널에서 ecall 명령어 실행, CPU는 OpenSBI가 부팅 시점에 설정해둔 M-Mode
  트랩 핸들러(mtvec 레지스터)로 점프
  * 2. 레지스터를 저장한 뒤, C로 작성된 트랩 핸들러 호출
  * 3. eid에 따라, 해당 SBI 기능을 처리하는 함수가 실행
  * 4. 8250 UART용 디바이스 드라이버가 문자를 QEMU에 전송
  * 5. QEMU의 8250 UART 에뮬레이션이 이 문자를 받아서 표준 출력으로 보냄
  * 6. 터미널 에뮬레이터가 문자를 화면에 표시
  즉, Console Putchar 함수를 호출하는 건 단지 OpenSBI에 구현된 디바이스
  드라이버를 호출하는 것! */
  const char *s = "\n\nHello World!\n";
  for (int i = 0; s[i] != '\0'; i++) {
    putchar(s[i]);
  }

  /* wfi (Wait For Interrupt)
   * RISC-V의 전력 관리 명령어
   * CPU를 절전 모드로 진입, 인터럽트가 발생할 때까지 CPU를 대기 상태로 만듦 */
  for (;;) {
    __asm__ __volatile__("wfi");
  }
}

// boot() 함수를 섹션 (__text_boot)에 배치
#ifdef __APPLE__
__attribute__((section("__TEXT,__text_boot")))
__attribute__((naked)) //--------- macOS (Mach-O) Mach Object
#else
__attribute__((section(".text.boot")))
__attribute__((naked)) //--------- Linux (ELF) Executable and Linkable Format
#endif
// 함수 프롤로그와 에필로그를 자동으로 생성하지 않도록 설정 (어셈블리 코드 삽입)
__attribute__((naked)) void
boot(void) {
  // 스택의 최상단을 sp 레지스터에 설정
  // kernel_main() 함수로 점프
  __asm__ __volatile__("mv sp, %[stack_top]\n"
                       "j kernel_main\n"
                       :
                       : [stack_top] "r"(__stack_top));
}

/* 요약
1. 부트 코드 실행 (boot()), sp를 __stack_top으로 설정, kernel_main() 함수로 점프
2. kernel_main()에서 BSS 영역 초기화 및 무한 루프 실행
*/
