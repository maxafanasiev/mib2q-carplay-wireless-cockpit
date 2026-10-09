#!/bin/bash
# Build mainmirror_display (cluster copy of the main CarPlay screen) for QNX/ARMv7 in Docker.
# Same import-stub approach as build_renderers.sh: the unit provides the real
# libscreen / libEGL / libGLESv2 at runtime.
set -e
IMG=qnx65-armv7-toolchain:latest
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
docker run --rm --platform=linux/amd64 -v "$PROJECT_DIR":/src "$IMG" bash -c '
  set -e
  export PATH=/opt/qnx650/host/linux/x86/usr/bin:$PATH
  export QNX_HOST=/opt/qnx650/host/linux/x86 QNX_TARGET=/opt/qnx650/target/qnx6
  CC=arm-unknown-nto-qnx6.5.0eabi-gcc
  ABI_INCLUDE=/src/toolchain/qnx65-abi/include
  gen_stub(){ local so="$1" rx="$2"; shift 2
    grep -rhoE "$rx" "$@" 2>/dev/null | sort -u | sed "s/.*/int &(){return 0;}/" > /tmp/st_$so.c
    $CC -shared -fPIC -Wl,-soname,"$so" /tmp/st_$so.c -o /tmp/"$so"
  }
  cd /src/mainmirror_display
  SRCS="main.c ../common/cluster_surface.c"
  gen_stub libscreen.so.1  "\bscreen_[a-z_]+"      $SRCS
  gen_stub libEGL.so.1     "\begl[A-Z][A-Za-z0-9]+" $SRCS
  gen_stub libGLESv2.so.1  "\bgl[A-Z][A-Za-z0-9]+"  $SRCS
  $CC -O2 -std=gnu99 -Wall -Wextra -D__QNX__ -DPLATFORM_QNX -I. -I../common -I"$ABI_INCLUDE" $SRCS \
      -o /src/build/mainmirror_display -Wl,--allow-shlib-undefined \
      -L/tmp -l:libscreen.so.1 -l:libEGL.so.1 -l:libGLESv2.so.1 -lm
  n=$(arm-unknown-nto-qnx6.5.0eabi-nm /src/build/mainmirror_display 2>/dev/null | grep -ci emutls || true)
  [ "$n" = "0" ] || { echo "REJECTED: emutls"; exit 1; }
  arm-unknown-nto-qnx6.5.0eabi-readelf -d /src/build/mainmirror_display | grep NEEDED
  echo "  built build/mainmirror_display"
'
