#!/usr/bin/env bash
#
# End-to-end tests for the http module: starts tests/http/server.pith and
# exercises it with curl (routing, params, static files, errors, keep-alive,
# concurrency, slow clients). Run from the pith_lang directory:
#     tests/http/run_http_tests.sh [path/to/pith_lang]
# Exits non-zero if any check fails. Requires curl.

set -u
EXECUTABLE="${1:-./build/pith_lang}"
BASE="http://127.0.0.1:18099"
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

command -v curl >/dev/null || { echo "SKIP http tests (curl not installed)"; exit 0; }

"$EXECUTABLE" tests/http/server.pith > /tmp/pith_http_server.log 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
    grep -q ready /tmp/pith_http_server.log 2>/dev/null && break
    sleep 0.1
done

# Routing and parameters
check "GET /" "home" "$(curl -s $BASE/)"
check "path param + json" '{"id":42,"next":43}' "$(curl -s $BASE/users/42)"
check "json content type" "application/json" "$(curl -s -o /dev/null -w '%{content_type}' $BASE/users/42)"
check "POST form + 201" "201 created Ada Lovelace" \
      "$(curl -s -w '%{http_code} ' -o /tmp/pith_body -d 'name=Ada+Lovelace' $BASE/users; cat /tmp/pith_body)"
check "Location header" "/users/7" "$(curl -s -D - -o /dev/null -d 'name=x' $BASE/users | tr -d '\r' | sed -n 's/^Location: //p')"
check "wildcard param" "rest=a/b/c.txt" "$(curl -s $BASE/files/a/b/c.txt)"
check "encoded param" "rest=hello world" "$(curl -s $BASE/files/hello%20world)"
check "any-method route" "yes|q=1" "$(curl -s -X PUT -H 'X-Test: yes' "$BASE/echo?q=1")"

# Errors
check "404" "404" "$(curl -s -o /dev/null -w '%{http_code}' $BASE/nope)"
check "405" "405" "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE $BASE/users/1)"
check "405 Allow header" "GET" "$(curl -s -D - -o /dev/null -X DELETE $BASE/users/1 | tr -d '\r' | sed -n 's/^Allow: //p')"
check "handler error -> 500" "500" "$(curl -s -o /dev/null -w '%{http_code}' $BASE/crash)"
check "no response -> 500" "500" "$(curl -s -o /dev/null -w '%{http_code}' $BASE/silent)"
check "server survives errors" "home" "$(curl -s $BASE/)"
check "HEAD has no body" "0" "$(curl -s -I $BASE/ -o /dev/null -w '%{size_download}')"
check "bad percent-encoding -> 400" "400" "$(curl -s -o /dev/null -w '%{http_code}' $BASE/files/%ZZ)"

# Static files
check "static index" "<h1>index</h1>" "$(curl -s $BASE/assets/)"
check "static css type" "text/css; charset=utf-8" "$(curl -s -o /dev/null -w '%{content_type}' $BASE/assets/style.css)"
check "static nested" "nested" "$(curl -s $BASE/assets/sub/page.txt)"
curl -s $BASE/assets/blob.bin -o /tmp/pith_blob.bin
check "static binary intact" "same" "$(cmp -s /tmp/pith_blob.bin tests/http/public/blob.bin && echo same)"
check "traversal blocked" "404" "$(curl -s --path-as-is -o /dev/null -w '%{http_code}' $BASE/assets/../secret.txt)"
check "encoded traversal blocked" "404" "$(curl -s -o /dev/null -w '%{http_code}' $BASE/assets/%2e%2e/secret.txt)"
check "static missing" "404" "$(curl -s -o /dev/null -w '%{http_code}' $BASE/assets/none.css)"

# Keep-alive: several requests on one connection
reuse=$(curl -s -v $BASE/ $BASE/users/1 $BASE/ 2>&1 | grep -c 'Re-using existing connection\|Reusing existing')
check "keep-alive reuses connection" "2" "$reuse"

# A slow client that never finishes its request must not block others
(exec 3<>/dev/tcp/127.0.0.1/18099; printf 'GET / HTTP/1.1\r\nHost: x\r\n' >&3; sleep 3) &
SLOW_PID=$!
sleep 0.2
start=$(date +%s%N)
check "fast client during slow client" "home" "$(curl -s -m 2 $BASE/)"
elapsed_ms=$(( ($(date +%s%N) - start) / 1000000 ))
check "not blocked by slow client" "yes" "$([ $elapsed_ms -lt 1000 ] && echo yes || echo "no (${elapsed_ms}ms)")"
kill $SLOW_PID 2>/dev/null

# Many concurrent clients
pids=""
for i in $(seq 1 50); do curl -s -m 10 "$BASE/users/$i" -o "/tmp/pith_c_$i" & pids="$pids $!"; done
wait $pids # Not a bare `wait`: that would also wait for the server.
ok=0
for i in $(seq 1 50); do grep -q "\"id\":$i," "/tmp/pith_c_$i" && ok=$((ok + 1)); rm -f "/tmp/pith_c_$i"; done
check "50 concurrent clients" "50" "$ok"

# Malformed request line
check "malformed request -> 400" "HTTP/1.1 400 Bad Request" \
      "$(printf 'NONSENSE\r\n\r\n' | (exec 3<>/dev/tcp/127.0.0.1/18099; cat >&3; head -1 <&3) | tr -d '\r')"

# Shutdown via http.stop()
check "stop" "bye" "$(curl -s -X POST $BASE/shutdown)"
for _ in $(seq 1 30); do kill -0 $SERVER_PID 2>/dev/null || break; sleep 0.1; done
check "server exited after stop" "stopped" "$(tail -1 /tmp/pith_http_server.log)"
check "handler errors logged" "1" "$(grep -c 'GET /crash: Division by zero' /tmp/pith_http_server.log)"
kill $SERVER_PID 2>/dev/null

echo "http tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
