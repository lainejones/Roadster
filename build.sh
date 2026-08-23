#!/bin/sh
# Build Roadster with amiga-gcc. Run inside WSL:
#   cd /mnt/c/projects/Roadster && ./build.sh
set -e
export PATH=/opt/amiga/bin:$PATH

CC=m68k-amigaos-gcc
CFLAGS="-Os -msmall-code -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer"

mkdir -p out

echo "== Roadster (release) =="
# Strip at LINK time with -s. NEVER m68k-amigaos-strip: it corrupts the hunk
# reloc table on this toolchain -> guru 8000000B. See toolchain memory.
$CC $CFLAGS -s -o out/Roadster src/roadster.c

ls -l out
echo "Done."
