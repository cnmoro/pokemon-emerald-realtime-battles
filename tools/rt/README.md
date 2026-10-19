# Geração dos assets do mod

Veja a seção "Ferramentas de desenvolvimento" do README principal. As variáveis de ambiente são:

- `SPRITECOLLAB`: clone do https://github.com/PMDCollab/SpriteCollab (o padrão é `./SpriteCollab`).
- `POKEEMERALD`: raiz do repositório (o padrão é `../..` a partir deste script).
- `RT_WORK`: pasta de arquivos intermediários (o padrão é `build/rt_work`).

Ordem: `sprites.py` (2×) → `build_sprites.py` → `fxgen.py` → `movelist.py` → `movegen.py`.
O `build_sprites.py` precisa do `lzc` compilado nesta pasta (`gcc -O2 lzc.c -o lzc`).
