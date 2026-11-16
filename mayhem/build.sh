#!/usr/bin/env bash
#
# mayhem/build.sh — build jiffy's fuzz harnesses and its test oracle.
#
# jiffy is an Erlang NIF, so there is no BEAM to fuzz it with. mayhem/harness/enif_mock.c models the
# part of erl_nif.h that c_src uses (terms, binaries, maps, resources, yielding), and nif_host.c
# compiles the unmodified c_src/jiffy.c into the same translation unit and drives its real
# nif_decode_init / nif_encode_init entry points. The fuzzed code is jiffy's own decoder/encoder.
#
# Produces, all under $SRC:
#   fuzz_decode, fuzz_encode                 libFuzzer binaries (sanitized, DWARF 3)
#   fuzz_decode-standalone, fuzz_encode-standalone   run-once reproducers (no libFuzzer runtime)
#   jiffy-kat                                known-answer probe (normal flags, same mock host)
#   mayhem/build-oracle/                     the real NIF (.so) + compiled Erlang sources and eunit tests
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# The only external input is erl_nif.h (apt erlang-dev, installed in the image).
ERL_ROOT="$(erl -noshell -eval 'io:format("~s", [code:root_dir()]), halt().')"
ERL_INC="$ERL_ROOT/usr/include"
test -f "$ERL_INC/erl_nif.h"

H="$SRC/mayhem/harness"
OBJ="$(mktemp -d)"
trap 'rm -rf "$OBJ"' EXIT

# 1) The code under test, instrumented: the mock VM, the unity-built jiffy NIF, and each harness.
#    -fsanitize=fuzzer-no-link puts SanCov edges on every unit, so coverage reaches c_src.
CFLAGS_FUZZ="$SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link -O1 -I$ERL_INC"
for u in enif_mock nif_host fuzz_decode fuzz_encode; do
  $CC $CFLAGS_FUZZ -c "$H/$u.c" -o "$OBJ/$u.o"
done
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$H/lsan_off.cc" -o "$OBJ/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ/standalone_main.o"

# 2) Link each harness twice: with libFuzzer, and with the run-once driver.
for t in fuzz_decode fuzz_encode; do
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE \
      "$OBJ/$t.o" "$OBJ/nif_host.o" "$OBJ/enif_mock.o" "$OBJ/lsan_off.o" -lm -o "$SRC/$t"
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS \
      "$OBJ/standalone_main.o" "$OBJ/$t.o" "$OBJ/nif_host.o" "$OBJ/enif_mock.o" "$OBJ/lsan_off.o" \
      -lm -o "$SRC/$t-standalone"
done

# 3) Test oracle, with NORMAL flags (independent of the sanitized build).
#    a) the known-answer probe: real jiffy c_src on the mock host
$CC -O2 $COVERAGE_FLAGS -I"$ERL_INC" "$H/kat.c" "$H/nif_host.c" "$H/enif_mock.c" -lm \
    -o "$SRC/jiffy-kat"
file "$SRC/jiffy-kat" | grep -q 'dynamically linked'
#    b) the real NIF as a shared object, plus jiffy's Erlang modules and eunit suite
ORA="$SRC/mayhem/build-oracle"
rm -rf "$ORA"
mkdir -p "$ORA/jiffy/ebin" "$ORA/jiffy/priv" "$ORA/test"
$CC -O3 -fPIC -fvisibility=hidden -Wall $COVERAGE_FLAGS -I"$SRC/c_src" -I"$ERL_INC" \
    -shared "$SRC/c_src/jiffy.c" -o "$ORA/jiffy/priv/jiffy.so"
erlc -o "$ORA/jiffy/ebin" "$SRC"/src/*.erl
erlc -I "$SRC/test" -o "$ORA/test" "$SRC"/test/*.erl
