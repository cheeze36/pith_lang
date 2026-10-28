#!/usr/bin/env bash
#
# Portable test runner for Pith (Linux/macOS counterpart to run_test.bat).
#
# Runs every tests/*.pith that has a matching tests/*.expected, compares the
# interpreter's stdout against the expected output, and prints PASS/FAIL plus a
# summary. Also runs the command-line tests (tests/cli/) and, when curl is
# available, the http and blog end-to-end tests (tests/http/, tests/blog/). Exits non-zero if any
# test fails.
#
# Usage:
#   ./run_tests.sh [path/to/pith_lang]
# If no binary path is given, it looks for ./build/pith_lang.

set -u

EXECUTABLE="${1:-./build/pith_lang}"
TEST_DIR="tests"
pass_count=0
fail_count=0

if [ ! -x "$EXECUTABLE" ]; then
    echo "Error: interpreter not found at '$EXECUTABLE'."
    echo "Build it first (cmake -S . -B build && cmake --build build), or pass the path."
    exit 2
fi

echo "============================"
echo "     Running Pith Tests"
echo "============================"

for expected in "$TEST_DIR"/*.expected; do
    [ -e "$expected" ] || continue
    name="$(basename "$expected" .expected)"
    test_file="$TEST_DIR/$name.pith"

    if [ ! -f "$test_file" ]; then
        echo "SKIP $name (no .pith file)"
        continue
    fi

    # Capture stdout only; expected files hold stdout.
    actual="$("$EXECUTABLE" "$test_file" 2>/dev/null)"

    if [ "$actual" == "$(cat "$expected")" ]; then
        echo "PASS $name"
        pass_count=$((pass_count + 1))
    else
        echo "FAIL $name"
        echo "  --- expected ---"; sed 's/^/  /' "$expected"
        echo "  --- actual ---";   printf '%s\n' "$actual" | sed 's/^/  /'
        fail_count=$((fail_count + 1))
    fi
done

# Command-line tests: stderr messages, exit codes, the REPL, -i, imports.
if cli_output="$(tests/cli/run_cli_tests.sh "$EXECUTABLE" 2>&1)"; then
    echo "PASS cli ($(echo "$cli_output" | tail -1))"
    pass_count=$((pass_count + 1))
else
    echo "FAIL cli"
    printf '%s\n' "$cli_output" | sed 's/^/  /'
    fail_count=$((fail_count + 1))
fi

# End-to-end http module tests (need curl; skipped if it is missing).
if command -v curl >/dev/null; then
    if http_output="$(tests/http/run_http_tests.sh "$EXECUTABLE" 2>&1)"; then
        echo "PASS http ($(echo "$http_output" | tail -1))"
        pass_count=$((pass_count + 1))
    else
        echo "FAIL http"
        printf '%s\n' "$http_output" | sed 's/^/  /'
        fail_count=$((fail_count + 1))
    fi
else
    echo "SKIP http (curl not installed)"
fi

# End-to-end tests of the example blog (examples/blog), which exercises most
# modules together (need curl; skipped if it is missing).
if command -v curl >/dev/null; then
    if blog_output="$(tests/blog/run_blog_tests.sh "$EXECUTABLE" 2>&1)"; then
        echo "PASS blog ($(echo "$blog_output" | tail -1))"
        pass_count=$((pass_count + 1))
    else
        echo "FAIL blog"
        printf '%s\n' "$blog_output" | sed 's/^/  /'
        fail_count=$((fail_count + 1))
    fi
else
    echo "SKIP blog (curl not installed)"
fi

echo "============================"
echo "        Test Summary"
echo "============================"
echo "Passed: $pass_count"
echo "Failed: $fail_count"

[ "$fail_count" -eq 0 ]
