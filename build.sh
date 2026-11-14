#!/bin/bash
# Build the Iron Phoenix native runtime (32-bit Windows, llvm-mingw).
# Usage: ./build.sh [clean]
set -e
cd "$(dirname "$0")"
TC=${TC:-toolchain/llvm-mingw-20261006-ucrt-x86_64/bin}
CXX=$TC/i686-w64-mingw32-clang++
CC=$TC/i686-w64-mingw32-clang
OUT=build
OBJ=$OUT/obj
if [ "${VULKAN:-0}" = "1" ]; then OBJ=$OUT/obj-vulkan; fi
[ "$1" == "clean" ] && rm -rf $OBJ
mkdir -p $OBJ
COMMON="-DHAVE_STDBOOL_H=1 -DENABLE_SINC_FAST_CONVERTER -DENABLE_SINC_MEDIUM_CONVERTER -DENABLE_SINC_BEST_CONVERTER -DPACKAGE=\"libsamplerate\" -DVERSION=\"0.2\" -O2 -g -march=pentium4 -msse2 -mfpmath=sse -fno-strict-aliasing -fno-omit-frame-pointer -DXXH_INLINE_ALL"
INC="-Isrc -Isrc/net -Isrc/apu -Ithird_party/libsamplerate/include -Ithird_party/glad/include -Isrc/nv2a -Isrc/nv2a/compat -Isrc/nv2a/pgraph/thirdparty/gloffscreen -Ithird_party/nv2a_vsh_cpu/src -Ithird_party/xxHash"
VK_SRCS=""
VK_LIBS=""
if [ "${VULKAN:-0}" = "1" ]; then
  VD=toolchain/vulkan
  [ -f "$VD/glslang-build/glslang/libglslang.a" ] || { echo "Run python tools/prepare_vulkan.py first"; exit 1; }
  COMMON="$COMMON -DCONFIG_VULKAN -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_WIN32_KHR -DWIN32 -DVMA_STATIC_VULKAN_FUNCTIONS=0 -DVMA_DYNAMIC_VULKAN_FUNCTIONS=0"
  INC="$INC -I$VD/headers/include -I$VD/volk -I$VD/vma/include -I$VD/reflect -I$VD/glslang"
  VK_SRCS="$(find src/nv2a/pgraph/vk -name '*.c' -o -name '*.cpp') $VD/volk/volk.c $VD/reflect/spirv_reflect.c"
  VK_LIBS="$(find $VD/glslang-build -name '*.a' | sort)"
fi
CXXFLAGS="-std=c++20 $COMMON -Wall -Wno-unused-function -Wno-unused-variable -Wno-missing-braces -Wno-unused-but-set-variable -Wno-format $INC"
CFLAGS="-std=gnu11 $COMMON -w $INC"
SRCS="$(find src -name '*.cpp' -o -name '*.S' -o -name '*.c' | grep -v debug_renderdoc.c | grep -v /pgraph/vk/ | sort)
$VK_SRCS
third_party/glad/src/gl.c third_party/glad/src/wgl.c
third_party/libsamplerate/src/samplerate.c third_party/libsamplerate/src/src_linear.c third_party/libsamplerate/src/src_sinc.c third_party/libsamplerate/src/src_zoh.c
$(ls third_party/nv2a_vsh_cpu/src/*.c)"
# Rebuild everything if any header changed after the newest object
NEWEST_H=$(find src -name '*.h' -o -name '*.inc' | xargs ls -t | head -1)
OBJS=""
pids=""
running=0
for s in $SRCS; do
  o=$OBJ/$(echo ${s#src/} | tr '/' '_').o
  OBJS="$OBJS $o"
  if [ ! -f $o ] || [ $s -nt $o ] || [ $NEWEST_H -nt $o ]; then
    echo "  CC $s"
    case $s in
      *.c) $CC $CFLAGS -c $s -o $o & ;;
      *) $CXX $CXXFLAGS -c $s -o $o & ;;
    esac
    pids="$pids $!"
    running=$((running+1))
    if [ $running -ge 12 ]; then for p in $pids; do wait $p || exit 1; done; pids=""; running=0; fi
  fi
done
for p in $pids; do wait $p || exit 1; done
# reserve.S must be first so the Xbox image block lands at 0x11000
RES=$OBJ/reserve.S.o
OBJS="$RES $(echo $OBJS | tr ' ' '\n' | grep -v reserve.S.o | tr '\n' ' ')"
echo "  LD $OUT/ironphoenix.exe"
if ! $CXX -o $OUT/ironphoenix.exe $OBJS -static -Wl,--image-base=0x10000 -Wl,--disable-dynamicbase -Wl,--large-address-aware \
  -Wl,-Xlink=-merge:.buildid=.axbe,-Xlink=-merge:.text=.axbe,-Xlink=-merge:.rdata=.axbe,-Xlink=-merge:.data=.axbe \
  -Wl,--start-group $VK_LIBS -Wl,--end-group -lwinmm -luser32 -lgdi32 -ldwmapi -lole32 -lopengl32 -luuid -lksuser -lwindowscodecs > "$OUT/link.log" 2>&1; then
  cat "$OUT/link.log"
  exit 1
fi
grep -v "align specified without /driver" "$OUT/link.log" || true
[ -f $OUT/ironphoenix.exe ] || exit 1
$TC/llvm-objdump -h $OUT/ironphoenix.exe | grep -E "axbe"
# Sanity: a kernel export implemented with the wrong stdcall arg count silently falls back to
# the weak stub (different @N decoration). Detect names that exist with two decorations.
DUPS=$($TC/llvm-nm $OUT/ironphoenix.exe | grep -oE '_xk_[A-Za-z0-9]+@[0-9]+' | sort -u | sed 's/@.*//' | uniq -d)
if [ -n "$DUPS" ]; then echo "ERROR: kernel export arg-count mismatch: $DUPS"; exit 1; fi
