/**
 * @file http_module.c
 * @brief Implementation of the native `http` module.
 *
 * See http_module.h for the Pith-facing API, usage examples and limitations.
 *
 * ## Structure
 *
 *  - A small fixed table of servers; the Pith-visible handle is an index into it.
 *  - Every server runs a non-blocking event loop over its listening socket and
 *    all client connections (poll() on POSIX, WSAPoll() on Windows). Reading
 *    requests and writing responses are multiplexed, so a slow or idle client
 *    never stalls the others; only the Pith code that handles a request runs
 *    one request at a time.
 *  - Each connection is a small state machine:
 *        READING  -> a complete request is parsed -> HANDLING (Pith code runs)
 *        HANDLING -> respond() queues the response -> WRITING
 *        WRITING  -> fully sent -> READING again (keep-alive) or closed.
 *    Requests are parsed incrementally from the connection's input buffer, so
 *    pipelined requests and requests split across many packets both work.
 *  - Timeouts protect the server: a request must arrive within
 *    REQUEST_TIMEOUT_SECONDS, an idle keep-alive connection is closed after
 *    KEEPALIVE_IDLE_SECONDS, and a client must accept its response within
 *    WRITE_TIMEOUT_SECONDS.
 *  - Two ways to use it: the accept()/respond() loop, or route() + serve(),
 *    where serve() runs the loop itself and calls the Pith handler registered
 *    for each request's method and path (a raising handler becomes a 500 and
 *    the server keeps running).
 *
 * ## Memory / GC
 *
 * Request maps are built while pushed as GC roots (every field is a fresh
 * string allocation). Route handlers are Pith function values held in C; they
 * are kept alive through `registered_handlers`, a Pith list registered with
 * gc_register_value_root().
 *
 * Bodies are binary-safe: they are read and sent by byte length.
 */

// getaddrinfo(), poll() and friends are POSIX, not C99; request them explicitly
// so the file builds under a strict -std=c99 as well as the default gnu99.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // WSAPoll needs Windows Vista or later
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#define poll WSAPoll
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
typedef int socket_t;
#define INVALID_SOCKET_HANDLE (-1)
#endif

#include "http_module.h"
#include "interpreter.h"
#include "common.h"
#include "gc.h"
#include "pith_string.h"
#include "types.h"
#include "time_module.h"

#define MAX_SERVERS 16
#define MAX_CONNECTIONS 512 // Per server; beyond this, new clients wait in the listen backlog.
#define MAX_HEADER_BYTES (64 * 1024)
#define MAX_BODY_BYTES (8 * 1024 * 1024)
#define MAX_STATIC_FILE_BYTES (64L * 1024 * 1024)
#define REQUEST_TIMEOUT_SECONDS 10.0
#define KEEPALIVE_IDLE_SECONDS 5.0
#define WRITE_TIMEOUT_SECONDS 30.0
#define DEFAULT_CONTENT_TYPE "text/plain; charset=utf-8"

// Request headers and route parameters are stored in the request map under
// these prefixes (lower-cased header name / parameter name); see http.header()
// and http.param().
#define HEADER_KEY_PREFIX "header:"
#define PARAM_KEY_PREFIX "param:"

typedef enum
{
    CONN_READING, // Waiting for a complete request.
    CONN_HANDLING, // Request handed to Pith; waiting for respond().
    CONN_WRITING // Response queued; sending it.
} ConnState;

typedef struct
{
    socket_t socket;
    ConnState state;
    char *in; // Received bytes not yet consumed by a request.
    size_t in_length;
    size_t in_capacity;
    size_t request_length; // Bytes of `in` used by the request being handled.
    int needs_parse; // New input arrived (or old input is left over) since the last parse attempt.
    char *out; // Response being sent.
    size_t out_length;
    size_t out_sent;
    int keep_alive; // Keep the connection open after this response.
    int is_head; // HEAD request: the response has headers only.
    double deadline; // Monotonic time after which the connection is closed.
} Connection;

typedef struct
{
    char *method; // "GET", "POST", ... or "*" for any method.
    char **segments; // Path pattern split on '/': literal text, ":name" or a trailing "*".
    int segment_count;
    Value handler; // Pith function (kept alive via registered_handlers), unless static_dir is set.
    char *static_dir; // Non-NULL for http.static() mounts.
} Route;

typedef struct
{
    int is_open;
    socket_t listener;
    Connection **connections;
    int connection_count;
    int next_connection; // Round-robin start for fairness between ready connections.
    Connection *current; // Connection whose request Pith is handling, or NULL.
    Route *routes;
    int route_count;
    int stop_requested; // Set by http.stop(); serve() returns after the current request.
} HttpServer;

static HttpServer servers[MAX_SERVERS];

// All route handlers ever registered, as a Pith list so the GC keeps them alive.
static Value registered_handlers = {VAL_VOID};

// --- Small helpers ---

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/**
 * @brief Case-insensitive ASCII comparison (portable strcasecmp).
 *
 * @return 0 if equal (same convention as strcmp), non-zero otherwise.
 */
static int ascii_compare_ignore_case(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b))
            return 1;
    }
    return *a != *b;
}

/**
 * @brief Finds @p needle in the first @p length bytes of @p haystack.
 *
 * @return The offset of the first match, or -1.
 */
static long find_bytes(const char *haystack, size_t length, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= length; i++)
    {
        if (memcmp(haystack + i, needle, n) == 0)
            return (long) i;
    }
    return -1;
}

static const char *status_reason(int status)
{
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

// --- Socket helpers ---

static void close_socket(socket_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static void set_nonblocking(socket_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

/** @brief True if the last socket call failed only because it would block. */
static int would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// --- Connections ---

static Connection *connection_new(socket_t s)
{
    Connection *c = calloc(1, sizeof(Connection));
    c->socket = s;
    c->state = CONN_READING;
    c->deadline = time_monotonic_seconds() + REQUEST_TIMEOUT_SECONDS;
    return c;
}

static void connection_free(Connection *c)
{
    close_socket(c->socket);
    free(c->in);
    free(c->out);
    free(c);
}

/**
 * @brief Removes connection @p index from @p server and closes it.
 */
static void server_drop_connection(HttpServer *server, int index)
{
    Connection *c = server->connections[index];
    if (server->current == c)
        server->current = NULL;
    connection_free(c);
    server->connections[index] = server->connections[--server->connection_count];
}

/**
 * @brief Sends as much pending output as the socket accepts without blocking.
 *
 * @return 1 if everything has been sent, 0 if output remains, -1 on a dead socket.
 */
static int connection_flush(Connection *c)
{
#ifdef MSG_NOSIGNAL
    // Without this, writing to a client that disconnected raises SIGPIPE and
    // kills the whole interpreter on Linux.
    int flags = MSG_NOSIGNAL;
#else
    int flags = 0;
#endif
    while (c->out_sent < c->out_length)
    {
        size_t remaining = c->out_length - c->out_sent;
        int chunk = remaining > 1048576 ? 1048576 : (int) remaining;
        int sent = (int) send(c->socket, c->out + c->out_sent, chunk, flags);
        if (sent > 0)
        {
            c->out_sent += (size_t) sent;
            continue;
        }
        if (sent < 0 && would_block())
            return 0;
        return -1;
    }
    return 1;
}

/**
 * @brief Called when a connection's response has been fully sent.
 *
 * @return 1 to keep the connection (back to READING), 0 to close it.
 */
static int connection_finish_response(Connection *c)
{
    free(c->out);
    c->out = NULL;
    c->out_length = c->out_sent = 0;
    if (!c->keep_alive)
        return 0;
    // Drop the consumed request; any pipelined bytes behind it stay buffered.
    memmove(c->in, c->in + c->request_length, c->in_length - c->request_length);
    c->in_length -= c->request_length;
    c->request_length = 0;
    c->state = CONN_READING;
    c->needs_parse = c->in_length > 0;
    c->deadline = time_monotonic_seconds() + (c->in_length > 0 ? REQUEST_TIMEOUT_SECONDS : KEEPALIVE_IDLE_SECONDS);
    return 1;
}

/**
 * @brief Queues a complete response on @p c and starts sending it.
 *
 * @return 1 to keep the connection, 0 if it should be closed now.
 */
static int connection_queue_response(Connection *c, int status, const char *content_type, const char *extra_headers,
                                     const char *body, size_t body_length)
{
    if (extra_headers == NULL)
        extra_headers = "";
    size_t head_size = strlen(content_type) + strlen(extra_headers) + 256;
    size_t send_body = c->is_head ? 0 : body_length; // HEAD: headers only, same Content-Length.
    char *out = malloc(head_size + send_body);
    int head_length = snprintf(out, head_size,
                               "HTTP/1.1 %d %s\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %lu\r\n"
                               "Connection: %s\r\n"
                               "%s"
                               "\r\n",
                               status, status_reason(status), content_type, (unsigned long) body_length,
                               c->keep_alive ? "keep-alive" : "close", extra_headers);
    memcpy(out + head_length, body, send_body);

    free(c->out);
    c->out = out;
    c->out_length = (size_t) head_length + send_body;
    c->out_sent = 0;
    c->state = CONN_WRITING;
    c->deadline = time_monotonic_seconds() + WRITE_TIMEOUT_SECONDS;

    int flushed = connection_flush(c); // Most responses fit in the socket buffer right away.
    if (flushed < 0)
        return 0;
    if (flushed == 1)
        return connection_finish_response(c);
    return 1;
}

// --- Request parsing ---

static void map_set_bytes(HashMap *map, const char *key, const char *bytes, int length)
{
    hashmap_set(map, key, string_value(bytes, length), 0);
}

static void map_set_string(HashMap *map, const char *key, const char *str)
{
    map_set_bytes(map, key, str, (int) strlen(str));
}

/**
 * @brief Tries to parse one complete request from the front of @p c->in.
 *
 * @param request_out Receives the request map when one is complete.
 * @param error_status Receives the HTTP status to answer with on a malformed request.
 * @return 1 = complete request, 0 = need more bytes, -1 = malformed (answer with *error_status).
 */
static int parse_request(Connection *c, HashMap **request_out, int *error_status)
{
    *error_status = 400;
    long header_end = find_bytes(c->in, c->in_length, "\r\n\r\n");
    if (header_end < 0)
    {
        if (c->in_length > MAX_HEADER_BYTES)
        {
            *error_status = 431;
            return -1;
        }
        return 0;
    }

    // Work on a NUL-terminated copy of the header block.
    char *head = malloc((size_t) header_end + 1);
    memcpy(head, c->in, (size_t) header_end);
    head[header_end] = '\0';
    size_t body_start = (size_t) header_end + 4;

    // Request line: METHOD SP TARGET SP VERSION
    char *line_end = strstr(head, "\r\n");
    char *headers = line_end ? line_end + 2 : head + header_end;
    if (line_end)
        *line_end = '\0';
    char *method = head;
    char *target = strchr(method, ' ');
    char *version = target ? strchr(target + 1, ' ') : NULL;
    if (target == NULL || version == NULL)
    {
        free(head);
        return -1;
    }
    *target++ = '\0';
    *version++ = '\0';
    if (*method == '\0' || *target != '/' || strncmp(version, "HTTP/1.", 7) != 0)
    {
        free(head);
        return -1;
    }

    // First pass over the headers: what is needed to know whether the body is complete.
    long content_length = 0;
    int connection_close = 0, connection_keep_alive = 0, chunked = 0;
    for (char *line = headers; line < head + header_end;)
    {
        char *next = strstr(line, "\r\n");
        size_t len = next ? (size_t) (next - line) : strlen(line);
        char *colon = memchr(line, ':', len);
        if (colon != NULL)
        {
            size_t name_len = (size_t) (colon - line);
            const char *value = colon + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            char name[64];
            snprintf(name, sizeof(name), "%.*s", (int) (name_len < 63 ? name_len : 63), line);
            if (ascii_compare_ignore_case(name, "content-length") == 0)
            {
                char *end;
                content_length = strtol(value, &end, 10);
                if (end == value || content_length < 0)
                {
                    free(head);
                    return -1;
                }
            }
            else if (ascii_compare_ignore_case(name, "transfer-encoding") == 0)
                chunked = 1;
            else if (ascii_compare_ignore_case(name, "connection") == 0)
            {
                // Connection tokens are case-insensitive ("close", "Keep-Alive", ...).
                char tokens[128];
                size_t n = (size_t) (line + len - value);
                if (n >= sizeof(tokens))
                    n = sizeof(tokens) - 1;
                for (size_t k = 0; k < n; k++)
                    tokens[k] = (char) tolower((unsigned char) value[k]);
                tokens[n] = '\0';
                if (strstr(tokens, "close") != NULL)
                    connection_close = 1;
                if (strstr(tokens, "keep-alive") != NULL)
                    connection_keep_alive = 1;
            }
        }
        if (next == NULL)
            break;
        line = next + 2;
    }
    if (chunked)
    {
        *error_status = 501; // Chunked request bodies are not supported.
        free(head);
        return -1;
    }
    if (content_length > MAX_BODY_BYTES)
    {
        *error_status = 413;
        free(head);
        return -1;
    }
    if (c->in_length < body_start + (size_t) content_length)
    {
        free(head);
        return 0; // Body not complete yet.
    }

    // HTTP/1.1 keeps connections open unless told otherwise; HTTP/1.0 only on request.
    c->keep_alive = strcmp(version, "HTTP/1.1") == 0 ? !connection_close : connection_keep_alive;
    c->is_head = strcmp(method, "HEAD") == 0;
    c->request_length = body_start + (size_t) content_length;

    char *query = strchr(target, '?');
    if (query != NULL)
        *query++ = '\0';

    HashMap *request = hashmap_create(VAL_STRING, VAL_VOID);
    request->value_spec = type_from_name("string");
    gc_push_root((ObjHeader *) request); // Each field below is a fresh allocation.
    map_set_string(request, "method", method);
    map_set_string(request, "path", target);
    map_set_string(request, "query", query ? query : "");
    map_set_string(request, "version", version);

    // Second pass: store every header, lower-cased, under HEADER_KEY_PREFIX.
    for (char *line = headers; line < head + header_end;)
    {
        char *next = strstr(line, "\r\n");
        if (next != NULL)
            *next = '\0';
        char *colon = strchr(line, ':');
        if (colon != NULL)
        {
            *colon = '\0';
            char *value = colon + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            char key[256];
            snprintf(key, sizeof(key), "%s%s", HEADER_KEY_PREFIX, line);
            for (char *k = key; *k; k++)
                *k = (char) tolower((unsigned char) *k);
            map_set_string(request, key, value);
        }
        if (next == NULL)
            break;
        line = next + 2;
    }
    map_set_bytes(request, "body", c->in + body_start, (int) content_length); // May contain NUL bytes.
    gc_pop_root();
    free(head);

    *request_out = request;
    return 1;
}

// --- The event loop ---

/**
 * @brief Runs the server's event loop until a complete request is available.
 *
 * Accepts connections, reads and parses requests, sends queued responses and
 * enforces timeouts. Malformed requests are answered here (400/413/431/501)
 * without reaching Pith.
 *
 * @return The request map; the connection becomes server->current.
 */
static HashMap *next_request(HttpServer *server)
{
    struct pollfd *fds = malloc(sizeof(struct pollfd) * (MAX_CONNECTIONS + 1));
    for (;;)
    {
        // 1. Hand out a request that is already complete in some buffer
        //    (round-robin, so one busy keep-alive client cannot starve others).
        int count = server->connection_count;
        for (int k = 0; k < count; k++)
        {
            int i = (server->next_connection + k) % count;
            Connection *c = server->connections[i];
            if (c->state != CONN_READING || !c->needs_parse)
                continue;
            c->needs_parse = 0;
            HashMap *request;
            int error_status;
            int result = parse_request(c, &request, &error_status);
            if (result == 1)
            {
                c->state = CONN_HANDLING;
                server->current = c;
                server->next_connection = (i + 1) % count;
                free(fds);
                return request;
            }
            if (result < 0)
            {
                c->keep_alive = 0;
                c->is_head = 0;
                const char *reason = status_reason(error_status);
                if (!connection_queue_response(c, error_status, DEFAULT_CONTENT_TYPE, NULL, reason, strlen(reason)))
                {
                    server_drop_connection(server, i);
                    k--;
                    count--;
                }
            }
        }

        // 2. Close connections whose deadline has passed.
        double now = time_monotonic_seconds();
        double next_deadline = now + 1.0;
        for (int i = 0; i < server->connection_count; i++)
        {
            Connection *c = server->connections[i];
            if (c->state == CONN_HANDLING)
                continue;
            if (c->deadline <= now)
            {
                server_drop_connection(server, i);
                i--;
                continue;
            }
            if (c->deadline < next_deadline)
                next_deadline = c->deadline;
        }

        // 3. Wait for activity.
        int nfds = 0;
        int listening = server->connection_count < MAX_CONNECTIONS;
        if (listening)
        {
            fds[nfds].fd = server->listener;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        for (int i = 0; i < server->connection_count; i++)
        {
            Connection *c = server->connections[i];
            fds[nfds].fd = c->socket;
            fds[nfds].events = c->state == CONN_WRITING ? POLLOUT : (c->state == CONN_READING ? POLLIN : 0);
            fds[nfds].revents = 0;
            nfds++;
        }
        int timeout_ms = (int) ((next_deadline - now) * 1000.0) + 1;
        int ready = poll(fds, (unsigned long) nfds, timeout_ms);
        if (ready <= 0)
            continue; // Timeout (deadlines are handled above) or EINTR.

        // 4. Service connections. (Iterate the poll slots in step with the
        //    connection array; dropped connections are handled by index fix-up.)
        int base = listening ? 1 : 0;
        int conn_count_at_poll = server->connection_count;
        Connection **polled = malloc(sizeof(Connection *) * (size_t) (conn_count_at_poll + 1));
        memcpy(polled, server->connections, sizeof(Connection *) * (size_t) conn_count_at_poll);
        for (int p = 0; p < conn_count_at_poll; p++)
        {
            short revents = fds[base + p].revents;
            if (revents == 0)
                continue;
            Connection *c = polled[p];
            int index = -1;
            for (int i = 0; i < server->connection_count; i++)
            {
                if (server->connections[i] == c)
                {
                    index = i;
                    break;
                }
            }
            if (index < 0)
                continue;

            int keep = 1;
            if (c->state == CONN_READING && (revents & (POLLIN | POLLHUP | POLLERR)))
            {
                for (;;)
                {
                    if (c->in_capacity - c->in_length < 16384)
                    {
                        c->in_capacity = c->in_capacity == 0 ? 16384 : c->in_capacity * 2;
                        c->in = realloc(c->in, c->in_capacity);
                    }
                    int received = (int) recv(c->socket, c->in + c->in_length,
                                              (int) (c->in_capacity - c->in_length), 0);
                    if (received > 0)
                    {
                        c->in_length += (size_t) received;
                        c->needs_parse = 1;
                        if (c->deadline - time_monotonic_seconds() > REQUEST_TIMEOUT_SECONDS)
                            c->deadline = time_monotonic_seconds() + REQUEST_TIMEOUT_SECONDS;
                        if (c->in_length > MAX_HEADER_BYTES + MAX_BODY_BYTES + 1)
                            break; // parse_request() rejects it.
                        continue;
                    }
                    if (received < 0 && would_block())
                        break;
                    keep = 0; // Closed by the client, or an error.
                    break;
                }
                // A client that idled past keep-alive but sent data gets the full request timeout.
                if (keep && c->in_length > 0 && c->deadline < time_monotonic_seconds() + 0.5)
                    c->deadline = time_monotonic_seconds() + REQUEST_TIMEOUT_SECONDS;
            }
            else if (c->state == CONN_WRITING && (revents & (POLLOUT | POLLHUP | POLLERR)))
            {
                int flushed = connection_flush(c);
                if (flushed < 0)
                    keep = 0;
                else if (flushed == 1)
                    keep = connection_finish_response(c);
            }
            if (!keep)
                server_drop_connection(server, index);
        }
        free(polled);

        // 5. Accept new connections.
        if (listening && (fds[0].revents & POLLIN))
        {
            while (server->connection_count < MAX_CONNECTIONS)
            {
                socket_t client = accept(server->listener, NULL, NULL);
                if (client == INVALID_SOCKET_HANDLE)
                    break; // No more pending (EAGAIN) or a transient error.
                set_nonblocking(client);
#ifdef SO_NOSIGPIPE
                // macOS has no MSG_NOSIGNAL; disable SIGPIPE on the socket instead.
                int no_sigpipe = 1;
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
                server->connections[server->connection_count++] = connection_new(client);
            }
        }
    }
}

// --- Responses from Pith ---

/** @brief True if @p name is a valid HTTP header name (RFC 9110 token characters). */
static int valid_header_name(const char *name)
{
    if (*name == '\0')
        return 0;
    for (; *name; name++)
    {
        unsigned char c = (unsigned char) *name;
        if (!(isalnum(c) || strchr("!#$%&'*+-.^_`|~", c)))
            return 0;
    }
    return 1;
}

/**
 * @brief Formats a map of response headers as "Name: value\r\n" lines.
 *
 * Rejects names/values that could break the response (CR/LF in a value would
 * let a value inject extra headers), and headers this module manages itself.
 *
 * @param headers The Pith map (values must be strings).
 * @param content_type_out Set to the map's Content-Type value, if it has one.
 * @return A malloc'd string; the caller frees it.
 */
static char *format_headers(HashMap *headers, const char **content_type_out)
{
    MapEntry **entries = hashmap_entries_in_order(headers);
    size_t size = 1;
    const char *problem = NULL;
    const char *problem_key = NULL;
    for (int i = 0; i < headers->count && problem == NULL; i++)
    {
        MapEntry *entry = entries[i];
        problem_key = entry->key;
        if (!valid_header_name(entry->key))
            problem = "respond(): invalid header name '%s'.";
        else if (entry->value.type != VAL_STRING)
            problem = "respond(): header '%s' must be a string.";
        else if (memchr(entry->value.string->chars, '\r', (size_t) entry->value.string->length) ||
                 memchr(entry->value.string->chars, '\n', (size_t) entry->value.string->length) ||
                 memchr(entry->value.string->chars, '\0', (size_t) entry->value.string->length))
            problem = "respond(): header '%s' contains a line break.";
        else if (ascii_compare_ignore_case(entry->key, "Content-Length") == 0 ||
                 ascii_compare_ignore_case(entry->key, "Connection") == 0 ||
                 ascii_compare_ignore_case(entry->key, "Transfer-Encoding") == 0)
            problem = "respond(): header '%s' is set by the http module.";
        else
            size += strlen(entry->key) + (size_t) entry->value.string->length + 4;
    }
    if (problem != NULL)
    {
        char message[512];
        snprintf(message, sizeof(message), problem, problem_key);
        free(entries);
        report_error(get_exec_error_line(), "%s", message);
    }

    char *out = malloc(size);
    out[0] = '\0';
    char *end = out;
    for (int i = 0; i < headers->count; i++)
    {
        MapEntry *entry = entries[i];
        if (ascii_compare_ignore_case(entry->key, "Content-Type") == 0)
        {
            *content_type_out = entry->value.string->chars;
            continue;
        }
        end += sprintf(end, "%s: %s\r\n", entry->key, entry->value.string->chars);
    }
    free(entries);
    return out;
}

/**
 * @brief Sends a response for the server's current request.
 */
static void server_respond(HttpServer *server, int status, const char *content_type, const char *extra_headers,
                           const char *body, size_t body_length)
{
    Connection *c = server->current;
    server->current = NULL;
    if (!connection_queue_response(c, status, content_type, extra_headers, body, body_length))
    {
        for (int i = 0; i < server->connection_count; i++)
        {
            if (server->connections[i] == c)
            {
                server_drop_connection(server, i);
                break;
            }
        }
    }
}

/** @brief Answers a request Pith left unanswered with a 500. */
static void server_respond_unanswered(HttpServer *server)
{
    const char *message = "The server did not send a response.";
    server_respond(server, 500, DEFAULT_CONTENT_TYPE, NULL, message, strlen(message));
}

/**
 * @brief Looks up a server handle, reporting a Pith error if it is not open.
 */
static HttpServer *server_arg(Value v, const char *func_name)
{
    if (v.type != VAL_INT || v.int_val < 0 || v.int_val >= MAX_SERVERS || !servers[v.int_val].is_open)
        report_error(get_exec_error_line(), "%s(): first argument must be an open server handle from http.listen().",
                     func_name);
    return &servers[v.int_val];
}

// --- Routing ---

/**
 * @brief Splits a URL path into its segments ("/a/b/" -> "a", "b", "").
 *
 * @param decode Percent-decode each segment (request paths); patterns are kept as written.
 * @return A malloc'd array of malloc'd strings, or NULL if decoding failed.
 */
static char **split_path(const char *path, int *count_out, int decode)
{
    int capacity = 8, count = 0;
    char **segments = malloc(sizeof(char *) * (size_t) capacity);
    const char *p = path;
    if (*p == '/')
        p++;
    for (;;)
    {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t) (end - p) : strlen(p);
        char *segment = malloc(len + 1);
        size_t n = 0;
        for (size_t i = 0; i < len; i++)
        {
            if (decode && p[i] == '%')
            {
                int high = i + 2 < len ? hex_value(p[i + 1]) : -1;
                int low = high >= 0 ? hex_value(p[i + 2]) : -1;
                if (low < 0)
                {
                    free(segment);
                    for (int k = 0; k < count; k++)
                        free(segments[k]);
                    free(segments);
                    return NULL;
                }
                segment[n++] = (char) (high * 16 + low);
                i += 2;
            }
            else
                segment[n++] = p[i];
        }
        segment[n] = '\0';
        if (count == capacity)
        {
            capacity *= 2;
            segments = realloc(segments, sizeof(char *) * (size_t) capacity);
        }
        segments[count++] = segment;
        if (end == NULL)
            break;
        p = end + 1;
    }
    *count_out = count;
    return segments;
}

static void free_segments(char **segments, int count)
{
    for (int i = 0; i < count; i++)
        free(segments[i]);
    free(segments);
}

/**
 * @brief Matches decoded request path segments against a route's pattern.
 *
 * @param params_out If non-NULL and the route matches, receives the index in
 *        @p path where a trailing "*" (or a static mount's remainder) starts.
 * @return 1 if the path matches the pattern.
 */
static int route_matches(Route *route, char **path, int path_count, int *rest_out)
{
    int pattern_count = route->segment_count;
    int has_rest = route->static_dir != NULL ||
                   (pattern_count > 0 && strcmp(route->segments[pattern_count - 1], "*") == 0);
    int fixed = has_rest && route->static_dir == NULL ? pattern_count - 1 : pattern_count;
    if (has_rest ? path_count < fixed : path_count != fixed)
        return 0;
    for (int i = 0; i < fixed; i++)
    {
        const char *pattern = route->segments[i];
        if (pattern[0] == ':')
        {
            if (path[i][0] == '\0')
                return 0; // A parameter must not be empty.
        }
        else if (strcmp(pattern, path[i]) != 0)
            return 0;
    }
    if (rest_out)
        *rest_out = fixed;
    return 1;
}

/** @brief Content-Type for a file name, by extension. */
static const char *content_type_for(const char *file)
{
    static const char *types[][2] = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"}, {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"}, {".md", "text/markdown; charset=utf-8"},
        {".xml", "application/xml"}, {".svg", "image/svg+xml"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
        {".webp", "image/webp"}, {".ico", "image/x-icon"}, {".pdf", "application/pdf"},
        {".wasm", "application/wasm"}, {".woff", "font/woff"}, {".woff2", "font/woff2"},
        {".mp3", "audio/mpeg"}, {".mp4", "video/mp4"}, {".zip", "application/zip"},
    };
    const char *dot = strrchr(file, '.');
    if (dot != NULL)
    {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        {
            if (ascii_compare_ignore_case(dot, types[i][0]) == 0)
                return types[i][1];
        }
    }
    return "application/octet-stream";
}

/**
 * @brief Serves a file below a static mount's directory.
 *
 * Path segments are already percent-decoded; any segment that could escape the
 * directory ("..", ".", or one containing a separator) is refused with 404, so
 * nothing outside the mounted directory can be read.
 */
static void serve_static_file(HttpServer *server, Route *route, char **path, int path_count, int rest)
{
    size_t size = strlen(route->static_dir) + 16;
    for (int i = rest; i < path_count; i++)
    {
        const char *segment = path[i];
        if (strcmp(segment, "..") == 0 || strcmp(segment, ".") == 0 || strpbrk(segment, "/\\:") != NULL)
        {
            const char *message = "Not Found";
            server_respond(server, 404, DEFAULT_CONTENT_TYPE, NULL, message, strlen(message));
            return;
        }
        size += strlen(segment) + 1;
    }
    char *file = malloc(size + 16);
    strcpy(file, route->static_dir);
    for (int i = rest; i < path_count; i++)
    {
        strcat(file, "/");
        strcat(file, path[i]);
    }
    // "/assets/" (or "/assets") serves the directory's index.html.
    if (rest >= path_count || path[path_count - 1][0] == '\0')
        strcat(file, rest >= path_count ? "/index.html" : "index.html");

    long length = 0;
    char *bytes = read_file_bytes(file, &length);
    if (bytes == NULL || length > MAX_STATIC_FILE_BYTES)
    {
        const char *message = "Not Found";
        server_respond(server, 404, DEFAULT_CONTENT_TYPE, NULL, message, strlen(message));
    }
    else
        server_respond(server, 200, content_type_for(file), NULL, bytes, (size_t) length);
    free(bytes);
    free(file);
}

/**
 * @brief Finds and runs the route for @p request (serve() loop body).
 */
static void dispatch(HttpServer *server, HashMap *request)
{
    const char *method = AS_CSTRING(hashmap_get(request, "method"));
    const char *raw_path = AS_CSTRING(hashmap_get(request, "path"));
    int path_count;
    char **path = split_path(raw_path, &path_count, 1);
    if (path == NULL)
    {
        const char *message = "Bad Request";
        server_respond(server, 400, DEFAULT_CONTENT_TYPE, NULL, message, strlen(message));
        return;
    }

    int path_matched = 0;
    char allowed[256] = "";
    for (int r = 0; r < server->route_count; r++)
    {
        Route *route = &server->routes[r];
        int rest;
        if (!route_matches(route, path, path_count, &rest))
            continue;
        path_matched = 1;
        int is_head = strcmp(method, "HEAD") == 0;
        int method_ok = strcmp(route->method, "*") == 0 || strcmp(route->method, method) == 0 ||
                        (is_head && strcmp(route->method, "GET") == 0);
        if (!method_ok)
        {
            if (strlen(allowed) + strlen(route->method) + 3 < sizeof(allowed) && strstr(allowed, route->method) == NULL)
            {
                if (allowed[0])
                    strcat(allowed, ", ");
                strcat(allowed, route->method);
            }
            continue;
        }

        if (route->static_dir != NULL)
        {
            serve_static_file(server, route, path, path_count, rest);
            free_segments(path, path_count);
            return;
        }

        // Path parameters: ":id" segments, and "*" for the rest of the path.
        for (int i = 0; i < route->segment_count; i++)
        {
            char key[256];
            const char *segment = route->segments[i];
            if (segment[0] == ':')
            {
                snprintf(key, sizeof(key), "%s%s", PARAM_KEY_PREFIX, segment + 1);
                map_set_string(request, key, path[i]);
            }
            else if (strcmp(segment, "*") == 0)
            {
                size_t rest_size = 1;
                for (int k = i; k < path_count; k++)
                    rest_size += strlen(path[k]) + 1;
                char *joined = malloc(rest_size);
                joined[0] = '\0';
                for (int k = i; k < path_count; k++)
                {
                    if (k > i)
                        strcat(joined, "/");
                    strcat(joined, path[k]);
                }
                snprintf(key, sizeof(key), "%s*", PARAM_KEY_PREFIX);
                map_set_string(request, key, joined);
                free(joined);
            }
        }
        free_segments(path, path_count);

        Value arg;
        arg.type = VAL_HASHMAP;
        arg.hashmap = request;
        Value result;
        char error[1024];
        if (interpreter_call_protected(route->handler, &arg, 1, &result, error, sizeof(error)) != 0)
        {
            fprintf(stderr, "[http] %s %s: %s\n", method, raw_path, error);
            if (server->current != NULL)
            {
                const char *message = "Internal Server Error";
                server_respond(server, 500, DEFAULT_CONTENT_TYPE, NULL, message, strlen(message));
            }
        }
        else if (server->current != NULL)
        {
            fprintf(stderr, "[http] %s %s: handler returned without calling http.respond()\n", method, raw_path);
            server_respond_unanswered(server);
        }
        return;
    }
    free_segments(path, path_count);

    if (path_matched)
    {
        char headers[300];
        snprintf(headers, sizeof(headers), "Allow: %s\r\n", allowed);
        const char *message = "Method Not Allowed";
        server_respond(server, 405, DEFAULT_CONTENT_TYPE, headers, message, strlen(message));
    }
    else
    {
        const char *message = "Not Found";
        server_respond(server, 404, DEFAULT_CONTENT_TYPE, NULL, message, strlen(message));
    }
}

/**
 * @brief Adds a route (handler) or static mount (directory) to a server.
 */
static void server_add_route(HttpServer *server, const char *method, const char *pattern, Value handler,
                             const char *static_dir)
{
    server->routes = realloc(server->routes, sizeof(Route) * (size_t) (server->route_count + 1));
    Route *route = &server->routes[server->route_count++];
    route->method = strdup(method);
    route->segments = split_path(pattern, &route->segment_count, 0);
    // A static mount at "/" has no fixed segments (a plain "/" route keeps its
    // single empty segment, which matches exactly the path "/").
    if (static_dir && route->segment_count == 1 && route->segments[0][0] == '\0')
    {
        free(route->segments[0]);
        route->segment_count = 0;
    }
    route->handler = handler;
    route->static_dir = static_dir ? strdup(static_dir) : NULL;
}

// --- Native functions ---

// http.listen(port [, host]) -> int server handle.
static Value native_http_listen(int arg_count, Value *args)
{
    if (arg_count < 1 || arg_count > 2 || args[0].type != VAL_INT)
        report_error(get_exec_error_line(), "listen() takes a port (int) and an optional host (string).");
    if (arg_count == 2 && args[1].type != VAL_STRING)
        report_error(get_exec_error_line(), "listen() host must be a string, e.g. \"0.0.0.0\".");
    if (args[0].int_val < 0 || args[0].int_val > 65535)
        report_error(get_exec_error_line(), "listen(): port must be between 0 and 65535.");
    int port = (int) args[0].int_val;
    const char *host = arg_count == 2 ? AS_CSTRING(args[1]) : "127.0.0.1";

    int handle = -1;
    for (int i = 0; i < MAX_SERVERS; i++)
    {
        if (!servers[i].is_open)
        {
            handle = i;
            break;
        }
    }
    if (handle < 0)
        report_error(get_exec_error_line(), "listen(): too many open servers (max %d).", MAX_SERVERS);

    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    struct addrinfo hints;
    struct addrinfo *address = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host, port_text, &hints, &address) != 0 || address == NULL)
        report_error(get_exec_error_line(), "listen(): could not resolve host '%s'.", host);

    socket_t listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (listener == INVALID_SOCKET_HANDLE)
    {
        freeaddrinfo(address);
        report_error(get_exec_error_line(), "listen(): could not create a socket.");
    }

    // Let a restarted program rebind the port immediately instead of waiting
    // for the old connections' TIME_WAIT to expire.
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));

    int bound = bind(listener, address->ai_addr, (int) address->ai_addrlen) == 0;
    freeaddrinfo(address);
    if (!bound || listen(listener, 128) != 0)
    {
        close_socket(listener);
        report_error(get_exec_error_line(), "listen(): could not listen on %s:%d (is the port already in use?).",
                     host, port);
    }
    set_nonblocking(listener);

    HttpServer *server = &servers[handle];
    memset(server, 0, sizeof(*server));
    server->is_open = 1;
    server->listener = listener;
    server->connections = malloc(sizeof(Connection *) * MAX_CONNECTIONS);

    Value v;
    v.type = VAL_INT;
    v.int_val = handle;
    return v;
}

// http.accept(server) -> map<string, string>. Waits for the next complete request.
static Value native_http_accept(int arg_count, Value *args)
{
    if (arg_count != 1)
        report_error(get_exec_error_line(), "accept() takes exactly one argument (the server handle).");
    HttpServer *server = server_arg(args[0], "accept");
    if (server->current != NULL)
        server_respond_unanswered(server); // Never leave a client hanging.

    Value v;
    v.type = VAL_HASHMAP;
    v.hashmap = next_request(server);
    return v;
}

// http.header(req, name) -> string. Case-insensitive; "" when absent.
static Value native_http_header(int arg_count, Value *args)
{
    if (arg_count != 2 || args[0].type != VAL_HASHMAP || args[1].type != VAL_STRING)
        report_error(get_exec_error_line(), "header() takes a request map and a header name (string).");

    char key[256];
    snprintf(key, sizeof(key), "%s%s", HEADER_KEY_PREFIX, AS_CSTRING(args[1]));
    for (char *c = key; *c; c++)
        *c = (char) tolower((unsigned char) *c);

    // Strings are immutable, so the stored header can be returned as-is.
    Value found = hashmap_get(args[0].hashmap, key);
    return found.type == VAL_STRING ? found : cstring_value("");
}

// http.param(req, name) -> string. A ":name" (or "*") path parameter of the matched route.
static Value native_http_param(int arg_count, Value *args)
{
    if (arg_count != 2 || args[0].type != VAL_HASHMAP || args[1].type != VAL_STRING)
        report_error(get_exec_error_line(), "param() takes a request map and a parameter name (string).");
    char key[256];
    snprintf(key, sizeof(key), "%s%s", PARAM_KEY_PREFIX, AS_CSTRING(args[1]));
    if (!hashmap_has(args[0].hashmap, key))
        report_error(get_exec_error_line(), "param(): the matched route has no parameter '%s'.", AS_CSTRING(args[1]));
    return hashmap_get(args[0].hashmap, key);
}

// http.respond(server, status, body [, content_type | headers]) -> void.
static Value native_http_respond(int arg_count, Value *args)
{
    if (arg_count < 3 || arg_count > 4)
        report_error(get_exec_error_line(), "respond() takes (server, status, body [, content_type or headers]).");
    HttpServer *server = server_arg(args[0], "respond");
    if (args[1].type != VAL_INT || args[1].int_val < 100 || args[1].int_val > 999)
        report_error(get_exec_error_line(), "respond(): status must be an integer such as 200 or 404.");
    if (args[2].type != VAL_STRING)
        report_error(get_exec_error_line(), "respond(): body must be a string.");
    if (arg_count == 4 && args[3].type != VAL_STRING && args[3].type != VAL_HASHMAP)
        report_error(get_exec_error_line(),
                     "respond(): the 4th argument must be a content type (string) or a map of headers.");
    if (server->current == NULL)
        report_error(get_exec_error_line(), "respond(): no pending request (already answered, or accept() not called).");

    const char *content_type = DEFAULT_CONTENT_TYPE;
    char *extra_headers = NULL;
    if (arg_count == 4 && args[3].type == VAL_STRING)
        content_type = AS_CSTRING(args[3]);
    else if (arg_count == 4)
        extra_headers = format_headers(args[3].hashmap, &content_type);

    server_respond(server, (int) args[1].int_val, content_type, extra_headers, AS_CSTRING(args[2]),
                   (size_t) STRING_LENGTH(args[2])); // Binary-safe.
    free(extra_headers);
    return (Value){VAL_VOID};
}

// http.route(server, method, pattern, handler) -> void.
static Value native_http_route(int arg_count, Value *args)
{
    if (arg_count != 4 || args[1].type != VAL_STRING || args[2].type != VAL_STRING)
        report_error(get_exec_error_line(), "route() takes (server, method, path_pattern, handler).");
    HttpServer *server = server_arg(args[0], "route");
    if (args[3].type != VAL_FUNC && args[3].type != VAL_BOUND_METHOD && args[3].type != VAL_NATIVE_FN)
        report_error(get_exec_error_line(), "route(): the handler must be a function, got %s.",
                     type_describe_value(args[3]));
    if (AS_CSTRING(args[2])[0] != '/')
        report_error(get_exec_error_line(), "route(): the path pattern must start with '/'.");

    char method[32];
    snprintf(method, sizeof(method), "%s", AS_CSTRING(args[1]));
    for (char *c = method; *c; c++)
        *c = (char) toupper((unsigned char) *c);

    // Keep the handler alive for the rest of the run.
    if (registered_handlers.type != VAL_LIST)
    {
        registered_handlers.list = (List *) allocate_obj(sizeof(List), OBJ_LIST);
        registered_handlers.type = VAL_LIST;
    }
    list_add(registered_handlers.list, args[3]);
    server_add_route(server, method, AS_CSTRING(args[2]), args[3], NULL);
    return (Value){VAL_VOID};
}

// http.static(server, url_prefix, directory) -> void. Serves files below directory.
static Value native_http_static(int arg_count, Value *args)
{
    if (arg_count != 3 || args[1].type != VAL_STRING || args[2].type != VAL_STRING)
        report_error(get_exec_error_line(), "static() takes (server, url_prefix, directory).");
    HttpServer *server = server_arg(args[0], "static");
    if (AS_CSTRING(args[1])[0] != '/')
        report_error(get_exec_error_line(), "static(): the URL prefix must start with '/'.");
    server_add_route(server, "GET", AS_CSTRING(args[1]), (Value){VAL_VOID}, AS_CSTRING(args[2]));
    return (Value){VAL_VOID};
}

// http.serve(server) -> void. Runs the server: dispatches every request to its route.
static Value native_http_serve(int arg_count, Value *args)
{
    if (arg_count != 1)
        report_error(get_exec_error_line(), "serve() takes exactly one argument (the server handle).");
    HttpServer *server = server_arg(args[0], "serve");
    if (server->route_count == 0)
        report_error(get_exec_error_line(), "serve(): no routes; add some with http.route() or http.static().");
    if (server->current != NULL)
        server_respond_unanswered(server);

    server->stop_requested = 0;
    // is_open: a handler may also have called http.close() on this server.
    while (server->is_open && !server->stop_requested)
    {
        Value request;
        request.type = VAL_HASHMAP;
        request.hashmap = next_request(server);
        gc_push_value(request); // Only C holds it while the handler runs.
        dispatch(server, request.hashmap);
        gc_pop_root();
    }
    return (Value){VAL_VOID};
}

// http.stop(server) -> void. Makes serve() return after the current request.
static Value native_http_stop(int arg_count, Value *args)
{
    if (arg_count != 1)
        report_error(get_exec_error_line(), "stop() takes exactly one argument (the server handle).");
    server_arg(args[0], "stop")->stop_requested = 1;
    return (Value){VAL_VOID};
}

// http.close(server) -> void.
static Value native_http_close(int arg_count, Value *args)
{
    if (arg_count != 1)
        report_error(get_exec_error_line(), "close() takes exactly one argument (the server handle).");
    HttpServer *server = server_arg(args[0], "close");
    for (int i = 0; i < server->connection_count; i++)
    {
        connection_flush(server->connections[i]); // Best effort for a just-queued response.
        connection_free(server->connections[i]);
    }
    free(server->connections);
    for (int r = 0; r < server->route_count; r++)
    {
        free(server->routes[r].method);
        free_segments(server->routes[r].segments, server->routes[r].segment_count);
        free(server->routes[r].static_dir);
    }
    free(server->routes);
    close_socket(server->listener);
    memset(server, 0, sizeof(*server));
    return (Value){VAL_VOID};
}

// --- URL helpers ---

/**
 * @brief Percent-decodes @p text ("%41" -> "A"); if @p plus_is_space, '+' becomes ' '
 *        (the form/query-string convention).
 */
static Value url_decode_value(ObjString *text, int plus_is_space)
{
    char *out = malloc((size_t) text->length + 1);
    int n = 0;
    for (int i = 0; i < text->length; i++)
    {
        char c = text->chars[i];
        if (c == '%')
        {
            int high = i + 2 < text->length ? hex_value(text->chars[i + 1]) : -1;
            int low = high >= 0 ? hex_value(text->chars[i + 2]) : -1;
            if (low < 0)
            {
                free(out);
                report_error(get_exec_error_line(), "url_decode(): invalid percent-encoding in '%s'.", text->chars);
            }
            out[n++] = (char) (high * 16 + low);
            i += 2;
        }
        else
            out[n++] = (plus_is_space && c == '+') ? ' ' : c;
    }
    Value v = string_value(out, n);
    free(out);
    return v;
}

// http.url_decode(text) -> string. Decodes %XX escapes and '+' (as a space).
static Value native_http_url_decode(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "url_decode() takes exactly one string argument.");
    return url_decode_value(args[0].string, 1);
}

// http.url_encode(text) -> string. Percent-encodes everything except A-Z a-z 0-9 - _ . ~
static Value native_http_url_encode(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "url_encode() takes exactly one string argument.");
    ObjString *text = args[0].string;
    char *out = malloc((size_t) text->length * 3 + 1);
    int n = 0;
    for (int i = 0; i < text->length; i++)
    {
        unsigned char c = (unsigned char) text->chars[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out[n++] = (char) c;
        else
            n += sprintf(out + n, "%%%02X", c);
    }
    Value v = string_value(out, n);
    free(out);
    return v;
}

// http.parse_query(text) -> map<string, string>. Parses "a=1&b=two%20words" (also
// form bodies). A key without '=' maps to ""; a repeated key keeps its last value.
static Value native_http_parse_query(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "parse_query() takes exactly one string argument.");
    ObjString *text = args[0].string; // Rooted: it is an argument.

    HashMap *result = hashmap_create(VAL_STRING, VAL_VOID);
    result->value_spec = type_from_name("string");
    gc_push_root((ObjHeader *) result); // Each decoded string below allocates.

    int start = 0;
    while (start <= text->length)
    {
        int end = start;
        while (end < text->length && text->chars[end] != '&')
            end++;
        if (end > start)
        {
            int eq = start;
            while (eq < end && text->chars[eq] != '=')
                eq++;
            ObjString *raw_key = string_new(text->chars + start, eq - start);
            gc_push_root((ObjHeader *) raw_key);
            Value key = url_decode_value(raw_key, 1);
            gc_push_value(key);
            ObjString *raw_value = eq < end ? string_new(text->chars + eq + 1, end - eq - 1) : string_new("", 0);
            gc_push_root((ObjHeader *) raw_value);
            Value value = url_decode_value(raw_value, 1);
            hashmap_set(result, AS_CSTRING(key), value, 0);
            gc_pop_root();
            gc_pop_root();
            gc_pop_root();
        }
        start = end + 1;
    }

    gc_pop_root();
    Value v;
    v.type = VAL_HASHMAP;
    v.hashmap = result;
    return v;
}

// --- Registration ---

void http_register_module(HashMap *modules)
{
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    gc_register_value_root(&registered_handlers);

    HashMap *http_funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(http_funcs, "listen", native_http_listen);
    register_native_method(http_funcs, "accept", native_http_accept);
    register_native_method(http_funcs, "header", native_http_header);
    register_native_method(http_funcs, "param", native_http_param);
    register_native_method(http_funcs, "respond", native_http_respond);
    register_native_method(http_funcs, "route", native_http_route);
    register_native_method(http_funcs, "static", native_http_static);
    register_native_method(http_funcs, "serve", native_http_serve);
    register_native_method(http_funcs, "stop", native_http_stop);
    register_native_method(http_funcs, "close", native_http_close);
    register_native_method(http_funcs, "url_decode", native_http_url_decode);
    register_native_method(http_funcs, "url_encode", native_http_url_encode);
    register_native_method(http_funcs, "parse_query", native_http_parse_query);

    Value http_module_val;
    http_module_val.type = VAL_HASHMAP;
    http_module_val.hashmap = http_funcs;
    hashmap_set(modules, "http", http_module_val, 0);
}
