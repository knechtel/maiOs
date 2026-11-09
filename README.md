# Kernel mínimo em C no QEMU

Este exemplo inicializa um kernel Multiboot 1 de 32 bits com um terminal
interativo de texto VGA. O kernel lê o teclado PS/2 e oferece comandos internos
simples; o pequeno arquivo assembly prepara a pilha e chama `kernel_main`.
O mapa do kernel segue ABNT2, com Shift, Caps Lock e AltGr (Alt direito).
`make run` passa `-k pt-br` ao QEMU. O terminal VGA ainda não oferece suporte
Unicode geral para acentos.

Comandos disponíveis: `help`, `clear`, `echo`, `about`, `ls`, `cd`, `pwd`,
`mkdir`, `write`, `cat`, `halt` e `reboot`. O kernel lê e grava uma imagem ext4
por setores ATA PIO. `make run` cria e conecta `ext4-2g.img`, uma imagem ext4
de 2 GiB, preservando `disk.img` e `ext4.img` preexistentes.
O driver mapeia blocos diretos e indiretos simples, duplos e triplos.

Para gravar várias linhas, use `write NOME`, digite o conteúdo e finalize com um
ponto sozinho em uma linha. `cat NOME` lê os blocos do arquivo em sequência.

O driver suporta até 256 grupos ext4 com blocos de 1024 bytes. Ele não suporta
journal, extents, checksums ou partições. A navegação por diretórios é limitada
a 16 níveis. A imagem criada pelo Makefile desativa journal, extents e checksums.
Sem journal, uma queda de energia durante a gravação pode corromper metadados.
Evite montar a imagem simultaneamente no Linux e no QEMU.

O QEMU conecta uma E1000 com rede de usuário. No boot, o kernel procura a placa
PCI Intel 8086:100E, habilita acesso à memória e bus mastering, reseta o
dispositivo e mostra o endereço MAC. O driver configura anéis TX/RX de oito
descritores e oferece `e1000_send` e `e1000_receive` com polling. A camada de
rede implementa ARP, cabeçalhos/checksum IPv4 e ICMP Echo (`ping`), com
configuração estática `10.0.2.15/24` e gateway `10.0.2.2`; `ifconfig` mostra
essa configuração e `ping` sem argumento testa o gateway. A pilha também faz
configuração DHCP no boot (com fallback estático), transporte UDP e consulta DNS
de registros IPv4 tipo A. Use `dhcp` para renovar a configuração, `ifconfig` para
ver IP/máscara/gateway/DNS, e `dns exemplo.com` para resolver um nome. TCP
implementa conexão de saída com handshake, envio/recepção de segmentos e
encerramento para uma conexão por vez. O comando `tcp IP PORT` testa o handshake
e fecha a conexão; suporte a servidor/listen, retransmissão avançada e múltiplas
conexões ainda não está implementado.

Exemplos:

```text
write notas oi mundo
cat notas
ls
mkdir docs
cd docs
pwd
cd ..
```

## Requisitos

- GCC com suporte a compilação `-m32`
- GNU `ld`, `make` e `qemu-system-i386`
- `mke2fs` (pacote e2fsprogs), para criar `ext4-2g.img`

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
