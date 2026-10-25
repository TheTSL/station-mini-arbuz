#!/bin/sh
# Сборка pcm_play под aarch64 со статической stock alsa-lib.
# Требуется кросс-компилятор aarch64-linux-gnu-gcc.
set -e

ALSA_VER=1.2.11
ALSA_DIR=${ALSA_DIR:-$PWD/alsa-lib-$ALSA_VER}

if [ ! -d "$ALSA_DIR" ]; then
  curl -L "https://www.alsa-project.org/files/pub/lib/alsa-lib-$ALSA_VER.tar.bz2" -o alsa.tar.bz2
  tar xf alsa.tar.bz2
  cd "$ALSA_DIR"
  ./configure --host=aarch64-linux-gnu --enable-static --disable-shared \
      --disable-python --without-debug
  make -j"$(nproc)"
  cd -
fi

aarch64-linux-gnu-gcc -O2 -static \
  -I"$ALSA_DIR/include" \
  -o pcm_play pcm_play.c \
  -L"$ALSA_DIR/src/.libs" -lasound -lpthread -lm -ldl -lrt

echo "built: ./pcm_play"
