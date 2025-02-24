#include "kernel.h"
#include "common.h"

typedef unsigned char uint8_t; // 0 ~ 2^8-1 (255)
typedef unsigned int uint32_t; // 0 ~ 2^32-1
typedef uint32_t size_t;

#define PROCS_MAX 8     // 최대 프로세스 개수
#define PROC_UNUSED 0   // 사용되지 않는 프로세스 구조체
#define PROC_RUNNABLE 1 // 실행 가능한 프로세스

struct process procs[PROCS_MAX]; // 모든 프로세스 제어 구조체 배열
struct process *current_proc;    // 현재 실행 중인 프로세스
struct process *idle_proc;       // Idle 프로세스

extern char __kernel_base[];
extern char __free_ram[], __free_ram_end[];

/** Bump Allocator / Linear Allocator
 * @brief  메모리 할당 함수, (메모리 해제 기능 없음)
 *
 * @param n 할당할 페이지 수
 * @return paddr_t 할당된 메모리 주소
 */
paddr_t alloc_pages(uint32_t n) {
  static paddr_t next_paddr = (paddr_t)__free_ram;
  paddr_t paddr = next_paddr;
  // 링커 스크립트에서 ALIGN(4096)으로 정렬되어 있음
  next_paddr += n * PAGE_SIZE;

  if (next_paddr > (paddr_t)__free_ram_end)
    PANIC("out of memory");

  memset((void *)paddr, 0, n * PAGE_SIZE);
  return paddr;
}

/**
 * @brief 가상주소를 물리주소로 매핑하는 페이지 테이블 엔트리를 설정
 *
 * table1 (1단계 페이지 테이블)
 * - 각 엔트리는 2단계 페이지 테이블의 물리 주소와 플래그를 포함
 *
 * table0 (2단계 페이지 테이블)
 * - 각 엔트리는 물리 주소와 플래그를 포함
 *
 * @param table1 1단계 페이지 테이블의 포인터
 * @param vaddr 매핑할 가상 주소
 * @param paddr 매핑할 물리 주소
 * @param flags 페이지 속성 플래그
 */
void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
  // 주소 정렬 검사, 각 주소가 페이지 크기에 맞게 정렬되어 있는지 확인
  if (!is_aligned(vaddr, PAGE_SIZE))
    PANIC("unaligned vaddr %x", vaddr);
  if (!is_aligned(paddr, PAGE_SIZE))
    PANIC("unaligned paddr %x", paddr);

  // 1단계 페이지 테이블 인덱스 계산 (오른쪽으로 22칸 시프트, 상위 10비트)
  uint32_t vpn1 = (vaddr >> 22) & 0x3ff;

  // 2단계 페이지 테이블 생성 확인 (지연 할당, 요구 페이징)
  // 처음부터 모든 주소에 대해 2단계 테이블을 만들면 메모리가 낭비
  // 필요할 때만 2단계 테이블을 생성하여 메모리를 절약
  if ((table1[vpn1] & PAGE_V) == 0) {
    // 2단계 페이지 테이블을 위한 물리 메모리 할당
    uint32_t pt_paddr = alloc_pages(1);
    // 물리 페이지 번호 (PPN)과 플래그를 설정하여 1단계 페이지 테이블에 매핑
    table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10) | PAGE_V;
  }

  // 중간 10비트를 추출하여 2단계 페이지 테이블의 인덱스로 사용
  uint32_t vpn0 = (vaddr >> 12) & 0x3ff;
  // 가상 주소의 중간 10비트를 추출하여 2단계 페이지 테이블의 인덱스로 사용
  uint32_t *table0 = (uint32_t *)((table1[vpn1] >> 10) * PAGE_SIZE);
  // 물리 주소와 플래그를 설정하여 최종 매핑
  table0[vpn0] = ((paddr / PAGE_SIZE) << 10) | flags | PAGE_V;
}

/**
 * @brief 프로세스 생성
 *
 * @param pc 프로세스가 처음 실행할 주소
 * @return struct process*  생성된 프로세스 구조체의 주소
 */
struct process *create_process(uint32_t pc) {
  // 미사용 상태의 프로세스 구조체 찾기
  struct process *proc = NULL;
  int i;
  for (i = 0; i < PROCS_MAX; i++) {
    // procs 배열에서 UNUSED 상태인 슬롯의 주소를 proc에 저장
    if (procs[i].state == PROC_UNUSED) {
      proc = &procs[i];
      break;
    }
  }

  if (!proc)
    PANIC("no free process slots");

  // 커널 스택 초기화, 스택의 최상단(가장 높은 주소)부터 시작
  uint32_t *sp = (uint32_t *)&proc->stack[sizeof(proc->stack)];

  // 커널 스택에 callee-saved 레지스터 공간을 미리 준비
  // 첫 컨텍스트 스위치 시, switch_context에서 이 값들을 복원
  *--sp = 0;            // s11
  *--sp = 0;            // s10
  *--sp = 0;            // s9
  *--sp = 0;            // s8
  *--sp = 0;            // s7
  *--sp = 0;            // s6
  *--sp = 0;            // s5
  *--sp = 0;            // s4
  *--sp = 0;            // s3
  *--sp = 0;            // s2
  *--sp = 0;            // s1
  *--sp = 0;            // s0
  *--sp = (uint32_t)pc; // ra (처음 실행 시 점프할 주소)

  uint32_t *page_table = (uint32_t *)alloc_pages(1);
  // 커널 메모리 영역을 일대일 방식으로 매핑
  for (paddr_t paddr = (paddr_t)__kernel_base; paddr < (paddr_t)__free_ram_end;
       paddr += PAGE_SIZE)
    map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);

  // 구조체 필드 초기화
  proc->pid = i + 1;
  proc->state = PROC_RUNNABLE;
  proc->sp = (uint32_t)sp;
  proc->page_table = page_table;
  return proc;
}

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
 * @brief 프로세스 간 컨텍스트 스위치 수행
 * called-saved 레지스터(ra, sp, s0-s11)만 저장/복원하여 성능 최적화
 *
 * @param prev_sp 이전 프로세스의 스택 포인터 (a0 레지스터)
 * @param next_sp 다음 프로세스의 스택 포인터 (a1 레지스터)
 */
__attribute__((naked)) void switch_context(uint32_t *prev_sp,
                                           uint32_t *next_sp) {

  __asm__ __volatile__(
      //=========================================================
      // 1. 현재 프로세스의 스택에 callee-saved 레지스터를 저장
      "addi sp, sp, -13 * 4\n" // 13개(4바이트씩), 52B 레지스터 공간 확보
      "sw ra,  0  * 4(sp)\n"   // callee-saved 레지스터만 저장
      "sw s0,  1  * 4(sp)\n"
      "sw s1,  2  * 4(sp)\n"
      "sw s2,  3  * 4(sp)\n"
      "sw s3,  4  * 4(sp)\n"
      "sw s4,  5  * 4(sp)\n"
      "sw s5,  6  * 4(sp)\n"
      "sw s6,  7  * 4(sp)\n"
      "sw s7,  8  * 4(sp)\n"
      "sw s8,  9  * 4(sp)\n"
      "sw s9,  10 * 4(sp)\n"
      "sw s10, 11 * 4(sp)\n"
      "sw s11, 12 * 4(sp)\n"

      //=========================================================
      // 2. 스택 포인터 교체
      "sw sp, (a0)\n" // *prev_sp = sp, 현재 스택 포인터를 prev_sp에 저장
      "lw sp, (a1)\n" // sp를 다음 프로세스의 값으로 변경

      //=========================================================
      // 3. 다음 프로세스 스택에서 callee-saved 레지스터 복원
      "lw ra,  0  * 4(sp)\n" // 저장해둔 반환 주소 복원
      "lw s0,  1  * 4(sp)\n" // s0 ~ s11 레지스터 복원
      "lw s1,  2  * 4(sp)\n"
      "lw s2,  3  * 4(sp)\n"
      "lw s3,  4  * 4(sp)\n"
      "lw s4,  5  * 4(sp)\n"
      "lw s5,  6  * 4(sp)\n"
      "lw s6,  7  * 4(sp)\n"
      "lw s7,  8  * 4(sp)\n"
      "lw s8,  9  * 4(sp)\n"
      "lw s9,  10 * 4(sp)\n"
      "lw s10, 11 * 4(sp)\n"
      "lw s11, 12 * 4(sp)\n"
      "addi sp, sp, 13 * 4\n" // 스택 포인터 복원 후, 스택 포인터 복귀
      "ret\n");               // 복원된 ra 주소로 반환
}

/**
 * @brief 라운드 로빈 방식의 스케줄러 (순차적)
 * 프로세스들이 자발적으로 CPU를 양보
 * 우선순위나 실행 시간은 고려 x
 */
void yield(void) {
  // 실행 가능한 프로세스 탐색
  struct process *next = idle_proc;
  for (int i = 0; i < PROCS_MAX; i++) {
    struct process *proc = &procs[(current_proc->pid + i) % PROCS_MAX];
    if (proc->state == PROC_RUNNABLE && proc->pid > 0) {
      next = proc;
      break;
    }
  }

  // 현재 프로세스를 제외하고 실행 가능한 프로세스가 없다면 리턴
  if (next == current_proc)
    return;

  // 다음 프로세스의 스택 포인터를 sscratch CSR에 저장
  // 나중에 예외가 발생했을 때, 커널이 이 스택을 사용하여 컨텍스트 복원
  __asm__ __volatile__(
      "sfence.vma\n" // TLB(Tanslation Lookaside Buffer) 엔트리를 모두 무효화
      "csrw satp, %[satp]\n" // satp 레지스터에 값을 쓰면 페이지 테이블 변경
      "sfence.vma\n" // 새로운 페이지 테이블 설정이 즉시 적용되도록 무효화 처리
      "csrw sscratch, %[sscratch]\n"
      :
      : [satp] "r"(SATP_SV32 | ((uint32_t)next->page_table / PAGE_SIZE)),
        [sscratch] "r"((uint32_t)&next->stack[sizeof(next->stack)]));

  // 컨텍스트 스위칭
  struct process *prev = current_proc;
  current_proc = next;
  switch_context(&prev->sp, &next->sp);
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
      "csrrw sp, sscratch, sp\n" // sscratch와 sp 레지스터 값을 서로 교환,
                                 // sscratch로부터 커널 스택의 실행 중인
                                 // 프로세스를 복원
      "addi sp, sp, -4 * 31\n"   // 스택에 31개 레지스터를 저장할 공간 확보
      "sw ra,  4 * 0(sp)\n"      // 리턴 주소 저장
      "sw gp,  4 * 1(sp)\n"      // 전역 포인터 저장
      "sw tp,  4 * 2(sp)\n"      // 스레드 포인터 저장
      "sw t0,  4 * 3(sp)\n"      // =================== t0 ~ t6 (임시 레지스터)
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

      //===================================================================
      // 기존 스택 포인터를 보관하고 예외 처리를 위한 새로운 스택 영역을 설정
      "csrr a0, sscratch\n" // sscratch에서 원래 sp 값 읽기
      "sw a0, 4 * 30(sp)\n" // 스택에 저장

      "addi a0, sp, 4 * 31\n" // 현재 sp에 124(31*4)을 더한 주소를 a0에 저장
      "csrw sscratch, a0\n"   // 계산된 새 스택 포인터 값(a0)을 sscratch
                              // 레지스터에 저장, 다음 예외 발생 시 사용할 스택
                              // 포인터 준비

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

void delay(void) {
  for (int i = 0; i < 30000000; i++)
    __asm__ __volatile__("nop"); // do nothing
}

struct process *proc_a;
struct process *proc_b;

void proc_a_entry(void) {
  printf("starting process A\n");
  while (1) {
    putchar('A');
    yield();
  }
}

void proc_b_entry(void) {
  printf("starting process B\n");
  while (1) {
    putchar('B');
    yield();
  }
}

// 커널 메인 함수
void kernel_main(void) {
  /* BSS 영역 초기화 (BSS 영역만큼 버퍼를 0으로 채움)
   * 일부 부트로더가 .bss를 클리어해주기도 하지만 여러 환경에서 확실히 동작하게
   * 하려면 수동으로 초기화 하는 것이 안전 */
  memset(__bss, 0, (size_t)__bss_end - (size_t)__bss);

  printf("\n\n");

  /*
   * 1. stvec 레지스터에 kernel_entry 함수의 주소(예외 핸들러)를 저장
   * 2. unimp 명령어 실행
   * 3. 예외 발생
   * 4. CPU가 stvec에 저장된 주소(kernel_entry)로 점프하여 예외 핸들러 실행
   * 5. 예외 처리 시작
   */
  WRITE_CSR(stvec, (uint32_t)kernel_entry);

  idle_proc = create_process((uint32_t)NULL);
  idle_proc->pid = 0; // idle
  current_proc = idle_proc;

  proc_a = create_process((uint32_t)proc_a_entry);
  proc_b = create_process((uint32_t)proc_b_entry);

  yield();
  PANIC("switched to idle process");

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
__attribute__((section(".text.boot"))) __attribute__((naked))
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
