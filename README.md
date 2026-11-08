# Kernel mínimo em C no QEMU

Este exemplo inicializa um kernel Multiboot 1 de 32 bits. O código C escreve
uma mensagem diretamente na memória de texto VGA; o pequeno arquivo assembly
prepara a pilha e chama `kernel_main`.

## Requisitos

- GCC com suporte a compilação `-m32`
- GNU `ld`, `make` e `qemu-system-i386`

## Compilar e executar

```sh
make
make run
```

A mensagem aparece no início da tela do emulador. Para sair do QEMU, use
`Ctrl+C` no terminal ou `Ctrl+Alt+G` para liberar o mouse e depois feche a
janela.

Se sua instalação do QEMU não aceitar um ELF Multiboot diretamente com
`-kernel`, será necessário empacotar o mesmo `kernel.elf` em uma imagem de
boot Multiboot (por exemplo, com GRUB).
# maiOs
