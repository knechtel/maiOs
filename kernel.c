#include <stdint.h>

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_COLOR  0x0F00u /* texto branco sobre fundo preto */

static void print_vga(const char *message)
{
    for (uint32_t i = 0; message[i] != '\0'; ++i) {
        VGA_MEMORY[i] = (uint16_t)(VGA_COLOR | (uint8_t)message[i]);
    }
}

void kernel_main(void)
{
    print_vga("Ola do kernel C rodando no QEMU!");

    /* Mantem a CPU parada depois de inicializar. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
