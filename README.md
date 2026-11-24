# Kernel mínimo em C no QEMU

Este exemplo inicializa um kernel Multiboot 1 de 32 bits com um terminal
interativo de texto VGA. O kernel lê o teclado PS/2 e oferece comandos internos
simples; o pequeno arquivo assembly prepara a pilha e chama `kernel_main`.
O mapa do kernel segue ABNT2, com Shift, Caps Lock e AltGr (Alt direito).
`make run` passa `-k pt-br` ao QEMU. O terminal VGA ainda não oferece suporte
Unicode geral para acentos.

O escalonador usa o PIT a 100 Hz para alternar tarefas de kernel em uma CPU.
`parallel` inicia duas tarefas de cálculo concorrentes e `tasks` mostra quantas
continuam ativas. Código do kernel pode criar tarefas com `task_create()` e
ceder a CPU com `task_yield()`. As tarefas compartilham memória e executam no
mesmo núcleo; APIs de dispositivos e subsistemas com estado global não devem
ser chamadas por tarefas concorrentes sem sincronização.

Comandos disponíveis: `help`, `clear`, `echo`, `about`, `tasks`, `parallel`, `ls`, `cd`, `pwd`,
`mkdir`, `rmdir`, `rm`, `delete`, `mv`, `write`, `cat`, `python`, `python-stop`, `sqlite3`, `curl`, `halt` e `reboot`. O kernel lê e grava uma imagem ext4
por setores ATA PIO. `make run` cria e conecta `ext4-2g.img`, uma imagem ext4
de 2 GiB, preservando `disk.img` e `ext4.img` preexistentes.
O driver mapeia blocos diretos e indiretos simples, duplos e triplos.
O comando `help` pagina a lista; pressione qualquer tecla para avançar ou `q`
para sair.

Para gravar várias linhas, use `write NOME`, digite o conteúdo e finalize com um
ponto sozinho em uma linha. `cat NOME` lê os blocos do arquivo em sequência.
Use `rm NOME` para apagar um arquivo e `rmdir NOME` para apagar um diretório
vazio no diretório atual. `delete NOME` apaga recursivamente um diretório e seu
conteúdo, incluindo arquivos e subdiretórios. Esses comandos aceitam nomes
simples, sem caminhos.
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

Para manter um servidor Python ativo enquanto usa o prompt, inicie o script
com `python server.py &`. O comando `python-stop` solicita que o servidor em
segundo plano pare. Enquanto ele estiver ativo, o CPython e a rede ficam
reservados para essa tarefa; comandos locais do shell continuam disponíveis.
Saída Python fica oculta para não sobrescrever a linha de comando. Comandos de
rede e `sqlite3` ficam indisponíveis até o servidor parar. `write` coleta o
texto antes de gravar no ext4 e aceita até 64 KiB.

O comando `sqlite3 [ARQUIVO]` abre um terminal SQL interativo; sem argumento,
usa `maios.db`. Encerre com `.quit` ou Ctrl+D. O mesmo banco fica disponível no
CPython por `import sqlite3`, incluindo a API DB-API padrão. Os arquivos de
banco são limitados a 4 MiB e precisam ter nomes simples no diretório atual.
Use o journal padrão `DELETE`; WAL não está disponível, e o VFS não oferece
bloqueios para acesso concorrente ao mesmo banco.

O kernel padrão usa CPython 3.11.18, compilado para i386 com um runtime freestanding.
O interpretador suporta scripts do ext4, REPL e os módulos built-in descritos
abaixo. `json` também está incluído: o pacote e seu acelerador `_json` ficam no
binário, sem exigir arquivos de biblioteca no ext4. O comando `python` mantém a
instância CPython ativa entre chamadas.

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

def page(method, path, body):
    return "maiOs diz ola! Voce pediu " + path

webserver.serve(page)  # porta TCP 8070; também aceita webserver.serve(page, 8080)
```

O servidor mantém até 16 sessões em memória. `webserver.session()` retorna o
dicionário da requisição atual; os valores ficam disponíveis nas próximas
requisições que enviarem o cookie de sessão. O servidor cria o cookie
`maios_session` quando necessário, com `HttpOnly` e `SameSite=Lax`:

```python
import webserver

def page(method, path, body):
    session = webserver.session()
    session["visitas"] = session.get("visitas", 0) + 1
    return "Visitas nesta sessão: " + str(session["visitas"])

webserver.serve(page)
```

As sessões existem somente enquanto o processo do servidor estiver ativo; ao
atingir 16 sessões, uma sessão antiga é substituída. `session()` só pode ser
chamada durante o callback de uma requisição. Durante a execução do servidor,
`Ctrl+C` interrompe `webserver.serve()` e retorna ao terminal do maiOs.
Para execução em segundo plano, use `python server.py &` e encerre com
`python-stop`.

Com `make run`, o QEMU encaminha `127.0.0.1:8070` do host para o maiOs. Acesse
`http://127.0.0.1:8070/`. O callback recebe método, caminho e corpo como `bytes`
e deve retornar `str` ou `bytes`. O corpo é lido conforme `Content-Length`, até
16 KiB. O servidor atende uma conexão por vez e limita a resposta a 16 KiB.
Respostas usam HTTP 200; requisições malformadas e erros do callback recebem
400 e 500, respectivamente.

O alvo `make install-python-stdlib` instala `/python311.zip` na imagem ext4 com
CPython e os pacotes puros Django 5.2.19, asgiref 3.13.0 e sqlparse 0.6.1.
`make run` também atualiza esse arquivo e instala o exemplo como `/django`.
Como o servidor HTTP permanece ativo, inicie-o em segundo plano no terminal do
maiOs com `python django &`; assim o prompt continua disponível. Acesse
`http://127.0.0.1:8070/` no host e use `python-stop` para encerrar o servidor.
A aplicação demonstra Django via WSGI e responde na rota `/`. O exemplo usa
`USE_TZ=False`, pois a imagem ainda não inclui uma base de dados de fusos
horários. Os wheels usados na compilação estão em `third_party/django_wheels/`.

O shell inclui `django-admin startproject NOME` e `django-admin startapp NOME`.
Por exemplo, `django-admin startproject site` cria `site/` no diretório atual;
depois, `cd site` e `django-admin startapp core` cria a aplicação `core/`.
Para iniciar o projeto, ainda dentro de `site/`, use `django-admin runserver`;
ele serve a aplicação WSGI na porta 8070, encaminhada para
`http://127.0.0.1:8070/`. Pressione Ctrl+C no maiOs para parar o servidor.
O comando usa os templates oficiais empacotados com o wheel Django e grava os
arquivos no ext4. O `runserver` do maiOs usa o servidor WSGI do kernel porque o
port não oferece sockets POSIX gerais.
O CPython do maiOs compila o módulo `array`, usado por `sqlite3` durante a
inicialização de `django.contrib.auth` e `django.contrib.admin`. Para usar o
admin, mantenha os apps e middlewares padrão do projeto, rode
`django-admin migrate` e crie um usuário administrador com
`django-admin createsuperuser` antes de acessar `/admin/`.

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
