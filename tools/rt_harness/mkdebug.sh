#!/bin/bash
# Builds a debug copy of the ROM (gRtDebugEnabled=1) next to this script and writes syms.txt.
H="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$H/../.." && pwd)"
cd "$REPO"
ADDR=$(grep -E "^\s+0x[0-9a-f]+\s+gRtDebugEnabled$" pokeemerald_modern.map | awk '{print $1}')
OFF=$((ADDR - 0x08000000))
cp pokeemerald_modern.gba "$H/debug.gba"
printf '\x01' | dd of="$H/debug.gba" bs=1 seek=$OFF conv=notrunc status=none
for s in gRtDebugSpecies gRtDebugLevel gRtDebugTrainer gRtDebugAutoplay gRtDebugCommand gRtDebugWarpIdx gRtDebugTestMove gRt gRtProf gMain gPlayerPartyCount; do
  echo "$s $(grep -E "^\s+0x[0-9a-f]+\s+$s$" pokeemerald_modern.map | awk '{print $1}')"
done > "$H/syms.txt"
cat "$H/syms.txt"
