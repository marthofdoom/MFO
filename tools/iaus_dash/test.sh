#!/usr/bin/env bash
# Fixture test for the IAUS dashboard's non-interactive modes. No curses, no network.
set -u
cd "$(dirname "$0")" || exit 1
fail=0
OUT=$(mktemp)
trap 'rm -f "$OUT" "$OUT.bad"' EXIT
check() { # name expected_rc cmd...
  local name=$1 want=$2; shift 2
  "$@" >"$OUT" 2>&1; local rc=$?
  if [ "$rc" -eq "$want" ]; then echo "PASS $name (rc=$rc)"; else echo "FAIL $name (rc=$rc, want $want)"; head -5 "$OUT"; fail=1; fi
}
F=fixtures
for f in $F/run-*.json; do check "json-check $(basename "$f")" 0 python3 dash.py --json-check "$f"; done
echo '{"schema": 2, "classes": 5}' > "$OUT.bad"
check "json-check bad file" 1 python3 dash.py --json-check "$OUT.bad"
check "json-check missing file" 1 python3 dash.py --json-check /nonexistent-iaus-file.json
check "report newest" 0 python3 dash.py --runs $F --report
check "report run+compare" 0 python3 dash.py --runs $F --report --run 20261109-091500-fx3 --compare 20261101-100000-fx1
check "report unknown run" 2 python3 dash.py --runs $F --report --run nope
check "report empty dir" 2 python3 dash.py --runs /nonexistent-iaus-dir --report
if python3 dash.py --runs $F --report --run 20261101-100000-fx1 | grep -q "FAILING RULINGS (2 of 4)"; then echo "PASS run1 has the 2 failing rulings"; else echo "FAIL run1 failing rulings"; fail=1; fi
if python3 dash.py --runs $F --report --run 20261109-091500-fx3 | grep -q "FAILING RULINGS (0 of 4)"; then echo "PASS run3 has 0 failing rulings"; else echo "FAIL run3 failing rulings"; fail=1; fi
if python3 -c "import dash"; then echo "PASS import"; else echo "FAIL import"; fail=1; fi
exit $fail
