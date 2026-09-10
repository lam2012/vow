#!/bin/sh
# Slice-3 acceptance in three parts, all against $VOW (default ./vow):
#   1. check: exit codes over every fixture (0 clean, 2 lex/parse,
#      3 check), with catalog diagnostic lines on failures.
#   2. run: exit codes through execution (runtime failures exit 5,
#      main's integer return propagates, missing main exits 3).
#      Root-dependent pass fixtures (eff-03, eff-16 shape) are
#      check-only here; the suite gate below covers their runtime.
#   3. gate: one full `vow test` of the conformance tree from the
#      repo root must exit 0 (driver spawns resolve `vow` through
#      PATH, argv paths are repo-root-relative per R3).
VOW=${VOW:-./vow}
C=../conformance
pass=0
fail=0

# Part 0: coverage tripwire (docs <-> tree consistency) fails fast.
if ! sh "$(dirname "$0")/../conformance/coverage.sh"; then
  echo "COVERAGE-DIRTY"
  exit 1
fi

chk() {
  code=0
  "$VOW" check "$C/$1" 2>/tmp/vowerr || code=$?
  if [ "$code" -eq "$2" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "MISMATCH check $1: want $2 got $code"
    return
  fi
  if [ "$2" -ne 0 ]; then
    if ! grep -Eq '^[^:]+:[0-9]+:[0-9]+: E[23][0-9]{3} ' /tmp/vowerr; then
      fail=$((fail + 1))
      echo "BAD-DIAG check $1"
    fi
  fi
}

run() {
  code=0
  "$VOW" run "$C/$1" 2>/tmp/vowerr || code=$?
  if [ "$code" -eq "$2" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "MISMATCH run $1: want $2 got $code"
  fi
}

use() {
  want="$1"
  shift
  code=0
  "$@" 2>/tmp/vowerr || code=$?
  if [ "$code" -ne 1 ]; then
    fail=$((fail + 1))
    echo "MISMATCH usage $*: want 1 got $code"
    return
  fi
  pass=$((pass + 1))
  if ! grep -Eq "^[^:]+:[0-9]+:[0-9]+: $want " /tmp/vowerr; then
    fail=$((fail + 1))
    echo "BAD-DIAG usage $* (want $want)"
  fi
}

# Usage errors (E1xxx spot checks, counted in part12).
use E1000 "$VOW" frobnicate
use E1001 "$VOW" check
use E1002 "$VOW" check "$C/no-such-file.vow"
use E1002 "$VOW" run "$C/no-such-file.vow"
use E1003 "$VOW" test "$C/no-such-dir"
use E1001 "$VOW" build
use E1001 "$VOW" build --emit-dylib
use E1001 "$VOW" build --frobnicate "$C/driver.vow"
use E1002 "$VOW" build --emit-dylib "$C/no-such-file.vow"
use E1003 "$VOW" build --emit-dylib "$C/driver.vow" -o "$C/no-such-dir/x.so"

bld() {
  want="$1"
  shift
  code=0
  "$@" 2>/tmp/vowerr || code=$?
  if [ "$code" -eq "$want" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "MISMATCH build $*: want $want got $code"
  fi
}

# Build check-failures propagate the check class and write nothing.
bld 3 "$VOW" build --emit-dylib "$C/vow/runner/run-03-nomain.vow" -o /tmp/vow-test-nomain.so
bld 2 "$VOW" build --emit-dylib "$C/vow/lexer/fail/lex-02b-big-int.vow" -o /tmp/vow-test-lexfail.so
if [ -e /tmp/vow-test-nomain.so ] || [ -e /tmp/vow-test-lexfail.so ]; then
  echo "BUILD-WROTE-ON-FAILURE"
  fail=$((fail + 1))
else
  pass=$((pass + 1))
fi

# Part 1: check.
chk vow/lexer/pass/lex-01-tokens.vow 0
chk vow/lexer/pass/lex-01-crlf.vow 0
chk vow/lexer/fail/lex-02a-bad-escape.vow 2
chk vow/lexer/fail/lex-02b-big-int.vow 2
chk vow/lexer/fail/lex-02c-bad-utf8.vow 2
chk vow/lexer/fail/lex-02d-lone-cr.vow 2
chk vow/lexer/fail/lex-02e-unexpected-token.vow 2
chk vow/lexer/fail/lex-02f-unterminated.vow 2
chk vow/grammar/pass/par-01-precedence.vow 0
chk vow/grammar/pass/par-02-for.vow 0
chk vow/grammar/pass/par-03-map-block.vow 0
chk vow/grammar/pass/par-05-import.vow 0
chk vow/grammar/pass/par-05-mod.vow 0
chk vow/grammar/fail/par-04a-dup-key.vow 3
chk vow/grammar/fail/par-04b-cycle1.vow 3
chk vow/grammar/fail/par-04b-cycle2.vow 3
chk vow/grammar/fail/par-04c-bad-hash.vow 3
chk vow/grammar/fail/par-04d-bad-effect.vow 3
chk vow/grammar/fail/par-04f-escape-root.vow 3
chk vow/grammar/fail/par-04g-call-in-let.vow 3
chk vow/grammar/fail/par-04h-unknown-name.vow 3
chk vow/grammar/fail/par-04i-same-dir-bad-hash.vow 3
chk vow/grammar/fail/par-04j-dup-func.vow 3
chk vow/grammar/fail/par-04k-reserved-root.vow 3
chk vow/grammar/fail/par-04l-bare-alias.vow 3
chk vow/grammar/fail/par-04e-computed-arg.vow 2
chk vow/effects/pass/eff-03-listing.vow 0
chk vow/effects/pass/eff-16-lock-true.vow 0
chk vow/effects/fail/eff-01-undeclared.vow 3
chk vow/effects/fail/eff-02a-read-outside.vow 0
chk vow/effects/fail/eff-02b-spawn-undeclared.vow 0
chk vow/effects/fail/eff-02c-write-outside.vow 0
chk vow/effects/fail/eff-04-no-clock-net.vow 3
chk vow/effects/fail/eff-05-reclimit.vow 0
chk vow/effects/fail/eff-06-transitive.vow 3
chk vow/effects/fail/eff-07-overflow.vow 0
chk vow/effects/fail/eff-08-divzero.vow 0
chk vow/effects/fail/eff-09-badindex.vow 0
chk vow/effects/fail/eff-10-missingkey.vow 0
chk vow/effects/fail/eff-12-nonfunc-call.vow 0
chk vow/effects/fail/eff-13-missing-file.vow 0
chk vow/effects/fail/eff-14-dynamic-effect.vow 0
chk vow/effects/fail/eff-15-type-error.vow 0
chk vow/effects/fail/eff-17-arity.vow 0
chk vow/runner/run-03-exit7.vow 0
chk vow/runner/run-03-nomain.vow 0
chk vow/runner/run-04-main-nonint.vow 0
chk vow/runner/_mini-run-01/a.vow 0
chk vow/runner/_mini-run-01/b.vow 0
chk vow/runner/_mini-run-02/c.vow 0
chk vow/runner/pass/memo-01a.vow 0
chk vow/runner/pass/memo-01b.vow 0
chk driver.vow 0

# Part 2: run.
run vow/effects/fail/eff-01-undeclared.vow 3
run vow/effects/fail/eff-02a-read-outside.vow 5
run vow/effects/fail/eff-02b-spawn-undeclared.vow 5
run vow/effects/fail/eff-02c-write-outside.vow 5
run vow/effects/fail/eff-04-no-clock-net.vow 3
run vow/effects/fail/eff-05-reclimit.vow 5
run vow/effects/fail/eff-06-transitive.vow 3
run vow/effects/fail/eff-07-overflow.vow 5
run vow/effects/fail/eff-08-divzero.vow 5
run vow/effects/fail/eff-09-badindex.vow 5
run vow/effects/fail/eff-10-missingkey.vow 5
run vow/effects/fail/eff-12-nonfunc-call.vow 5
run vow/effects/fail/eff-13-missing-file.vow 5
run vow/effects/fail/eff-14-dynamic-effect.vow 5
run vow/effects/fail/eff-15-type-error.vow 5
run vow/effects/fail/eff-17-arity.vow 5
run vow/grammar/fail/par-04a-dup-key.vow 3
run vow/runner/run-03-exit7.vow 7
run vow/runner/run-03-nomain.vow 3
run vow/runner/run-04-main-nonint.vow 5
run vow/lexer/pass/lex-01-tokens.vow 3

echo "part12: pass: $pass fail: $fail"
[ "$fail" -eq 0 ] || exit 1

# Part 3: full-suite gate from the repo root. VOWBIN defaults to the
# reference binary beside this script; callers (notably `zig build
# test`) point VOW and VOWBIN at a just-built binary anywhere. PATH
# carries that binary's directory because driver subprocesses spawn
# `vow`.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VOWBIN="${VOWBIN:-$ROOT/reference/vow}"
case "$VOWBIN" in
  /*) ;;
  *) VOWBIN="$PWD/$VOWBIN" ;;
esac
export PATH="$(dirname "$VOWBIN"):$PATH"
cd "$ROOT" || exit 1
"$VOWBIN" test conformance/ || exit 1

# Part 4: ABI slice — `vow build --emit-dylib` on the three sample
# CLIs, per library: abi.map content, exact dynamic exports,
# dlopen return equal to `vow run`, and bit-identical rebuilds.
# Needs cc, python3, nm, awk on PATH; a missing tool fails loudly
# by name. Builds land in a fresh mktemp dir; the tree is untouched.
for tool in cc python3 nm awk; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ABI-TOOL-MISSING $tool"
    exit 1
  fi
done
ABITMP=$(mktemp -d /tmp/vowabiXXXXXX) || exit 1
abipass=0
abifail=0
abi_one() {
  base="$1"
  sym="$2"
  src="examples/cli/$base.vow"
  d="$ABITMP/$base"
  d2="$ABITMP/$base-again"
  mkdir "$d" "$d2" || { echo "ABI-MKDIR $base"; exit 1; }
  if ! "$VOWBIN" build --emit-dylib "$src" -o "$d/$base.so" 2>/tmp/vowabierr; then
    echo "ABI-BUILD-FAIL $src"
    cat /tmp/vowabierr
    abifail=$((abifail + 1))
    return
  fi
  abipass=$((abipass + 1))
  if [ "$(cat "$d/abi.map")" != "$sym" ]; then
    echo "ABI-MAP $base"
    abifail=$((abifail + 1))
  else
    abipass=$((abipass + 1))
  fi
  got=$(nm -D --defined-only "$d/$base.so" | awk '{print $NF}')
  if [ "$got" != "$sym" ]; then
    echo "ABI-EXPORTS $base: [$got]"
    abifail=$((abifail + 1))
  else
    abipass=$((abipass + 1))
  fi
  "$VOWBIN" run "$src" 2>/dev/null
  want=$?
  python3 -c "import ctypes,sys; lib=ctypes.CDLL(sys.argv[1]); sys.exit(getattr(lib, sys.argv[2])())" "$d/$base.so" "$sym" 2>/dev/null
  gotcode=$?
  if [ "$gotcode" -ne "$want" ]; then
    echo "ABI-DLOPEN $base: want $want got $gotcode"
    abifail=$((abifail + 1))
  else
    abipass=$((abipass + 1))
  fi
  if ! "$VOWBIN" build --emit-dylib "$src" -o "$d2/$base.so" 2>/tmp/vowabierr; then
    echo "ABI-REBUILD-FAIL $src"
    abifail=$((abifail + 1))
    return
  fi
  if ! cmp -s "$d/$base.so" "$d2/$base.so"; then
    echo "ABI-NONIDENTICAL $base"
    abifail=$((abifail + 1))
  else
    abipass=$((abipass + 1))
  fi
}
abi_one hello vow_hello
abi_one hash-file vow_hash_file
abi_one verify-lock vow_verify_lock
echo "partABI: pass: $abipass fail: $abifail"
if [ "$abifail" -ne 0 ]; then
  echo "ABI-LEFTBEHIND $ABITMP"
  exit 1
fi
rm -rf "$ABITMP"

# Part 5: E5007 implementation-level tests (POSIX-only, by design:
# the catalog marks E5007 implementation-level only — no portable
# fixture can produce non-UTF-8 bytes, so this lives in shell, not
# in conformance/). Covers all three raise sites: spawn capture,
# fs.read, fs.list. Byte fixtures come from system printf in a
# mktemp dir, removed afterwards.
for tool in sh printf python3; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "E5007-TOOL-MISSING $tool"
    exit 1
  fi
done
E5007TMP=$(mktemp -d /tmp/vowe5007XXXXXX) || exit 1
e5007pass=0
e5007fail=0
printf 'AB\377\376CD' > "$E5007TMP/bad.bin"
nbad=$(printf 'n\377m')
touch "$E5007TMP/$nbad"
cat > "$E5007TMP/read.vow" <<'EOF'
func main() effect[fs.read["."]] {
  fs.read(["bad.bin"])
  return 0
}
EOF
cat > "$E5007TMP/list.vow" <<'EOF'
func main() effect[fs.list["."]] {
  fs.list(["."])
  return 0
}
EOF
# NOTE: the producer cannot be printf-with-octal: Vow string
# literals keep escape bytes raw (lex.c validates but does not
# process), so no backslash ever reaches the child. python3 emits
# the byte with digits only — no escapes, no shell involved.
cat > "$E5007TMP/spawn.vow" <<'EOF'
func main() effect[proc.spawn["python3"]] {
  proc.spawn(["python3", "-c", "import sys;sys.stdout.buffer.write(bytes([255]))"])
  return 0
}
EOF
e5007() {
  desc="$1"
  shift
  code=0
  "$@" 2>/tmp/vowerr5007 || code=$?
  if [ "$code" -ne 5 ]; then
    echo "MISMATCH e5007 $desc: want 5 got $code"
    e5007fail=$((e5007fail + 1))
    return
  fi
  e5007pass=$((e5007pass + 1))
  if ! grep -Eq '^[^:]+:[0-9]+:[0-9]+: E5007 ' /tmp/vowerr5007; then
    echo "BAD-DIAG e5007 $desc"
    e5007fail=$((e5007fail + 1))
  else
    e5007pass=$((e5007pass + 1))
  fi
}
e5007 read "$VOWBIN" run "$E5007TMP/read.vow"
e5007 list "$VOWBIN" run "$E5007TMP/list.vow"
e5007 spawn "$VOWBIN" run "$E5007TMP/spawn.vow"
echo "partE5007: pass: $e5007pass fail: $e5007fail"
if [ "$e5007fail" -ne 0 ]; then
  echo "E5007-LEFTBEHIND $E5007TMP"
  exit 1
fi
rm -rf "$E5007TMP"
echo "ALL-GREEN"
