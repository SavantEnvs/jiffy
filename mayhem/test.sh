#!/usr/bin/env bash
#
# mayhem/test.sh — run jiffy's own eunit suite against the real NIF, plus known-answer checks on the
# decoder/encoder (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}


passed=0; failed=0
ok()   { passed=$((passed + 1)); }
bad()  { failed=$((failed + 1)); echo "FAIL: $*" >&2; }

# 1) jiffy's own eunit suite (867 tests) against the real jiffy.so built by build.sh. The pass count is
#    parsed from eunit's report, so a runner that merely exits 0 does not count.
ORA="$SRC/mayhem/build-oracle"
mods=$(cd "$ORA/test" && ls ./*_tests.beam | sed 's|^\./||; s|\.beam$||' | paste -sd, -)
log=$(mktemp)
erl -noshell -pa "$ORA/jiffy/ebin" -pa "$ORA/test" \
    -eval "R = eunit:test([$mods], []), halt(case R of ok -> 0; _ -> 1 end)." >"$log" 2>&1
rc=$?
cat "$log"
if [ "$rc" -eq 0 ] && n=$(sed -n 's/^ *All \([0-9][0-9]*\) tests passed\./\1/p' "$log") && [ -n "$n" ] && [ "$n" -gt 800 ]; then
  passed=$((passed + n))
else
  nf=$(sed -n 's/^ *Failed: \([0-9][0-9]*\)\..*/\1/p' "$log" | tail -1)
  failed=$((failed + ${nf:-1}))
  echo "FAIL: eunit (rc=$rc)" >&2
fi
rm -f "$log"

# 2) Known-answer checks on jiffy's real C decoder/encoder (jiffy-kat links c_src/jiffy.c). Each probe is
#    unconditional: a missing or sabotaged binary yields the wrong output and fails.
KAT="$SRC/jiffy-kat"
[ -x "$KAT" ] || { bad "missing $KAT"; emit_ctrf jiffy "$passed" "$failed"; exit 1; }

dec() {  # dec <expected> <json> [option...]
  local want="$1" json="$2"; shift 2
  local got; got=$(printf '%s' "$json" | "$KAT" decode "$@" 2>&1)
  if [ "$got" == "$want" ]; then ok; else bad "decode $json $*: want [$want] got [$got]"; fi
}
enc() {  # enc <expected> <printf-escaped ETF> [option...]
  local want="$1" etf="$2"; shift 2
  local got; got=$(printf "$etf" | "$KAT" encode "$@" 2>&1)
  if [ "$got" == "$want" ]; then ok; else bad "encode $etf $*: want [$want] got [$got]"; fi
}

dec '#{<<"a">> => [1,2.5,null]}'      '{"a":[1,2.5,null]}' return_maps
dec '{[{<<"a">>,[1,2.5,null]}]}'      '{"a":[1,2.5,null]}'
dec '[true,false,null]'               '[true,false,null]'
dec '<<"\xC3\xA9\xF0\x9F\x98\x80\x0A">>' '"é😀\n"'
dec '{partial,{bignum,<<"123456789012345678901234567890">>}}' '123456789012345678901234567890'
dec '{has_trailer,[1],<<"x">>}'       '[1] x' return_trailer
dec '{[{<<"a">>,2}]}'                 '{"a":1,"a":2}' dedupe_keys
dec '#{<<"a">> => 2}'                 '{"a":1,"a":2}' return_maps
dec '{[{<<"x">>,nil}]}'               '{"x":null}' use_nil
dec '#{<<"a">> => #{<<"b">> => []}}'  '{"a":{"b":[]}}' return_maps copy_strings
dec '{error,{5,truncated_json}}'      '[1,2'
dec '{error,{1,invalid_literal}}'     'nul'
dec '{error,{4,invalid_json}}'        '[1,]'
dec '{error,{5,invalid_string}}'      '"abc'
big="[$(yes 1 | head -n 6000 | paste -sd, -)]"
dec "$big"                            "$big" bytes_per_red=1      # crosses many scheduler yields

enc '{"a":1}'                         '\x83t\x00\x00\x00\x01m\x00\x00\x00\x01aa\x01'
enc $'{\n  "a" : 1\n}'                '\x83t\x00\x00\x00\x01m\x00\x00\x00\x01aa\x01' pretty
enc '["\u00E9",1]'              '\x83l\x00\x00\x00\x02m\x00\x00\x00\x02\xc3\xa9a\x01j' uescape
enc '"a\/b"'                          '\x83m\x00\x00\x00\x03a/b' escape_forward_slashes
enc '"a/b"'                           '\x83m\x00\x00\x00\x03a/b'
enc '3.14'                            '\x83F\x40\x09\x1e\xb8\x51\xeb\x85\x1f'
enc '[true,false,null,255]'           '\x83l\x00\x00\x00\x04d\x00\x04trued\x00\x05falsed\x00\x04nulla\xffj'
enc '[null]'                          '\x83l\x00\x00\x00\x01d\x00\x03nilj' use_nil
enc '["nil"]'                         '\x83l\x00\x00\x00\x01d\x00\x03nilj'
enc '{"k":7}'                         '\x83h\x01l\x00\x00\x00\x01h\x02m\x00\x00\x00\x01ka\x07jj'
enc '[1,2]'                           '\x83h\x02d\x00\x04jsonm\x00\x00\x00\x05[1,2]'
enc '{error,{invalid_string,<<"\xFF\xFE">>}}' '\x83m\x00\x00\x00\x02\xff\xfe'
enc '{partial,[{1,2,3}]}'             '\x83h\x03a\x01a\x02a\x03'
enc "$big"                            "\\x83l\\x00\\x00\\x17\\x70$(yes '\x61\x01' | head -n 6000 | tr -d '\n')j" bytes_per_red=1

# a sabotaged (no-op) probe prints nothing, so the very first comparison above has already failed
emit_ctrf jiffy "$passed" "$failed"
