# Pith blog (example application)

A small multi-user blog that doubles as an end-to-end test of the language and
its standard library. Users register, log in, write posts, comment, edit and
delete their own posts; there is search, pagination, a JSON API and an RSS feed.

```
./build/pith_lang examples/blog/blog.pith      # then open http://localhost:8080
```

| Environment variable | Default | Meaning |
|---|---|---|
| `BLOG_PORT` | `8080` | Port to listen on (127.0.0.1 only) |
| `BLOG_DB` | `examples/blog/blog.db` | SQLite database file (created on first run) |
| `BLOG_SECRET` | random per run | Secret for CSRF tokens; set it so open forms survive a restart |
| `BLOG_ALLOW_STOP` | unset | `1` enables `POST /stop` (used by the automated tests) |

## Files

- `blog.pith` - the whole application: configuration, schema, helpers, request handlers, routes
- `views/` - templates; every page includes the `header` and `footer` partials
- `public/style.css` - served at `/static/style.css`

## What it exercises

| Module | Used for |
|---|---|
| `http` | routes with `:slug` parameters, forms, query strings, cookies, 303 redirects, static files, `stop()` |
| `sqlite` | users, sessions, posts, comments; foreign keys with `ON DELETE CASCADE`; parameters everywhere |
| `crypto` | `hash_password` / `verify_password`, `random_token` sessions (stored as SHA-256 hashes), HMAC CSRF tokens, `timing_safe_equal` |
| `template` | all pages, shared partials, helpers (`date`, `paragraphs`, `excerpt`), auto-escaping |
| `json` | `GET /api/posts` and `GET /api/posts/:slug` |
| `regex` | URL slugs from titles, username validation, paragraph splitting |
| `buffer` | the RSS feed (`/feed.xml`) is assembled in a buffer and freed afterwards |
| `time`, `io`, `sys`, `encoding` | dates, paths relative to the script, configuration, hex/base64url |

## Security measures

- Passwords are stored with PBKDF2 (`crypto.hash_password`); login gives the same
  message for an unknown user and a wrong password.
- Session cookies are `HttpOnly` and `SameSite=Lax`; only a hash of each session
  token is stored, so a leaked database cannot be used to log in.
- Every state-changing form carries a CSRF token derived from the session with
  HMAC and checked with a timing-safe comparison.
- All SQL uses parameters (including search, whose `LIKE` wildcards are escaped);
  all output is escaped by the templates (titles with `<script>` are shown as text).
- Only a post's author can edit or delete it.

## Tests

`tests/blog/run_blog_tests.sh` starts the blog on a fresh database and drives it
with curl as two users (69 checks: accounts, sessions, CSRF, authorization,
validation, escaping, search, pagination, API, RSS, errors). It runs as part of
`./run_tests.sh`.
