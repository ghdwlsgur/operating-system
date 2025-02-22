#pragma once
#include "common.h"

// Sv32 방식의 페이지 테이블
#define SATP_SV32 (1u << 31) // Sv32 모드 페이징 활성화
#define PAGE_V (1 << 0)      // "Valid" 비트 (엔트리가 유효함을 의미)
#define PAGE_R (1 << 1)      // 읽기 가능
#define PAGE_W (1 << 2)      // 쓰기 가능
#define PAGE_X (1 << 3)      // 실행 가능
#define PAGE_U (1 << 4)      // 사용자 모드 접근 가능

/**
 * @brief 예외 처리 시 저장된 레지스터들의 구조체 정의
 * ra, 리턴 주소
 * gp, 전역 포인터
 * tp, 스레드 포인터
 * t0 ~ t6, 임시 레지스터
 * a0 ~ a7, 인자 레지스터
 * s0 ~ s11, 저장 레지스터
 * sp, 스택 포인터 */
struct trap_frame {
  uint32_t ra;
  uint32_t gp;
  uint32_t tp;
  uint32_t t0;
  uint32_t t1;
  uint32_t t2;
  uint32_t t3;
  uint32_t t4;
  uint32_t t5;
  uint32_t t6;
  uint32_t a0;
  uint32_t a1;
  uint32_t a2;
  uint32_t a3;
  uint32_t a4;
  uint32_t a5;
  uint32_t a6;
  uint32_t a7;
  uint32_t s0;
  uint32_t s1;
  uint32_t s2;
  uint32_t s3;
  uint32_t s4;
  uint32_t s5;
  uint32_t s6;
  uint32_t s7;
  uint32_t s8;
  uint32_t s9;
  uint32_t s10;
  uint32_t s11;
  uint32_t sp;

  // 컴파일러에게 메모리 정렬(padding)을 하지 말라고 지시
  // 구조체의 각 멤버가 연속된 메모리에 빈틈없이 배치
  // 기본적으로 컴파일러는 성능을 위해 자동으로 정렬하지만
  // 이 구조체는 정확한 메모리 레이아웃이 필요
} __attribute__((packed));

/**
 * @brief CSR 레지스터를 읽는 매크로
 * CSR (Control and Status Register), RISC-V에서 프로세서의 상태를 제어하고
 * 모니터링하는 특수 목적 레지스터
 */
#define READ_CSR(reg)                                                          \
  ({                                                                           \
    unsigned long __tmp;                                                       \
    __asm__ __volatile__("csrr %0, " #reg : "=r"(__tmp));                      \
    __tmp;                                                                     \
  })

/**
 * @brief CSR 레지스터에 값을 쓰는 매크로
 *
 */
#define WRITE_CSR(reg, value)                                                  \
  do {                                                                         \
    uint32_t __tmp = (value);                                                  \
    __asm__ __volatile__("csrw " #reg ", %0" ::"r"(__tmp));                    \
  } while (0)

struct sbiret {
  long error;
  long value;
};

/**
 * @brief 커널 패닉 매크로
 * PANIC 매크로를 무한 루프로 끝내는 이유?
 * - 시스템 안정성
   심각한 오류 발생 시 시스템이 불안정한 상태로 계속 실행되는 것을 방지
   예측할 수 없는 동작이나 추가 손상을 막음
 * - 디버깅 용이성
   프로그램이 즉시 종료되는 대신 무한 루프에 걸리므로 디버거를 연결하여 상태
   검사 오류 발생 시점의 메모리와 레지스터의 상태를 확인
 * - 하드웨어 특성
   임베디드 시스템에서는 단순히 exit()를 호출하는 것이 적절하지 않을 수 있음
   워치독 타이머가 있는 경우 무한 루프는 시스템 리셋을 트리거 */
#define PANIC(fmt, ...)                                                        \
  do {                                                                         \
    printf("PANIC: %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);      \
    while (1) {                                                                \
    }                                                                          \
  } while (0)
