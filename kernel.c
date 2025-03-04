#include "kernel.h"
#include "common.h"

typedef unsigned char uint8_t; // 0 ~ 2^8-1 (255)
typedef unsigned int uint32_t; // 0 ~ 2^32-1
typedef uint32_t size_t;

#define PROCS_MAX 8     // 최대 프로세스 개수
#define PROC_UNUSED 0   // 사용되지 않는 프로세스 구조체
#define PROC_RUNNABLE 1 // 실행 가능한 프로세스

extern struct file files[FILES_MAX];
extern uint8_t disk[DISK_MAX_SIZE];
void read_write_disk(void *buf, unsigned sector, int is_write);

struct process procs[PROCS_MAX]; // 모든 프로세스 제어 구조체 배열
struct process *current_proc;    // 현재 실행 중인 프로세스
struct process *idle_proc;       // Idle 프로세스
struct virtio_virtq *virtq_init(unsigned index);

extern char __kernel_base[];
extern char __free_ram[], __free_ram_end[];

// shell.bin.o에 포함된 원시 바이너리 사용
extern char _binary_shell_bin_start[], _binary_shell_bin_size[];

// 사용자 모드 진입 함수
__attribute__((naked)) void user_entry(void) {
  __asm__ __volatile__(
      /* sepc(Supervisor Exception Program Counter) 레지스터에 USER_BASE 값을 씀
       * 이 레지스터는 sret 명령어 실행 시 프로그램이 돌아갈 주소를 지정
       * 즉, 사용자 프로그램(USER_BASE 주소)으로 점프 */
      "csrw sepc, %[sepc] \n"

      /* sstatus(Supervisor Status) 레지스터에 SSTATUS_SPIE 값을 씀
       * SSTATUS_SPIE는 인터럽트 활성화 상태와 관련된 비트 */
      "csrw sstatus, %[sstatus] \n"

      /* Supervisor Return, 예외 처리를 완료하고 sepc에 저장된 주소로 복귀
       * 특권 모드에서 사용자 모드로 전환 */
      "sret \n"
      :
      : [sepc] "r"(USER_BASE), [sstatus] "r"(SSTATUS_SPIE | SSTATUS_SUM));
}

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
 * @brief 프로세스 생성, 지정된 크기만큼 실행된 이미지를 페이지 단위로 복사하여
 * 프로세스의 페이지 테이블에 매핑
 *
 * @param image 실행 이미지의 포인터
 * @param image_size 이미지 크기
 * @return struct process* 생성된 프로세스 구조체의 주소
 */
struct process *create_process(const void *image, size_t image_size) {
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
  *--sp = 0;                    // s11
  *--sp = 0;                    // s10
  *--sp = 0;                    // s9
  *--sp = 0;                    // s8
  *--sp = 0;                    // s7
  *--sp = 0;                    // s6
  *--sp = 0;                    // s5
  *--sp = 0;                    // s4
  *--sp = 0;                    // s3
  *--sp = 0;                    // s2
  *--sp = 0;                    // s1
  *--sp = 0;                    // s0
  *--sp = (uint32_t)user_entry; // ra (처음 실행 시 점프할 주소)

  uint32_t *page_table = (uint32_t *)alloc_pages(1);

  // 커널 메모리 영역을 일대일 방식으로 매핑
  for (paddr_t paddr = (paddr_t)__kernel_base; paddr < (paddr_t)__free_ram_end;
       paddr += PAGE_SIZE)
    map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);

  // virtio 블록 디바이스의 메모리 영역도 프로세스 페이지 테이블에 매핑
  map_page(page_table, VIRTIO_BLK_PADDR, VIRTIO_BLK_PADDR, PAGE_R | PAGE_W);

  // 루프를 사용하여 이미지를 페이지 단위로 처리
  for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
    // 새 물리 메모리 페이지 할당
    paddr_t page = alloc_pages(1);

    // 현재 오프셋에서 복사할 크기를 계산
    size_t remaining = image_size - off;
    size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;

    // 이미지 데이터를 새로 할당된 페이지에 복사
    memcpy((void *)page, image + off, copy_size);

    // 가상 메모리 주소를 물리 메모리에 매핑
    map_page(page_table, USER_BASE + off, page,
             PAGE_U | PAGE_R | PAGE_W | PAGE_X);
  }

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

// SBI 호출을 통해 문자 입력을 받음
long getchar(void) {
  struct sbiret ret = sbi_call(0, 0, 0, 0, 0, 0, 0, 2);
  return ret.error;
}

// 파일명을 기준으로 파일을 검색
struct file *fs_lookup(const char *filename) {
  for (int i = 0; i < FILES_MAX; i++) {
    struct file *file = &files[i];
    if (!strcmp(file->name, filename))
      return file;
  }

  return NULL;
}

// TAR 형식으로 파일 시스템을 구성하고 가상 블록 장치에 저장
void fs_flush(void) {

  // 모든 파일 내용을 'disk' 버퍼에 복사
  memset(disk, 0, sizeof(disk));

  // 현재 디스크 오프셋 위치를 추적
  unsigned off = 0;

  // 모든 파일을 순회하며 TAR 형식으로 디스크에 저장
  for (int file_i = 0; file_i < FILES_MAX; file_i++) {
    struct file *file = &files[file_i];
    if (!file->in_use) // 사용중이지 않으면 건너뜀
      continue;

    // TAR 헤더 구성
    struct tar_header *header = (struct tar_header *)&disk[off];
    memset(header, 0, sizeof(*header));
    strcpy(header->name, file->name);
    strcpy(header->mode, "000644");
    strcpy(header->magic, "ustar");
    strcpy(header->version, "00");
    header->type = '0';

    // 파일 크기를 8진수 문자열로 변환하여 헤더에 설정
    int filesz = file->size;
    for (int i = sizeof(header->size); i > 0; i--) {
      header->size[i - 1] = (filesz % 8) + '0';
      filesz /= 8;
    }

    // 헤더 체크섬 계산
    int checksum = ' ' * sizeof(header->checksum);
    for (unsigned i = 0; i < sizeof(struct tar_header); i++)
      checksum += (unsigned char)disk[off + i];

    // 계산된 체크섬을 8진수 문자열로 변환하여 헤더에 설정
    for (int i = 5; i >= 0; i--) {
      header->checksum[i] = (checksum % 8) + '0';
      checksum /= 8;
    }

    // 파일 데이터를 헤더 뒤에 복사
    memcpy(header->data, file->data, file->size);

    // 다음 파일을 위해 오프셋 업데이트 (섹터 크기에 맞게 정렬)
    off += align_up(sizeof(struct tar_header) + file->size, SECTOR_SIZE);
  }

  // disk 버퍼의 내용을 virtio-blk 디바이스에 기록
  for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
    read_write_disk(&disk[sector * SECTOR_SIZE], sector, true);

  printf("wrote %d bytes to disk\n", sizeof(disk));
}

// 시스템 콜의 종류를 판별하여 처리
void handle_syscall(struct trap_frame *f) {
  // 시스템 콜 번호가 담긴 a3 레지스터 확인
  // ref: user.c, syscall 함수
  switch (f->a3) {
  case SYS_EXIT:
    printf("process %d exited\n", current_proc->pid);
    current_proc->state = PROC_EXITED;
    yield();
    PANIC("unreachable");
  case SYS_GETCHAR:
    while (1) {
      long ch = getchar();
      if (ch >= 0) {
        f->a0 = ch;
        break;
      }

      // 논블로킹 I/O, CPU 양보
      yield();
    }
    break;

  case SYS_PUTCHAR:
    putchar(f->a0);
    break;
  case SYS_READFILE:
  case SYS_WRITEFILE: {
    const char *filename = (const char *)f->a0;
    char *buf = (char *)f->a1;
    int len = f->a2;
    struct file *file = fs_lookup(filename);
    if (!file) {
      printf("file not found: %s\n", filename);
      f->a0 = -1;
      break;
    }

    if (len > (int)sizeof(file->data))
      len = file->size;

    if (f->a3 == SYS_WRITEFILE) {
      memcpy(file->data, buf, len);
      file->size = len;
      fs_flush();
    } else {
      memcpy(buf, file->data, len);
    }

    f->a0 = len;
    break;
  }
  default:
    PANIC("unexpected syscall a3=%x\n", f->a3);
  }
}

// 트랩 핸들러
void handle_trap(struct trap_frame *f) {
  // 트랩의 원인, (어떤 이유로 예외가 발생했는지)
  uint32_t scause = READ_CSR(scause);
  // 트랩과 관련된 추가 정보 (예외 부가정보, ex.잘못된 메모리 주소...)
  uint32_t stval = READ_CSR(stval);
  // 트랩이 발생한 명령어의 주소 (예외가 일어난 시점의 PC)
  uint32_t user_pc = READ_CSR(sepc);

  // 시스템 콜인 경우
  if (scause == SCAUSE_ECALL) {
    handle_syscall(f);
    user_pc += 4;
  } else {
    PANIC("unexpected trap scause=%x, stval=%x, sepc=%x\n", scause, stval,
          user_pc);
  }

  WRITE_CSR(sepc, user_pc);
}

// virtio 블록 디바이스의 32비트 레지스터 값 읽기
uint32_t virtio_reg_read32(unsigned offset) {
  return *((volatile uint32_t *)(VIRTIO_BLK_PADDR + offset));
}

// virtio 블록 디바이스의 64비트 레지스터 값 읽기
uint64_t virtio_reg_read64(unsigned offset) {
  return *((volatile uint64_t *)(VIRTIO_BLK_PADDR + offset));
}

// virtio 블록 디바이스의 32비트 레지스터에 값을 쓰기
void virtio_reg_write32(unsigned offset, uint32_t value) {
  *((volatile uint32_t *)(VIRTIO_BLK_PADDR + offset)) = value;
}

// Read-Modify-Write (RMW) 연산
// 레지스터의 현재 값을 읽고 지정된 비트들을 OR 연산 설정 후 쓰기 작업
void virtio_reg_fetch_and_or32(unsigned offset, uint32_t value) {
  virtio_reg_write32(offset, virtio_reg_read32(offset) | value);
}

// 블록 장치에 요청을 전송하기 위한 가상 큐 포인터
struct virtio_virtq *blk_request_vq;
// 블록 장치 요청 구조체로, 읽기/쓰기 명령과 관련 데이터를 포함
struct virtio_blk_req *blk_req;
// 블록 요청 구조체의 물리 주소
paddr_t blk_req_paddr;
// 블록 장치의 총 용량(바이트 단위)
unsigned blk_capacity;

void virtio_blk_init(void) {
  // 0x74726976은 ASCII로 "virv"이며, VirtIO 장치임을 확인하는 매직값
  if (virtio_reg_read32(VIRTIO_REG_MAGIC) != 0x74726976)
    PANIC("virtio: invalid magic value");
  // 버전과 장치 ID 검증
  if (virtio_reg_read32(VIRTIO_REG_VERSION) != 1)
    PANIC("virtio: invalid version");
  if (virtio_reg_read32(VIRTIO_REG_DEVICE_ID) != VIRTIO_DEVICE_BLK)
    PANIC("virtio: invalid device id");

  // 1. 장치 리셋 (레지스터를 0으로 초기화)
  virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, 0);
  // 2. ACKNOWLEDGE 상태 비트를 설정: 게스트 OS가 장치를 인식했음을 알림
  virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);
  // 3. DRIVER 상태 비트를 설정
  virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER);
  // 4. FEATURES_OK 상태 비트를 설정
  virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FEAT_OK);
  // 5. 장치별 설정 수행 (예, virtqueue 검색)
  blk_request_vq = virtq_init(0);
  // 6. DRIVER_OK 상태 비트를 설정
  virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER_OK);

  // 디스크 용량을 가져옴
  blk_capacity = virtio_reg_read64(VIRTIO_REG_DEVICE_CONFIG + 0) * SECTOR_SIZE;
  printf("virtio-blk: capacity is %d bytes\n", blk_capacity);

  // 장치에 요청(request)을 저장할 영역 할당
  blk_req_paddr =
      alloc_pages(align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
  blk_req = (struct virtio_blk_req *)blk_req_paddr;
}

// desc_index는 새로운 요청의 디스크립터 체인의 헤드 디스크립터 인덱스
// 장치에 새로운 요청이 있음을 알림
void virtq_kick(struct virtio_virtq *vq, int desc_index) {
  vq->avail.ring[vq->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
  vq->avail.index++;

  // 메모리 배리어는 이전의 메모리 작업이 이후 작업전(장치 알림)에 완료됨을 보장
  __sync_synchronize();

  virtio_reg_write32(VIRTIO_REG_QUEUE_NOTIFY, vq->queue_index);
  vq->last_used_index++;
}

// 장치가 요청을 처리 중인지 확인
bool virtq_is_busy(struct virtio_virtq *vq) {
  return vq->last_used_index != *vq->used_index;
}

/**
 * @brief virtqueue를 위한 메모리 영역을 할당하고 물리적 주소를 장치에 알림
 *
 * @param index 초기화할 virtqueue의 번호
 * @return struct virtio_virtq*
 */
struct virtio_virtq *virtq_init(unsigned index) {
  // 메모리 할당
  paddr_t virtq_paddr =
      alloc_pages(align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);

  // 가상 큐 초기화
  struct virtio_virtq *vq = (struct virtio_virtq *)virtq_paddr;
  vq->queue_index = index;
  vq->used_index = (volatile uint16_t *)&vq->used.index;

  // 1. QueueSel 레지스터에 인덱스를 기록하여 큐 선택
  virtio_reg_write32(VIRTIO_REG_QUEUE_SEL, index);
  // 2. QueueNum 레지스터에 큐의 크기를 기록하여 장치에 알림
  virtio_reg_write32(VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);
  // 3. QueueAlign 레지스터에 정렬값(바이트 단위)을 기록
  virtio_reg_write32(VIRTIO_REG_QUEUE_ALIGN, 0);
  // 4. 할당한 큐 메모리의 첫 페이지의 물리적 번호를 QueuePFN 레지스터에 기록
  virtio_reg_write32(VIRTIO_REG_QUEUE_PFN, virtq_paddr);
  return vq;
}

// virtio-blk 장치로부터 읽기/쓰기를 수행
void read_write_disk(void *buf, unsigned sector, int is_write) {
  if (sector >= blk_capacity / SECTOR_SIZE) {
    printf("virtio: tried to read/write sector=%d, but capacity is %d\n",
           sector, blk_capacity / SECTOR_SIZE);
    return;
  }

  // virtio-blk 사양에 따라 요청을 구성
  blk_req->sector = sector;
  blk_req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  if (is_write)
    memcpy(blk_req->data, buf, SECTOR_SIZE);

  // virtqueue 디스크립터를 구성 (3개의 디스크립터 사용)
  struct virtio_virtq *vq = blk_request_vq;
  vq->descs[0].addr = blk_req_paddr;
  vq->descs[0].len = sizeof(uint32_t) * 2 + sizeof(uint64_t);
  vq->descs[0].flags = VIRTQ_DESC_F_NEXT;
  vq->descs[0].next = 1;

  vq->descs[1].addr = blk_req_paddr + offsetof(struct virtio_blk_req, data);
  vq->descs[1].len = SECTOR_SIZE;
  vq->descs[1].flags = VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
  vq->descs[1].next = 2;

  vq->descs[2].addr = blk_req_paddr + offsetof(struct virtio_blk_req, status);
  vq->descs[2].len = sizeof(uint8_t);
  vq->descs[2].flags = VIRTQ_DESC_F_WRITE;

  // 장치에 새로운 요청이 있음을 알림
  virtq_kick(vq, 0);

  // 장치가 요청 처리를 마칠 때까지 대기(바쁜 대기; busy-wait)
  while (virtq_is_busy(vq))
    ;

  // virtio-blk: 0이 아닌 값이 반환되면 에러
  if (blk_req->status != 0) {
    printf("virtio: warn: failed to read/write sector=%d status=%d\n", sector,
           blk_req->status);
    return;
  }

  // 읽기 작업의 경우, 데이터를 버퍼에 복사
  if (!is_write)
    memcpy(buf, blk_req->data, SECTOR_SIZE);
}

// 파일 시스템의 파일 테이블
struct file files[FILES_MAX];
// 디스크 이미지를 메모리에 로드하기 위한 버퍼
uint8_t disk[DISK_MAX_SIZE];

/**
 * @brief 8진수(octal) 문자열을 10진수(decimal) 정수로 변환
 *
 * @param oct 8진수 문자열의 포인터
 * @param len 문자열의 최대 길이
 * @return int
 */
int oct2int(char *oct, int len) {
  int dec = 0;
  for (int i = 0; i < len; i++) {
    if (oct[i] < '0' || oct[i] > '7')
      break;
    dec = dec * 8 + (oct[i] - '0');
  }
  return dec;
}

// 파일 시스템 초기화
// 1. 전체 디스크 내용을 메모리에 로드
// 2. TAR 형식의 헤더를 순차적으로 파싱
// 3. 각 파일의 메타데이터(이름, 크기 등)을 추출
// 4. 파일 데이터를 메모리 내 파일 시스템 구조에 로드
// 5. 모든 파일을 처리하거나 비어있는 헤더를 만나면 초기화 완료
void fs_init(void) {
  // 디스크 데이터 로드
  for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
    read_write_disk(&disk[sector * SECTOR_SIZE], sector, false);

  // TAR 파일 구조 파싱
  unsigned off = 0;
  for (int i = 0; i < FILES_MAX; i++) {

    // TAR 파일 헤더 검사
    struct tar_header *header = (struct tar_header *)&disk[off];
    if (header->name[0] == '\0')
      break;

    // TAR 포맷 검증
    if (strcmp(header->magic, "ustar") != 0)
      PANIC("invalid tar header: magix=\"%s\"", header->magic);

    // 파일 크기 추출 및 파일 정보 저장
    int filesz = oct2int(header->size, sizeof(header->size));
    struct file *file = &files[i];
    file->in_use = true;
    strcpy(file->name, header->name);
    memcpy(file->data, header->data, filesz);
    file->size = filesz;
    printf("file: %s, size=%d\n", file->name, file->size);

    // 다음 파일 헤더로 이동
    off += align_up(sizeof(struct tar_header) + filesz, SECTOR_SIZE);
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

  virtio_blk_init();
  fs_init();

  char buf[SECTOR_SIZE];
  read_write_disk(buf, 0, false /* read from the disk */);
  printf("first sector: %s\n", buf);

  strcpy(buf, "hello from kernel!!!\n");
  read_write_disk(buf, 0, true /* write to the disk */);

  idle_proc = create_process(NULL, 0);
  idle_proc->pid = 0; // idle
  current_proc = idle_proc;

  create_process(_binary_shell_bin_start, (size_t)_binary_shell_bin_size);

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
