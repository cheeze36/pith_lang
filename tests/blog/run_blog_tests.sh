#!/usr/bin/env bash
#
# End-to-end tests for the example blog (examples/blog/blog.pith): starts it
# against a fresh database and drives it with curl like a browser would, as
# two users. Covers accounts, sessions, CSRF, posts, comments, authorization,
# validation, escaping, search, pagination, the JSON API, the RSS feed, static
# files and error pages. Run from the pith_lang directory:
#     tests/blog/run_blog_tests.sh [path/to/pith_lang]
# Exits non-zero if any check fails. Requires curl.

set -u
EXECUTABLE="${1:-./build/pith_lang}"
PORT=18120
BASE="http://127.0.0.1:$PORT"
WORK="$(mktemp -d)"
LOG="$WORK/server.log"
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

contains() { # name needle haystack
    if printf '%s' "$3" | grep -qF -- "$2"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $1 (missing: $2)"
    fi
}

lacks() { # name needle haystack
    if printf '%s' "$3" | grep -qF -- "$2"; then
        fail=$((fail + 1))
        echo "FAIL $1 (should not contain: $2)"
    else
        pass=$((pass + 1))
    fi
}

command -v curl >/dev/null || { echo "SKIP blog tests (curl not installed)"; exit 0; }

# Each user is a cookie jar, like a separate browser.
ALICE="$WORK/alice.jar"
BOB="$WORK/bob.jar"
GUEST="$WORK/guest.jar"
touch "$ALICE" "$BOB" "$GUEST"

page()     { curl -s -b "$1" -c "$1" "$BASE$2"; }                                      # jar path
status()   { curl -s -b "$1" -c "$1" -o /dev/null -w '%{http_code}' "$BASE$2"; }       # jar path
post()     { local jar="$1" path="$2"; shift 2; curl -s -b "$jar" -c "$jar" -o "$WORK/body" -w '%{http_code} %{redirect_url}' "$@" "$BASE$path"; }
body()     { cat "$WORK/body"; }
csrf_of()  { page "$1" "$2" | grep -o 'name="csrf" value="[^"]*"' | head -1 | sed 's/.*value="//; s/"$//'; }

BLOG_PORT=$PORT BLOG_DB="$WORK/blog.db" BLOG_SECRET="test-secret" BLOG_ALLOW_STOP=1 \
    "$EXECUTABLE" examples/blog/blog.pith > "$LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 600); do grep -q "Blog running" "$LOG" 2>/dev/null && break; sleep 0.1; done   # slow (debug) builds start slowly

# --- First visit ---
check "front page" "200" "$(status "$GUEST" /)"
contains "empty state" "No posts yet." "$(page "$GUEST" /)"
check "stylesheet" "200 text/css; charset=utf-8" \
      "$(curl -s -o /dev/null -w '%{http_code} %{content_type}' "$BASE/static/style.css")"
check "new post needs login" "303 $BASE/login" "$(post "$GUEST" /new -G)"

# --- Registration ---
check "invalid username rejected" "422 " "$(post "$GUEST" /register -d 'username=a!&password=longenough')"
contains "invalid username message" "3 to 20 letters" "$(body)"
check "short password rejected" "422 " "$(post "$GUEST" /register -d 'username=alice&password=short')"
contains "username kept in the form" 'value="alice"' "$(body)"
check "alice registers" "303 $BASE/" "$(post "$ALICE" /register -d 'username=alice&password=wonderland1')"
contains "session cookie is HttpOnly" "#HttpOnly_" "$(cat "$ALICE")"   # curl's cookie-jar marker
check "duplicate username rejected" "422 " "$(post "$GUEST" /register -d 'username=alice&password=different1')"
contains "duplicate username message" "That username is taken." "$(body)"
contains "header shows the user" '<span class="user">alice</span>' "$(page "$ALICE" /)"

# --- Writing posts ---
check "post without CSRF token refused" "403 " "$(post "$ALICE" /new -d 'title=Sneaky&body=text')"
TOKEN="$(csrf_of "$ALICE" /new)"
check "empty title rejected" "422 " "$(post "$ALICE" /new --data-urlencode "csrf=$TOKEN" -d 'title=&body=kept text')"
contains "validation keeps the text" "kept text" "$(body)"
check "alice publishes" "303 $BASE/posts/hello-script-world-script" \
      "$(post "$ALICE" /new --data-urlencode "csrf=$TOKEN" --data-urlencode 'title=Hello <script>World</script>' \
              --data-urlencode $'body=First paragraph.\n\nSecond paragraph, café.')"
POST_PAGE="$(page "$GUEST" /posts/hello-script-world-script)"
contains "title is escaped" "Hello &lt;script&gt;World&lt;/script&gt;" "$POST_PAGE"
lacks "no raw script tag" "<script>World" "$POST_PAGE"
check "body split into paragraphs" "2" "$(printf '%s' "$POST_PAGE" | grep -cE '<p>(First paragraph|Second paragraph)')"
lacks "guests see no edit link" "/edit" "$POST_PAGE"
check "same title gets a new slug" "303 $BASE/posts/hello-script-world-script-2" \
      "$(post "$ALICE" /new --data-urlencode "csrf=$TOKEN" --data-urlencode 'title=Hello <script>World</script>' -d 'body=Again')"

# --- Comments and a second user ---
TOKEN="$(csrf_of "$ALICE" /posts/hello-script-world-script)"
check "empty comment rejected" "422 " "$(post "$ALICE" /posts/hello-script-world-script/comments --data-urlencode "csrf=$TOKEN" -d 'body=')"
check "alice comments" "303 $BASE/posts/hello-script-world-script#comments" \
      "$(post "$ALICE" /posts/hello-script-world-script/comments --data-urlencode "csrf=$TOKEN" --data-urlencode 'body=Thanks <b>all</b>')"
contains "comment is escaped" "Thanks &lt;b&gt;all&lt;/b&gt;" "$(page "$GUEST" /posts/hello-script-world-script)"
check "bob registers" "303 $BASE/" "$(post "$BOB" /register -d 'username=bob&password=builder123')"
TOKEN_BOB="$(csrf_of "$BOB" /posts/hello-script-world-script)"
check "bob comments" "303 $BASE/posts/hello-script-world-script#comments" \
      "$(post "$BOB" /posts/hello-script-world-script/comments --data-urlencode "csrf=$TOKEN_BOB" -d 'body=Great post')"
contains "comment count on the front page" "2 comments" "$(page "$GUEST" /)"
check "bob cannot open alice's edit form" "403" "$(status "$BOB" /posts/hello-script-world-script/edit)"
check "bob cannot edit alice's post" "403 " \
      "$(post "$BOB" /posts/hello-script-world-script/edit --data-urlencode "csrf=$TOKEN_BOB" -d 'title=Hacked&body=x')"
check "bob cannot delete alice's post" "403 " "$(post "$BOB" /posts/hello-script-world-script/delete --data-urlencode "csrf=$TOKEN_BOB")"
check "alice's token is useless to bob" "403 " \
      "$(post "$BOB" /posts/hello-script-world-script/comments --data-urlencode "csrf=$TOKEN" -d 'body=forged')"

# --- Editing ---
TOKEN="$(csrf_of "$ALICE" /posts/hello-script-world-script/edit)"
check "alice edits her post" "303 $BASE/posts/hello-script-world-script" \
      "$(post "$ALICE" /posts/hello-script-world-script/edit --data-urlencode "csrf=$TOKEN" -d 'title=Hello again' -d 'body=Edited text.')"
contains "edit is visible" "Edited text." "$(page "$GUEST" /posts/hello-script-world-script)"

# --- JSON API and RSS ---
API="$(curl -s "$BASE/api/posts")"
contains "API lists posts" '"slug":"hello-script-world-script"' "$API"
contains "API keeps raw text (JSON, not HTML)" '"title":"Hello <script>World</script>"' "$API"
check "API content type" "application/json" "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/api/posts")"
contains "API single post" '"body":"Edited text."' "$(curl -s "$BASE/api/posts/hello-script-world-script")"
check "API missing post" "404" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/posts/nope")"
FEED="$(curl -s "$BASE/feed.xml")"
check "feed has an item per post" "2" "$(printf '%s' "$FEED" | grep -o '<item>' | wc -l | tr -d ' ')"
contains "feed escapes titles" "Hello &lt;script&gt;World&lt;/script&gt;" "$FEED"

# --- Search ---
contains "search finds by text" "1 result" "$(page "$GUEST" "/search?q=Edited")"
contains "search with no match" "0 results" "$(page "$GUEST" "/search?q=zebra")"
contains "LIKE wildcards are literal" "0 results" "$(page "$GUEST" "/search?q=%25")"
contains "SQL injection is just text" "0 results" "$(page "$GUEST" "/search?q=%27%20OR%201%3D1%20--")"

# --- Pagination ---
TOKEN="$(csrf_of "$ALICE" /new)"
for i in 1 2 3 4 5; do post "$ALICE" /new --data-urlencode "csrf=$TOKEN" -d "title=Filler $i" -d 'body=Text' > /dev/null; done
contains "first page links to older posts" "Older &rarr;" "$(page "$GUEST" /)"
PAGE2="$(page "$GUEST" "/?page=2")"
contains "second page links back" "&larr; Newer" "$PAGE2"
contains "oldest post is on page 2" "hello-script-world-script-2" "$PAGE2"
check "non-numeric page is a 400" "400" "$(status "$GUEST" "/?page=abc")"
check "page 0 is a 400" "400" "$(status "$GUEST" "/?page=0")"

# --- Logging in and out ---
check "wrong password" "401 " "$(post "$GUEST" /login -d 'username=alice&password=wrongpassword')"
WRONG_PASSWORD="$(body)"
check "unknown user" "401 " "$(post "$GUEST" /login -d 'username=nobody&password=whatever123')"
contains "same message for both" "Wrong username or password." "$WRONG_PASSWORD$(body)"
cp "$ALICE" "$WORK/alice_old.jar"
check "logout without CSRF token refused" "403 " "$(post "$ALICE" /logout -d "csrf=")"
TOKEN="$(csrf_of "$ALICE" /)"
check "alice logs out" "303 $BASE/" "$(post "$ALICE" /logout --data-urlencode "csrf=$TOKEN")"
check "logged out: new post redirects" "303" "$(status "$ALICE" /new)"
check "old session cookie no longer works" "303" "$(status "$WORK/alice_old.jar" /new)"
check "alice logs back in" "303 $BASE/" "$(post "$ALICE" /login -d 'username=alice&password=wonderland1')"

# --- Deleting (comments go with the post) ---
TOKEN="$(csrf_of "$ALICE" /posts/hello-script-world-script)"
check "alice deletes her post" "303 $BASE/" "$(post "$ALICE" /posts/hello-script-world-script/delete --data-urlencode "csrf=$TOKEN")"
check "deleted post is gone" "404" "$(status "$GUEST" /posts/hello-script-world-script)"
lacks "deleted post leaves the API" '"slug":"hello-script-world-script"' "$(curl -s "$BASE/api/posts")"
printf 'import "sqlite"\nDatabase db = sqlite.open("%s")\nprint(db.query("SELECT count(*) AS n FROM comments")[0]["n"])\n' \
    "$WORK/blog.db" > "$WORK/count_comments.pith"
check "its comments are deleted too (ON DELETE CASCADE)" "0" "$("$EXECUTABLE" "$WORK/count_comments.pith" 2>&1)"

# --- Errors and protocol details ---
check "unknown page" "404" "$(status "$GUEST" /no/such/page)"
contains "404 page is styled" "There is nothing at this address." "$(page "$GUEST" /posts/nope)"
check "wrong method" "405" "$(status "$GUEST" /logout)"
check "HEAD has no body" "0" "$(curl -s -I "$BASE/" -o /dev/null -w '%{size_download}')"

# --- Shutdown and server log ---
curl -s -X POST "$BASE/stop" > /dev/null
for _ in $(seq 1 30); do kill -0 $SERVER_PID 2>/dev/null || break; sleep 0.1; done
check "server stopped cleanly" "Blog stopped." "$(tail -1 "$LOG")"
check "no handler errors in the log" "0" "$(grep -c '^\[http\]' "$LOG")"
# When run against a sanitizer build, memory errors would be reported here.
check "no sanitizer reports in the log" "0" "$(grep -cE 'AddressSanitizer|runtime error' "$LOG")"
kill $SERVER_PID 2>/dev/null
rm -rf "$WORK"

echo "blog tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
