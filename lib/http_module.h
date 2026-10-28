/**
 * @file http_module.h
 * @brief Native `http` module for Pith: an HTTP/1.1 server with routing.
 *
 * ## Routing (recommended)
 *
 * @code
 *   import "http"
 *   import "json"
 *
 *   int server = http.listen(8080)            # 127.0.0.1:8080
 *
 *   define void home(map<string, string> req):
 *       http.respond(server, 200, "<h1>Hello</h1>", "text/html; charset=utf-8")
 *
 *   define void show_user(map<string, string> req):
 *       int id = int(http.param(req, "id"))
 *       http.respond(server, 200, json.encode({"id": id}), "application/json")
 *
 *   http.route(server, "GET", "/", home)
 *   http.route(server, "GET", "/users/:id", show_user)
 *   http.static(server, "/assets", "public")  # files in ./public
 *   http.serve(server)                        # runs until http.stop(server)
 * @endcode
 *
 * Handlers take the request map and answer it with http.respond(). If a handler
 * raises an error the client gets a 500, the error is logged to stderr, and the
 * server keeps running; a handler that returns without responding also gets a
 * 500. Unknown paths get 404, a known path with the wrong method gets 405 (with
 * an Allow header), and GET routes also answer HEAD (headers only).
 *
 * ## The accept() loop (low level)
 *
 * @code
 *   while (true):
 *       map<string, string> req = http.accept(server)
 *       http.respond(server, 200, "you asked for " + req["path"])
 * @endcode
 *
 * ## Functions
 *
 *  - `http.listen(port [, host])`  Start listening; returns a server handle (int).
 *                                  @c host defaults to "127.0.0.1" (local only);
 *                                  pass "0.0.0.0" to accept connections from other machines.
 *  - `http.route(server, method, pattern, handler)`
 *                                  Register a handler. @c method is "GET", "POST", ...
 *                                  or "*" for any. @c pattern segments are literal text,
 *                                  ":name" (one segment, read with http.param) or a
 *                                  final "*" (the rest of the path, http.param(req, "*")).
 *                                  Routes are tried in the order they were added.
 *  - `http.static(server, prefix, directory)`
 *                                  Serve the files below @c directory at URL @c prefix
 *                                  (GET/HEAD). "prefix/" serves index.html. Paths that
 *                                  would leave the directory ("..") are refused.
 *  - `http.serve(server)`          Run the server, dispatching requests to routes,
 *                                  until a handler calls http.stop(server).
 *  - `http.stop(server)`           Make serve() return after the current request.
 *  - `http.accept(server)`         Wait for the next request and return it (low level).
 *  - `http.respond(server, status, body [, content_type | headers])`
 *                                  Answer the current request. The optional 4th argument
 *                                  is a content type string or a map<string, string> of
 *                                  response headers (which may include Content-Type),
 *                                  e.g. {"Location": "/login"} for a redirect. The
 *                                  content type defaults to "text/plain; charset=utf-8".
 *                                  Content-Length/Connection are set by the module;
 *                                  header values containing line breaks are rejected.
 *  - `http.header(req, name)`      A request header by case-insensitive name, or "".
 *  - `http.param(req, name)`       A path parameter of the matched route.
 *  - `http.parse_query(text)`      Parse "a=1&b=x%20y" (a query string or a form
 *                                  body) into a map<string, string>.
 *  - `http.url_decode(text)`       Decode %XX escapes and '+'.
 *  - `http.url_encode(text)`       Percent-encode for use in a URL.
 *  - `http.close(server)`          Stop listening and close all connections.
 *
 * The request map has the keys "method", "path" (as sent, still percent-encoded),
 * "query", "version" and "body", plus the headers and route parameters (read
 * those with http.header / http.param).
 *
 * ## Concurrency and limits
 *
 *  - Connections are handled by a non-blocking event loop: many clients can be
 *    connected at once, and slow or idle clients never block the others.
 *    Handlers themselves run one at a time, so a handler that sleeps or does
 *    slow work delays every other request.
 *  - Keep-alive is supported (HTTP/1.1 default); idle connections close after
 *    5 s, a request must arrive within 10 s, and a response must be accepted
 *    by the client within 30 s.
 *  - Request and response bodies are binary-safe. Request bodies are limited to
 *    8 MB, headers to 64 KB, static files to 64 MB. Chunked request bodies are
 *    not supported (answered with 501).
 *  - Malformed requests are answered with 400 by the module and never reach Pith.
 */

#ifndef PITH_HTTP_MODULE_H
#define PITH_HTTP_MODULE_H

#include "value.h"

/**
 * @brief Registers the `http` module and all of its functions.
 *
 * Builds the module's function table and installs it into @p modules under the
 * key "http". On Windows it also initialises Winsock. Called once at start-up
 * from register_all_native_modules().
 *
 * @param modules The interpreter's native-module registry
 *                (its global `native_module_funcs` hashmap).
 */
void http_register_module(HashMap *modules);

#endif // PITH_HTTP_MODULE_H
