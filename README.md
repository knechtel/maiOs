# Kernel mínimo em C no QEMU

Este exemplo inicializa um kernel Multiboot 1 de 32 bits com um terminal
interativo de texto VGA. O kernel lê o teclado PS/2 e oferece comandos internos
simples; o pequeno arquivo assembly prepara a pilha e chama `kernel_main`.

Comandos disponíveis: `help`, `clear`, `echo`, `about`, `ls`, `pwd`, `mkdir`,
`write`, `cat`, `halt` e `reboot`. O kernel lê e grava uma imagem ext4 por
setores ATA PIO. `make run` cria `ext4.img` separadamente, preservando a antiga
`disk.img`. Arquivos gravados pelo maiOs podem ter até 1024 bytes.

O driver inicial suporta uma configuração ext4 simples de um grupo, blocos de
1024 bytes e endereçamento direto. Ele não suporta journal, extents, checksums,
partições, subdiretórios navegáveis ou sistemas de arquivos com recursos
avançados. A imagem criada pelo Makefile desativa esses recursos. Sem journal,
uma queda de energia durante a gravação pode corromper metadados. Evite montar
a imagem simultaneamente no Linux e no QEMU.

Exemplos:

```text
write notas oi mundo
cat notas
ls
```

## Requisitos

- GCC com suporte a compilação `-m32`
- GNU `ld`, `make` e `qemu-system-i386`
- `mke2fs` (pacote e2fsprogs), para criar `ext4.img`

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
