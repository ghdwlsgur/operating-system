
typedef unsigned char uint8_t; // 0 ~ 2^8-1 (255)
typedef unsigned int uint32_t; // 0 ~ 2^32-1
typedef uint32_t size_t;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-library-redeclaration"
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

/* 외부 심볼 선언
 * __bss ~ __bss_end: 초기화되지 않은 전역 변수가 저장될 메모리 영역
 * __stack_top: 스택의 최상단 주소
 * 실제로는 "해당 심볼이 가리키는 주소"가 필요하기 때문에 []를 사용하여 주소
 * 형식으로 사용 */
extern char __bss[], __bss_end[], __stack_top[];

// 메모리의 특정 영역을 지정된 값으로 초기화 (메모리 초기화)
void *memset(void *buf, char c, size_t n) {
  uint8_t *p = (uint8_t *)buf;
  while (n--)
    *p++ = c;
  return buf;
}

// 커널 메인 함수
void kernel_main(void) {
  /* BSS 영역 초기화 (BSS 영역만큼 버퍼를 0으로 채움)
   * 일부 부트로더가 .bss를 클리어해주기도 하지만 여러 환경에서 확실히 동작하게
   * 하려면 수동으로 초기화 하는 것이 안전 */
  memset(__bss, 0, (size_t)__bss_end - (size_t)__bss);

  // 무한 루프 (커널 종료 방지)
  for (;;)
    ;
}

// boot() 함수를 섹션 (__text_boot)에 배치
__attribute__((section(".text.boot")))
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

/* 일
1. 부트 코드 실행 (boot()), sp를 __stack_top으로 설정, kernel_main() 함수로 점프
2. kernel_main()에서 BSS 영역 초기화 및 무한 루프 실행
*/

#pragma GCC diagnostic pop