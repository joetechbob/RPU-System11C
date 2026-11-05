#!/bin/bash
# System 11C tests for the RPU library.
#
#   run.sh               build and run the tests against this branch's RPU.cpp (working tree)
#   run.sh branch        same
#   run.sh upstream      run the same tests against upstream RPU.cpp from commit 21b13a3, configured for
#                        architecture 15, Rev 102 (each test is expected to FAIL there, and says why)
#   run.sh both          run both and print one table: fails on upstream / passes on branch
#   run.sh list          list the tests
#   run.sh mutants       break each fix in a scratch copy and check that the tests notice
#   run.sh codediff [REF] check that RPU.cpp/.h/_Config.h/_AlphaScan.h differ from REF (default HEAD) only in comments
#   run.sh pulse [FILE]  optional: count AVR cycles of the E pulse in a compiled RPU_DataWrite/Read
#                        (FILE = an ELF or .o from avr-gcc; builds one from RPU.cpp when omitted)
#   run.sh pulse upstream  the same check on upstream RPU.cpp (expected to fail)
#
# Needs only a C++17 compiler (c++ / clang++ / g++) and git.  The pulse check also needs avr-gcc
# (Arduino IDE / arduino-cli install; see AVR_TOOLS below).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
UPSTREAM_COMMIT=21b13a3
BUILD="${S11C_TEST_BUILD:-$HERE/build}"
CXX="${CXX:-c++}"
AVR_TOOLS="${AVR_TOOLS:-$HOME/Library/Arduino15/packages/arduino/tools/avr-gcc/7.3.0-atmel3.6.1-arduino7/bin}"
HARNESS="$HERE/harness"
mkdir -p "$BUILD"

stage() {   # stage <branch|upstream> -> $BUILD/<which>/lib
  local which="$1" lib="$BUILD/$1/lib"
  rm -rf "$BUILD/$which"; mkdir -p "$lib"
  if [ "$which" = branch ]; then
    for f in RPU.cpp RPU.h RPU_Config.h RPU_AlphaScan.h; do cp "$REPO/$f" "$lib/$f"; done
  else
    git -C "$REPO" cat-file -e "$UPSTREAM_COMMIT^{commit}" 2>/dev/null || { echo "upstream commit $UPSTREAM_COMMIT is not in this repository" >&2; exit 2; }
    for f in RPU.cpp RPU.h RPU_Config.h; do git -C "$REPO" show "$UPSTREAM_COMMIT:$f" > "$lib/$f"; done
    # Upstream ships a Rev 3 / architecture 1 configuration.  Configure it as the branch is: Rev 102,
    # System 11 (arch 15), 6802 mode, W-type 11 sound.  Nothing else is changed.
    sed -i.bak \
      -e 's/^#define RPU_OS_HARDWARE_REV .*/#define RPU_OS_HARDWARE_REV 102/' \
      -e 's/^#define RPU_MPU_ARCHITECTURE .*/#define RPU_MPU_ARCHITECTURE 15/' \
      -e 's/^#define RPU_MPU_BUILD_FOR_6800 .*/#define RPU_MPU_BUILD_FOR_6800 0/' \
      -e 's/^#define RPU_OS_USE_WAV_TRIGGER_1p3.*//' \
      -e 's|^//#define RPU_OS_USE_W11_SOUND|#define RPU_OS_USE_WTYPE_11_SOUND|' "$lib/RPU_Config.h"
    rm -f "$lib/RPU_Config.h.bak"
  fi
  # The one inline-asm statement of RPU.cpp (RPU_E_WAIT_2, "rjmp .+0" = 2 cycles) becomes a call that
  # advances the fake clock by 2 cycles.  Upstream has none.
  sed -i.bak 's/__asm__ __volatile__("rjmp \.+0")/rpu_test_wait_cycles(2)/' "$lib/RPU.cpp"
  rm -f "$lib/RPU.cpp.bak"
}

build() {   # build <branch|upstream>
  local which="$1" out="$BUILD/$1" lib="$BUILD/$1/lib"
  stage "$which"
  local flags="-std=c++17 -O1 -g0 -D__AVR_ATmega2560__ -I$HARNESS/shim -I$lib"
  local srcs="$HARNESS/fakeavr.cpp $HARNESS/board.cpp $HARNESS/arduino_impl.cpp $HARNESS/absent.cpp"
  [ "$which" = upstream ] && srcs="$srcs $HARNESS/upstream_absent.cpp"
  "$CXX" $flags -w -c "$lib/RPU.cpp" -o "$out/RPU.o" || { echo "RPU.cpp does not compile for $which" >&2; exit 2; }
  "$CXX" $flags -Wall -Wno-unused-function -Wno-sign-compare "$HARNESS/tests.cpp" $srcs "$out/RPU.o" -o "$out/tests" || { echo "tests do not build for $which" >&2; exit 2; }
}

run_all() {   # run_all <which> -> writes $BUILD/<which>.tsv: id fix result reason
  local which="$1" tsv="$BUILD/$1.tsv"
  : > "$tsv"
  local ids; ids=$("$BUILD/$which/tests" --list | cut -f1)
  for id in $ids; do
    ( ulimit -t 120; "$BUILD/$which/tests" "$id" 2>&1 ) | grep '^RESULT' >> "$tsv" || printf 'RESULT\t%s\tCRASH\t?\tno result (crashed or timed out)\n' "$id" >> "$tsv"
  done
}

print_one() {   # print_one <which> <expect: pass|fail>
  local which="$1" expect="$2" tsv="$BUILD/$1.tsv"
  awk -F'\t' -v exp="$expect" '
    { id=$2; res=$3; fix=$4; why=$5
      tag=res
      if (exp=="fail" && res=="PASS") tag="PASS (UNEXPECTED: does not demonstrate the fix)"
      if (exp=="fail" && res=="FAIL") tag="FAIL (expected)"
      if (exp=="pass" && res!="PASS") tag=res " (UNEXPECTED)"
      printf "%-6s %-48s %s\n", fix, id, tag
      if (res!="PASS" && why!="") printf "         why: %s\n", substr(why,1,600)
      n++; if (tag ~ /UNEXPECTED|CRASH/) bad++ }
    END { printf "\n%d tests, %d unexpected\n", n, bad+0; exit (bad>0) }' "$tsv"
}

table() {   # one row per fix: fails on upstream / passes on branch
  awk -F'\t' '
    FNR==NR { up[$2]=$3; fix[$2]=$4; order[++n]=$2; next }
    { br[$2]=$3 }
    END {
      printf "%-6s %-48s %-18s %-14s\n", "fix", "test", "fails on upstream", "passes on branch"
      for (i=1;i<=n;i++) { id=order[i]
        printf "%-6s %-48s %-18s %-14s\n", fix[id], id, (up[id]=="FAIL" ? "yes" : "NO"), (br[id]=="PASS" ? "yes" : "NO") } }' "$BUILD/upstream.tsv" "$BUILD/branch.tsv"
}

case "${1:-branch}" in
  list) build branch; "$BUILD/branch/tests" --list | awk -F'\t' '{printf "%-6s %-48s %s\n",$2,$1,$3}' ;;
  branch)   build branch;   run_all branch;   print_one branch pass;  exit $? ;;
  upstream) build upstream; run_all upstream; print_one upstream fail; exit $? ;;
  both)
    build branch; run_all branch; echo "=== branch system11c: every test must PASS"; print_one branch pass; rc1=$?
    echo; build upstream; run_all upstream; echo "=== upstream $UPSTREAM_COMMIT (arch 15, Rev 102): every test must FAIL"; print_one upstream fail; rc2=$?
    echo; echo "=== summary"; table
    exit $((rc1 | rc2)) ;;
  mutants) exec python3 "$HERE/mutants.py" ;;
  codediff)   # codediff [REF]: is the library code identical to REF once comments are stripped?
    ref="${2:-$(git -C "$REPO" rev-parse HEAD)}"; rc=0
    for f in RPU.cpp RPU.h RPU_Config.h RPU_AlphaScan.h; do
      if diff <(git -C "$REPO" show "$ref:$f" | python3 "$HERE/strip_comments.py" /dev/stdin) <(python3 "$HERE/strip_comments.py" "$REPO/$f") >/dev/null; then
        echo "$f: same as $ref apart from comments"
      else echo "$f: CODE DIFFERS from $ref"; rc=1; fi
    done; exit $rc ;;
  pulse)
    shift
    if [ "${1:-}" = upstream ]; then   # the same check on upstream RPU.cpp (expected to fail)
      stage upstream; exec python3 "$HERE/check_pulse_cycles.py" --repo "$REPO" --src "$BUILD/upstream/lib" --avr-tools "$AVR_TOOLS"
    fi
    exec python3 "$HERE/check_pulse_cycles.py" --repo "$REPO" --avr-tools "$AVR_TOOLS" "$@" ;;
  *) sed -n '2,15p' "$0"; exit 2 ;;
esac
