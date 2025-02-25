ENTRY(start)

SECTIONS {
  /* 현재 위치 카운터를 0x1000000 (16MB) 주소로 설정 */
  . = 0x1000000;

  /* 실행 코드를 포함 */
  .text :{
    KEEP(*(.text.start));
    *(.text .text.*);
  }

  /* 읽기 전용 데이터(상수, 문자열 등)를 포함하는 섹션 */
  .rodata : ALIGN(4) {
    *(.rodata .rodata.*);
  }

  /* 초기화된 전역/정적 변수를 포함하는 섹션 */
  .data : ALIGN(4) {
    *(.data .data.*);
  }

  /* 초기화되지 않은 전역/정적 변수를 포함하는 섹션 */
  .bss : ALIGN(4) {
    *(.bss .bss.* .sbss .sbss.*);

    . = ALIGN(16); /* 현재 위치를 16바이트 경계에 정렬 */
    . += 64 * 1024; /* 현재 위치에 64KB(스택 영역)를 추가 */
    __stack_top = .;

    /* 최종 위치가 0x1800000(24MB)보다 작은지 확인 */
    ASSERT(. < 0x1800000, "too large executable");
  }
}