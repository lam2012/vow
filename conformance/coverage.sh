#!/bin/sh
# Coverage tripwire: every diagnostic code with a table row in
# docs/error-codes.md must be referenced by at least one fixture,
# expected file, driver line, or tests.sh case — unless its row says
# implementation-level/evaluator-level only. Reverse: every `code:`
# line under conformance/expected must name a table code (catches
# typos and registry drift). Exit 0 clean, 1 with a report.
# Safe to run from anywhere; wired into reference/tests.sh.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAB="$ROOT/docs/error-codes.md"
fail=0
for code in $(grep -oE '^\| E[0-9]{4} ' "$TAB" | grep -oE 'E[0-9]{4}'); do
  row=$(grep -m1 "| $code |" "$TAB")
  case "$row" in
    *[Oo]nly*)
      continue
      ;;
  esac
  if ! grep -rq "$code" "$ROOT/conformance/vow" \
      "$ROOT/conformance/expected" "$ROOT/conformance/driver.vow" \
      "$ROOT/reference/tests.sh"; then
    echo "UNCOVERED $code"
    fail=1
  fi
done
for code in $(grep -rhoE '^code: E[0-9]{4}' "$ROOT/conformance/expected" |
  grep -oE 'E[0-9]{4}'); do
  if ! grep -q "^| $code |" "$TAB"; then
    echo "UNLISTED $code"
    fail=1
  fi
done
if [ "$fail" -eq 0 ]; then
  echo "coverage: clean"
fi
exit "$fail"
