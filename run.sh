#!/bin/bash

set -xue

QEMU=qemu-system-riscv32
OBJCOPY=/opt/homebrew/opt/llvm/bin/llvm-objcopy

# clang 경로, 컴파일 옵션
CC=/opt/homebrew/opt/llvm/bin/clang
CFLAGS=(
  -std=c11                     # C11 표준 사용
  -O2                          # 최적화 레벨 2
  -g3                          # 최대한의 디버그 정보 생성
  -Wall                        # 핵심 경고 활성화
  -Wextra                      # 추가 경고 활성화
  --target=riscv32-unknown-elf # 32비트 RISC-V 대상 아키텍처
  -fno-stack-protector         # 스택 보호 기능 비활성화
  -ffreestanding               # 호스트 표준 라이브러리를 사용하지 않음
  -nostdlib                    # 표준 라이브러리를 링크하지 않음
)

# 애플리케이션 빌드
$CC "${CFLAGS[@]}" \
  -Wl,-Tuser.ld \
  -Wl,-Map=shell.map \
  -o shell.elf \
  shell.c user.c common.c

# ELF 형식의 실행 파일을 실제 메모리 내용만 포함하는 바이너리로 변환
$OBJCOPY --set-section-flags .bss=alloc,contents -O binary shell.elf shell.bin
$OBJCOPY -Ibinary -Oelf32-littleriscv shell.bin shell.bin.o

# 커널 빌드
$CC "${CFLAGS[@]}" \
  -Wl,-Tkernel.ld \
  -Wl,-Map=kernel.map \
  -o kernel.elf \
  kernel.c common.c shell.bin.o

(cd disk && tar cf ../disk.tar --format=ustar -- *.txt)

# virt 머신 시작
# QEMU가 제공하는 기본 펌웨어(OpenSBI)를 사용
# GUI 없이 콘솔만
$QEMU -machine virt -bios default -nographic -serial mon:stdio --no-reboot \
  -kernel kernel.elf \
  -d unimp,guest_errors,int,cpu_reset -D qemu.log \
  -device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0 -drive id=drive0,file=lorem.txt,format=raw,if=none \
  -drive id=drive0,file=disk.tar,format=raw,if=none
