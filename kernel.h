#pragma once
#include "common.h"

// virtio 관련 매크로
#define SECTOR_SIZE 512    // 디스크 섹터 크기 512바이트
#define VIRTQ_ENTRY_NUM 16 // 가상 큐 엔트리 수 16개

#define VIRTIO_DEVICE_BLK 2         // virtio 블록 디바이스의 ID
#define VIRTIO_BLK_PADDR 0x10001000 // virtio 블록 디바이스의 물리적 주소

#define VIRTIO_REG_MAGIC 0x00     // virtio 디바이스의 매직 넘버 레지스터 오프셋
#define VIRTIO_REG_VERSION 0x04   // virtio 디바이스의 버전 정보 레지스터 오프셋
#define VIRTIO_REG_DEVICE_ID 0x08 // 디바이스 ID 레지스터 오프셋
#define VIRTIO_REG_QUEUE_SEL 0x30 // 큐 선택 레지스터 오프셋
#define VIRTIO_REG_QUEUE_NUM_MAX 0x34  // 큐의 최대 크기 레지스터 오프셋
#define VIRTIO_REG_QUEUE_NUM 0x38      // 큐 크기 설정 레지스터 오프셋
#define VIRTIO_REG_QUEUE_ALIGN 0x3c    // 큐 정렬 설정 레지스터 오프셋
#define VIRTIO_REG_QUEUE_PFN 0x40      // 큐의 물리적 페이지 프레임 번호
#define VIRTIO_REG_QUEUE_READY 0x44    // 큐 준비 상태
#define VIRTIO_REG_QUEUE_NOTIFY 0x50   // 큐 알림
#define VIRTIO_REG_DEVICE_STATUS 0x70  // 디바이스 상태
#define VIRTIO_REG_DEVICE_CONFIG 0x100 // 디바이스 설정

#define VIRTIO_STATUS_ACK 1       // 디바이스 발견 확인
#define VIRTIO_STATUS_DRIVER 2    // 드라이버가 디바이스 사용 준비 완료
#define VIRTIO_STATUS_DRIVER_OK 4 // 드라이버 정상 작동
#define VIRTIO_STATUS_FEAT_OK 8   // 드라이버와 디바이스 기능 협상 완료

#define VIRTQ_DESC_F_NEXT 1          // 다음 디스크립터가 존재
#define VIRTQ_DESC_F_WRITE 2         // 디바이스가 이 버퍼에 쓰기 작업을 수행
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1 // 인터럽트 비활성화

#define VIRTIO_BLK_T_IN 0  // 블록 디바이스에서 데이터를 읽음
#define VIRTIO_BLK_T_OUT 1 // 블록 디바이스에 데이터를 씀

// virtqueue 디스크립터 엔트리
struct virtq_desc {
  uint64_t addr;  // 버퍼의 물리적 주소
  uint32_t len;   // 버퍼의 길이
  uint16_t flags; // 디스크립터의 속성 플래그
  uint16_t next;  // 체인에서의 다음 디스크립터 인덱스
} __attribute((packed));

// virtqueue available ring
struct virtq_avail {
  uint16_t flags; // 플래그
  uint16_t index; // 드라이버가 다음에 추가할 ring 엔트리의 인덱스
  uint16_t ring[VIRTQ_ENTRY_NUM]; // 이용 가능한 디스크립터의 인덱스 배열
} __attribute__((packed));

// virtqueue used ring 엔트리
struct virtq_used_elem {
  uint32_t id;  // 처리된 디스크립터 체인 인덱스
  uint32_t len; // 디바이스가 쓴 바이트 수
} __attribute__((packed));

// virtqueue used ring
struct virtq_used {
  uint16_t flags;
  uint16_t index; // 디바이스가 다음에 추가할 ring 엔트리의 인덱스
  struct virtq_used_elem
      ring[VIRTQ_ENTRY_NUM]; // 사용 완료된 디스크립터 엔트리 배열
} __attribute__((packed));

// virtqueue
struct virtio_virtq {
  struct virtq_desc descs[VIRTQ_ENTRY_NUM];                 // 디스크립터 배열
  struct virtq_avail avail;                                 // available ring
  struct virtq_used used __attribute((aligned(PAGE_SIZE))); // used ring
  int queue_index;                                          // 큐의 인덱스
  volatile uint16_t *used_index; // used 인덱스에 대한 포인터 (변경 감지)
  uint16_t last_used_index;      // 마지막으로 처리된 used 인덱스
} __attribute__((packed));

// virtio-blk 요청 구조체
struct virtio_blk_req {
  uint32_t type;     // 요청 타입
  uint32_t reserved; // 예약된 필드
  uint64_t sector;   // 접근하려는 디스크 섹터 번호
  uint8_t data[512]; // 데이터 버퍼
  uint8_t status;    // 작업의 결과 상태
} __attribute__((packed));

// 예외 트랩 핸들러
#define SCAUSE_ECALL 8
#define PROC_EXITED 2

// Sv32 방식의 페이지 테이블
#define SATP_SV32 (1u << 31) // Sv32 모드 페이징 활성화
#define PAGE_V (1 << 0)      // "Valid" 비트 (엔트리가 유효함을 의미)
#define PAGE_R (1 << 1)      // 읽기 가능
#define PAGE_W (1 << 2)      // 쓰기 가능
#define PAGE_X (1 << 3)      // 실행 가능
#define PAGE_U (1 << 4)      // 사용자 모드 접근 가능

// 애플리케이션 이미지의 기본 가상 주소
#define USER_BASE 0x1000000

#define SSTATUS_SPIE (1 << 5)

struct process {
  int pid;              // 프로세스 ID
  int state;            // 프로세스 상태: PROC_UNUSED 또는 PROC_RUNNABLE
  vaddr_t sp;           // 스택 포인터
  uint32_t *page_table; // 프로세스 1단계 페이지 테이블
  uint8_t stack[8192];  // 커널 스택 (CPU 레지스터, 함수 리턴 주소, 로컬 변수)
};

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
