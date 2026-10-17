# Runner headless de testes

Ferramenta usada para testar o mod sem tela: roda a ROM no núcleo do mGBA e executa um script de comandos.

## Compilar

Compile o mGBA como biblioteca estática (testado com o mGBA 0.11-dev; `-DBUILD_STATIC=ON -DBUILD_SHARED=OFF -DBUILD_QT=OFF -DBUILD_SDL=OFF`) e depois:

```bash
MGBA=/caminho/para/mgba
D="-DBUILD_STATIC -DENABLE_DEBUGGERS -DENABLE_DIRECTORIES -DENABLE_GDB_STUB -DENABLE_SCRIPTING -DENABLE_VFS -DENABLE_VFS_FD \
   -DHAVE_CRC32 -DHAVE_FREELOCALE -DHAVE_LOCALE -DHAVE_STRDUP -DHAVE_STRLCPY -DHAVE_STRNDUP -DHAVE_USELOCALE -DHAVE_VASPRINTF \
   -DM_CORE_GBA -DNDEBUG -DUSE_FREETYPE -DUSE_LZMA -DUSE_MINIZIP -DUSE_PNG -DUSE_PTHREADS -DUSE_ZLIB -D_GNU_SOURCE \
   -DHAVE_NEWLOCALE -DHAVE_LOCALTIME_R -DHAVE_REALPATH"
gcc -O2 $D -I$MGBA/include -I$MGBA/build/include -I$MGBA/src run.c -o run $MGBA/build/libmgba.a \
    -lm -lpng -lz -lpthread
```

As definições (`-D...`) precisam ser **as mesmas** usadas na compilação do `libmgba.a` (veja o `build.ninja` do mGBA), senão as structs não batem e o runner trava. Se o link reclamar de símbolos, acrescente as bibliotecas habilitadas no seu build do mGBA (lzma, freetype, json-c…).

## Uso

```bash
make modern -j$(nproc)          # na raiz do repositório
./mkdebug.sh                    # copia a ROM para debug.gba com gRtDebugEnabled=1 e gera syms.txt
OUT=/tmp/rt_out ./tt.sh te.txt  # inicia um jogo novo (boot.txt -> field.ss) e roda o script
```

Comandos de script: `wait N`, `hold TECLAS N`, `tap TECLAS`, `mash N`, `shot ARQ.png`, `savest/loadst ARQ`,
`peek END LEN`, `peekp PTR OFF LEN` (lê a partir de um ponteiro), `poke8/16/32 END VAL`, `pokep16 PTR OFF VAL`,
`waitp PTR OFF VAL MAX` (espera um byte), `dump END LEN ARQ` (despeja memória).
Teclas: `A B s`(select) `S`(start) `r l u d`(direcional) `R L`.

Nos scripts, `{simbolo}` / `{simbolo+offset}` vira o endereço do `syms.txt`, `{HARNESS}` vira esta pasta e `{OUT}` vira `$OUT`.

Variáveis de depuração (só têm efeito no `debug.gba`):

| Variável | Uso |
|---|---|
| `gRtDebugCommand` | 1 = batalha selvagem, 2 = batalha contra treinador, 3 = próximo warp de teste, 7 = curar equipe, 9 = encerrar batalha, 10 = batalha no Safari |
| `gRtDebugSpecies` / `gRtDebugLevel` | Espécie (nº nacional) e nível do Pokémon selvagem |
| `gRtDebugTrainer` | ID do treinador |
| `gRtDebugAutoplay` | 1 = o jogador também é controlado pela IA |
| `gRtDebugTestMove` | Força um golpe no slot 1 (o inimigo vira um boneco de treino) |
| `gRtProf` | Perfil por etapa do quadro (linhas de varredura acumuladas) |

Scripts incluídos:
- `allmovest.txt`: testa os 354 golpes.
- `te.txt`: batalha contra treinador com prints.
- `perf3t.txt`: perfil de desempenho numa batalha dupla.
- `arenadump.txt` + `render_arena.py`: renderiza a arena inteira a partir de um dump da VRAM.
- `surf.txt`: sequência de prints do Surf.
