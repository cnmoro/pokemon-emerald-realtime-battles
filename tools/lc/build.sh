#!/bin/bash
# Builds build/lc/rt_blob.bin: the real-time battle engine linked for the Liquid Crystal ROM.
set -e -o pipefail
cd "$(dirname "$0")/../.."
B=build/lc; mkdir -p $B
CC1=$(arm-none-eabi-gcc --print-prog-name=cc1)
CPPFLAGS="-iquote include -Wno-trigraphs -DFIRERED -DREVISION=0 -DENGLISH -DMODERN=1 -DRT_LC=1"
CFLAGS="-mthumb -mthumb-interwork -O2 -mabi=apcs-gnu -mtune=arm7tdmi -march=armv4t -fno-toplevel-reorder -Wno-pointer-to-int-cast -mlong-calls"
SRCS="${SRCS:-src/rt_lc_glue.c src/rt_main.c src/rt_arena.c src/rt_actor.c src/rt_moves.c src/rt_fx.c src/rt_ai.c src/rt_ui.c src/rt_props.c src/rt_vfx.c src/rt_data.c src/rt_debug.c src/option_menu.c src/rt_lc_summary.c src/rt_lc_text.c}"
OBJS=""
# headers (and the generated data headers) changed since the last build -> recompile everything
NEWEST_H=$(ls -t include/*.h src/data/rt/*.h | head -1)
for s in $SRCS; do
  o=$B/$(basename ${s%.c}).o
  if [ ! -f $o ] || [ $s -nt $o ] || [ -n "$FORCE" ] || [ $NEWEST_H -nt $o ]; then
    arm-none-eabi-cpp $CPPFLAGS $s | tools/preproc/preproc -i $s charmap.txt | $CC1 -quiet $CFLAGS -o - - | cat - <(echo -e ".text\n\t.align\t2, 0") | arm-none-eabi-as -mcpu=arm7tdmi -o $o -
  fi
  OBJS="$OBJS $o"
done
arm-none-eabi-as -mcpu=arm7tdmi -I . -o $B/rt_gfx.o data/rt_gfx_lc.s
arm-none-eabi-nm --defined-only $OBJS | awk 'NF==3 {print $3}' | sort -u > $B/own_syms.txt
python3 tools/lc/gen_syms.py tools/lc/pokefirered_bpre10.sym tools/lc/overrides.txt $B/syms.ld tools/lc/fr_functions.txt $B/own_syms.txt
LIBGCC=$(dirname $(arm-none-eabi-gcc -mthumb -march=armv4t -print-libgcc-file-name))
LIBC=$(dirname $(arm-none-eabi-gcc -mthumb -march=armv4t -print-file-name=libc.a))
arm-none-eabi-ld -L $B -T tools/lc/lc.ld -Map $B/rt_blob.map -o $B/rt_blob.elf $OBJS $B/rt_gfx.o -L $LIBC -L $LIBGCC -lc -lgcc
arm-none-eabi-objcopy -O binary -j .text -j .rodata -j .data $B/rt_blob.elf $B/rt_blob.bin
ls -la $B/rt_blob.bin
