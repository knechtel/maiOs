# Kernel mínimo em C no QEMU

Este exemplo inicializa um kernel Multiboot 1 de 32 bits com um terminal
interativo de texto VGA. O kernel lê o teclado PS/2 e oferece comandos internos
simples; o pequeno arquivo assembly prepara a pilha e chama `kernel_main`.

Comandos disponíveis: `help`, `clear`, `echo`, `about`, `ls`, `pwd`, `mkdir`,
`write`, `cat`, `halt` e `reboot`. O armazenamento usa setores ATA PIO e uma
imagem IDE (`disk.img`) criada automaticamente pelo `make run`. O sistema
mantém até 16 arquivos de até 512 bytes, com nomes de até 15 caracteres.
Os dados continuam na imagem quando o QEMU é fechado. `mkdir NOME` salva até
8 diretórios no disco, com nomes de até 21 caracteres.

Exemplos:

```text
write notas oi mundo
cat notas
ls
```

## Requisitos

- GCC com suporte a compilação `-m32`
- GNU `ld`, `make` e `qemu-system-i386`

## Compilar e executar

```sh
make
make run
```

O terminal aparece na tela do emulador. Para sair do QEMU, use `Ctrl+C` no
terminal ou `Ctrl+Alt+G` para liberar o mouse e depois feche a janela.

Se sua instalação do QEMU não aceitar um ELF Multiboot diretamente com
`-kernel`, será necessário empacotar o mesmo `kernel.elf` em uma imagem de
boot Multiboot (por exemplo, com GRUB).
# maiOs
