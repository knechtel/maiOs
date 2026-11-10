# Kernel mínimo em C no QEMU

Este exemplo inicializa um kernel Multiboot 1 de 32 bits com um terminal
interativo de texto VGA. O kernel lê o teclado PS/2 e oferece comandos internos
simples; o pequeno arquivo assembly prepara a pilha e chama `kernel_main`.
O mapa do kernel segue ABNT2, com Shift, Caps Lock e AltGr (Alt direito).
`make run` passa `-k pt-br` ao QEMU. O terminal VGA ainda não oferece suporte
Unicode geral para acentos.

Comandos disponíveis: `help`, `clear`, `echo`, `about`, `ls`, `cd`, `pwd`,
`mkdir`, `rmdir`, `rm`, `mv`, `write`, `cat`, `python`, `sqlite3`, `curl`, `halt` e `reboot`. O kernel lê e grava uma imagem ext4
por setores ATA PIO. `make run` cria e conecta `ext4-2g.img`, uma imagem ext4
de 2 GiB, preservando `disk.img` e `ext4.img` preexistentes.
O driver mapeia blocos diretos e indiretos simples, duplos e triplos.
O comando `help` pagina a lista; pressione qualquer tecla para avançar ou `q`
para sair.

Para gravar várias linhas, use `write NOME`, digite o conteúdo e finalize com um
ponto sozinho em uma linha. `cat NOME` lê os blocos do arquivo em sequência.
Use `rm NOME` para apagar um arquivo e `rmdir NOME` para apagar um diretório
vazio no diretório atual. Os dois comandos aceitam nomes simples, sem caminhos.
Use `mv NOME_ATUAL NOVO_NOME` para renomear um arquivo no diretório atual; o
destino não pode existir.
`python` abre o REPL CPython; `python ARQUIVO.py` executa um script salvo no
diretório atual. Exemplo:

```text
python
>>> print(6 * 7)
42
```

Pressione `Ctrl+D` para voltar ao shell. Para executar um arquivo:

```text
write exemplo.py print("oi do maiOs")
python exemplo.py
```

O comando `sqlite3 [ARQUIVO]` abre um terminal SQL interativo; sem argumento,
usa `maios.db`. Encerre com `.quit` ou Ctrl+D. O mesmo banco fica disponível no
CPython por `import sqlite3`, incluindo a API DB-API padrão. Os arquivos de
banco são limitados a 4 MiB e precisam ter nomes simples no diretório atual.
Use o journal padrão `DELETE`; WAL não está disponível, e o VFS não oferece
bloqueios para acesso concorrente ao mesmo banco.

O kernel padrão usa CPython 3.11.9, compilado para i386 com um runtime freestanding.
O interpretador suporta scripts do ext4, REPL e os módulos built-in descritos
abaixo. O comando `python` mantém a instância CPython ativa entre chamadas.

O módulo embutido `maiosfs` acessa arquivos do diretório atual no ext4:

```python
import maiosfs

maiosfs.write("nota.txt", "conteúdo")  # cria ou substitui; aceita str ou bytes
conteudo = maiosfs.read("nota.txt")    # retorna bytes
maiosfs.remove("nota.txt")
```

O módulo `sqlite3` usa o mecanismo SQLite 3.54.1 e o ext4 do maiOs. Exemplo:

```python
import sqlite3

db = sqlite3.connect("dados.db")
db.execute("CREATE TABLE IF NOT EXISTS notas (texto TEXT)")
db.execute("INSERT INTO notas VALUES (?)", ("ola",))
print(db.execute("SELECT texto FROM notas").fetchall())
db.commit()
db.close()
```

Por enquanto os nomes precisam ser simples, sem subdiretórios, e os arquivos
transferidos pelo MicroPython são limitados a 64 KiB.

O módulo CPython built-in `webserver` oferece um servidor HTTP sequencial.
Grave o script como `server.py` e execute `python server.py`:

```python
import webserver

def page(method, path):
    return "maiOs diz ola! Voce pediu " + path

webserver.serve(page)  # porta TCP 8070; também aceita webserver.serve(page, 8080)
```

Com `make run`, o QEMU encaminha `127.0.0.1:8070` do host para o maiOs. Acesse
`http://127.0.0.1:8070/`. O callback recebe método e caminho e deve retornar
`str` ou `bytes`. O servidor atende uma conexão por vez e limita a resposta a
16 KiB. Respostas usam HTTP 200; requisições malformadas e erros do callback
recebem 400 e 500, respectivamente.

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
implementa conexões de saída e `accept` sequencial para uma conexão por vez. O
built-in CPython `webserver` usa essa API; retransmissão avançada e múltiplas
conexões simultâneas ainda não são suportadas. O comando `tcp IP PORT` testa o
handshake de saída e fecha a conexão. HTTPS usa BearSSL 0.6, valida certificados
contra raízes incluídas no kernel e lê a data do RTC. `make run` ativa RDRAND
no CPU virtual do QEMU para semear o TLS. O cliente HTTP/1.1 envia GET e POST,
envia corpos e lê respostas com `Content-Length`, `Transfer-Encoding: chunked`
ou delimitadas pelo fechamento da conexão. `https HOST [CAMINHO]` exibe o corpo;
`https-post HOST CAMINHO CORPO` envia formulário; `https-save HOST CAMINHO ARQUIVO`
transfere o corpo em fluxo para o ext4, sem manter a resposta toda na RAM. A
API também aceita um callback de destino para encaminhar os dados recebidos.
A pilha oferece `curl https://HOST/CAMINHO` para exibir respostas GET e
`curl -o ARQUIVO https://HOST/CAMINHO` para salvar o corpo no diretório atual do
ext4. A implementação aceita somente HTTPS, exibe até 2048 caracteres
imprimíveis no terminal e não implementa todas as opções do curl convencional.
A lista de autoridades certificadoras é uma fotografia do
repositório do sistema usado na compilação e precisa ser atualizada para manter
a validação compatível com futuras mudanças de certificados.
O cliente Git implementa descoberta HTTP, leitura de packs, descompressão,
verificação SHA-1, resolução de deltas e checkout. `git clone URL [DIRETORIO]`
descobre a branch padrão anunciada pelo servidor, cria o diretório, instala os
objetos e grava o checkout e metadados básicos em `.git`. O pack recebido fica
no arquivo oculto `.git-pack` dentro do clone. `git refs` lista referências e
`git fetch HOST REPO_PATH REF ARQUIVO_PACK` baixa um pack e atualiza o checkout
no diretório atual. Exemplo:

```text
git clone https://github.com/knechtel/read-text.git
git refs github.com /DONO/REPO.git
git fetch github.com /DONO/REPO.git refs/heads/main repo.pack
```

O suporte atual limita packs a 2048 objetos e objetos individuais a 4 MiB;
refs de tags anotadas e repositórios que usam SHA-256 ainda não são suportados.

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
- `python3`, para gerar as tabelas do runtime MicroPython

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
