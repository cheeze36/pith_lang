#!/usr/bin/env bash
#
# Command-line regression tests: behaviour that a single .pith file compared
# against .expected output cannot check (error messages and line numbers on
# stderr, exit codes, the REPL, `-i` mode, running from another directory).
# Run from the pith_lang directory:
#     tests/cli/run_cli_tests.sh [path/to/pith_lang]
# Exits non-zero if any check fails.

set -u
EXECUTABLE="$(cd "$(dirname "${1:-./build/pith_lang}")" && pwd)/$(basename "${1:-./build/pith_lang}")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0
fail=0

check() { # name expected actual
    if [ "$2" == "$3" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $1"
        echo "  expected: $2"
        echo "  actual:   $3"
    fi
}

# Error line numbers stay right after a string literal that spans lines.
printf 'string a = "line one\nline two"\nint x = "oops"\n' > "$WORK/lines.pith"
check "line number after multi-line string" "[line 3]" \
      "$("$EXECUTABLE" "$WORK/lines.pith" 2>&1 | head -1 | cut -d' ' -f1-2)"

# REPL: functions and classes defined on earlier inputs keep working,
# including after an error.
repl_output="$(printf 'define int twice(int n):\n    return n * 2\n\nclass Box:\n    int v\n    define init(int x):\n        this.v = x\n\nint broken = "x"\nprint(twice(21))\nBox b = Box(7)\nprint(b.v)\n' \
    | "$EXECUTABLE" 2>/dev/null | tr -d '.>' | tr ' ' '\n' | grep -E '^[0-9]+$' | tr '\n' ' ')"
check "REPL keeps earlier definitions" "42 7 " "$repl_output"

# -i: the REPL can call functions the script defined.
printf 'define string greet(string n):\n    return "hi " + n\n' > "$WORK/lib.pith"
check "-i mode calls script functions" "hi there" \
      "$(printf 'print(greet("there"))\n' | "$EXECUTABLE" -i "$WORK/lib.pith" 2>/dev/null | grep -o 'hi there')"

# Running from another directory: the stdlib and the script's own modules are found.
mkdir -p "$WORK/app"
printf 'define string helper():\n    return "local module"\n' > "$WORK/app/helpers.pith"
printf 'import "math"\nimport "helpers"\nprint(math.abs(-2.5))\nprint(helpers.helper())\n' > "$WORK/app/main.pith"
check "imports from another directory" "2.500000 local module" \
      "$(cd / && "$EXECUTABLE" "$WORK/app/main.pith" 2>&1 | tr '\n' ' ' | sed 's/ $//')"

# Import errors name the problem.
printf 'import "no_such_module"\n' > "$WORK/missing.pith"
check "missing module" "No module named 'no_such_module'" \
      "$("$EXECUTABLE" "$WORK/missing.pith" 2>&1 | head -1 | grep -o "No module named '[a-z_]*'")"

# Native type names are reserved.
printf 'class Match:\n    int x\n' > "$WORK/reserved.pith"
check "reserved native type name" "that name is a built-in type" \
      "$("$EXECUTABLE" "$WORK/reserved.pith" 2>&1 | head -1 | grep -o 'that name is a built-in type')"

# Unrescued runaway recursion ends with an error message and exit code 1, not a crash.
printf 'define int f(int n):\n    return f(n + 1)\nprint(f(0))\n' > "$WORK/recurse.pith"
recursion_message="$("$EXECUTABLE" "$WORK/recurse.pith" 2>&1 | head -1 | grep -o 'Stack overflow')"
"$EXECUTABLE" "$WORK/recurse.pith" >/dev/null 2>&1
check "runaway recursion: exit code" "1" "$?"
check "runaway recursion: message" "Stack overflow" "$recursion_message"

# Pathologically nested source and regex patterns are errors, not crashes.
python3 -c "print('int x = ' + '(' * 100000 + '1' + ')' * 100000)" > "$WORK/parens.pith" 2>/dev/null &&
    check "deeply nested expression" "Stack overflow" \
          "$("$EXECUTABLE" "$WORK/parens.pith" 2>&1 | head -1 | grep -o 'Stack overflow')"
printf 'import "regex"\nRegex r = regex.compile("%s")\n' "$(printf '(?:%.0s' $(seq 1 5000))a$(printf ')%.0s' $(seq 1 5000))" \
    > "$WORK/regex_nesting.pith"
check "deeply nested regex groups" "nested too deeply" \
      "$("$EXECUTABLE" "$WORK/regex_nesting.pith" 2>&1 | head -1 | grep -o 'nested too deeply')"

echo "cli tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
