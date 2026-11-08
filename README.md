# Kernel mínimo em C no QEMU

Este exemplo inicializa um kernel Multiboot 1 de 32 bits com um terminal
interativo de texto VGA. O kernel lê o teclado PS/2 e oferece comandos internos
simples; o pequeno arquivo assembly prepara a pilha e chama `kernel_main`.

Comandos disponíveis: `help`, `clear`, `echo`, `about`, `ls`, `pwd`, `mkdir`,
`halt` e `reboot`. O shell ainda não possui sistema de arquivos persistente:
`mkdir NOME` registra diretórios apenas na memória, e `ls` lista os diretórios
criados. Eles desaparecem quando o kernel é reiniciado; `pwd` sempre retorna
`/`.

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
