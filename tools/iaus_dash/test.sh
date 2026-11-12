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
check "report schema 2 run+compare" 0 python3 dash.py --runs $F --report --run 20261112-080000-fx6 --compare 20261110-080000-fx4
check "report unknown run" 2 python3 dash.py --runs $F --report --run nope
check "report empty dir" 2 python3 dash.py --runs /nonexistent-iaus-dir --report
if python3 dash.py --runs $F --report --run 20261101-100000-fx1 | grep -q "FAILING RULINGS (2 of 4)"; then echo "PASS run1 has the 2 failing rulings"; else echo "FAIL run1 failing rulings"; fail=1; fi
if python3 dash.py --runs $F --report --run 20261109-091500-fx3 | grep -q "FAILING RULINGS (0 of 4)"; then echo "PASS run3 has 0 failing rulings"; else echo "FAIL run3 failing rulings"; fail=1; fi
# schema 2 (sim batch 2026-11-12): the Classes page's Rating (Tx) column, and Gap + Trend filled by a NORMAL class report run
# (no named scenario rows, a gap ledger that names class groups). fx4 = no slugs on the ledger rows, fx6 = the run after it on the same list.
echo '{"schema": 3, "run_id": "x", "time": "t", "list": "l", "classes": []}' > "$OUT.bad"
check "json-check unknown schema 3" 1 python3 dash.py --json-check "$OUT.bad"
echo '{"schema": 2, "run_id": "x", "time": "t", "list": "l", "classes": [], "rating": {"classes": [{"class": "a", "score": "high"}]}, "class_gaps": [{"class": "a"}]}' > "$OUT.bad"
check "json-check bad rating / class_gaps" 1 python3 dash.py --json-check "$OUT.bad"
R6=$(python3 dash.py --runs $F --report --run 20261112-080000-fx6)
row() { echo "$R6" | awk -v c="$1" '/^CLASSES/{on=1;next} /^RATING/{on=0} on && $1==c'; }
if echo "$R6" | grep -q "^Class  *T  *Rating (Tx)  *Line  *Rul  *Gap  *Trend"; then echo "PASS classes header: Rating (Tx) is column 3, Line after it"; else echo "FAIL classes header"; fail=1; fi
if row Knight | grep -Eq "^Knight +2 +T2 [0-9]/[0-9] [+-][0-9.]+( !)? +Melee +[0-9]+/[0-9]+ +[0-9.]+ +[v^=] "; then echo "PASS a class report run fills Rating, Gap and Trend (Knight)"; else echo "FAIL Knight row: $(row Knight)"; fail=1; fi
if [ "$(echo "$R6" | awk '/^CLASSES/{on=1;next} /^RATING/{on=0} on && NF && $1!="Class" && $1!="Paladin" && ($0 ~ / - +- *$/ || $0 ~ /  -  +-  /)' | wc -l)" -eq 0 ]; then echo "PASS no class of the run has an empty Gap + Trend"; else echo "FAIL empty Gap/Trend cells"; fail=1; fi
if row Paladin | grep -Eq "Hybrid +[0-9]+/[0-9]+ +[0-9.]+ "; then echo "PASS the ledger's group names reach a class with no class_gaps row (Paladin via casters)"; else echo "FAIL Paladin row: $(row Paladin)"; fail=1; fi
if python3 dash.py --runs $F --report --run 20261110-080000-fx4 | awk '/^CLASSES/{on=1;next} /^RATING/{on=0} on && $1=="Paladin"' | grep -Eq "Hybrid +[0-9]+/[0-9]+ +[0-9.]+ "; then echo "PASS ...and without gaps[].classes (the dashboard's own group fallback)"; else echo "FAIL fx4 Paladin"; fail=1; fi
if echo "$R6" | grep -q "^RATING (Tx)" && echo "$R6" | grep -Eq " !"; then echo "PASS the rating table prints and flags a class below its tier"; else echo "FAIL rating table / below flag"; fail=1; fi
if python3 dash.py --runs $F --report --run 20261101-100000-fx1 | grep -q "^Class  *T  *Rating (Tx)"; then echo "PASS a schema 1 run still reports (Rating '-')"; else echo "FAIL schema 1 report"; fail=1; fi
if python3 -c "import dash"; then echo "PASS import"; else echo "FAIL import"; fail=1; fi
exit $fail
