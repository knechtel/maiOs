CC = gcc
AS = gcc
LD = ld
CFLAGS = -m32 -std=c11 -ffreestanding -fno-pie -fno-stack-protector \
         -fno-asynchronous-unwind-tables -Wall -Wextra -O2
ASFLAGS = -m32 -ffreestanding -fno-pie
LDFLAGS = -m elf_i386 -T linker.ld

.PHONY: all run clean

all: kernel.elf

kernel.elf: boot.o kernel.o linker.ld
	$(LD) $(LDFLAGS) -o $@ boot.o kernel.o

boot.o: boot.s
	$(AS) $(ASFLAGS) -c $< -o $@

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

run: kernel.elf
	qemu-system-i386 -kernel kernel.elf

clean:
	rm -f boot.o kernel.o kernel.elf
