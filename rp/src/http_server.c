#include "include/http_server.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#define HTTP_RESPONSE_BUF_BYTES 1024
#define HTTP_PATH_BUF_BYTES 256
#define HTTP_QUERY_BUF_BYTES 256

// Idle-connection sweeper: tcp_poll fires every (4 × tcp_poll_interval) /
// 2 seconds. With interval 8, that's 16 seconds per tick; we close on
// the first tick, so a connection that never sends a complete request
// is closed in ~16 s.
#define HTTP_POLL_INTERVAL 8

typedef enum {
  HC_FREE = 0,
  HC_READ_HEADERS,
  HC_WRITE_RESPONSE,
  HC_DRAINING,
} hc_state_t;

typedef enum {
  HM_GET,
  HM_HEAD,
  HM_POST,
  HM_PUT,
  HM_DELETE,
  HM_UNKNOWN,
} hc_method_t;

typedef struct http_conn {
  struct tcp_pcb *pcb;
  hc_state_t state;

  // Header parser
  char hdr[HTTP_HEADER_BUF_BYTES];
  size_t hdr_len;

  // Parsed request
  hc_method_t method;
  bool is_head;  // True if HEAD; route handler treats as GET, body suppressed.
  char path[HTTP_PATH_BUF_BYTES];
  char query[HTTP_QUERY_BUF_BYTES];
  bool has_host;
  bool has_chunked_te;
  size_t content_length;

  // Response
  char resp[HTTP_RESPONSE_BUF_BYTES];
  size_t resp_len;
  size_t resp_sent;
} http_conn_t;

static http_conn_t g_conns[HTTP_SERVER_MAX_CONNECTIONS];
static struct tcp_pcb *g_listen_pcb = NULL;
static absolute_time_t g_boot_time;

static http_conn_t *conn_alloc(void);
static void conn_free(http_conn_t *c);
static void conn_close(http_conn_t *c);

static void parse_and_dispatch(http_conn_t *c);
static void route(http_conn_t *c);
static void send_buffered(http_conn_t *c);

static void write_response(http_conn_t *c, int status, const char *reason,
                           const char *content_type, const char *body,
                           size_t body_len);
static void write_error(http_conn_t *c, int status, const char *reason,
                        const char *code_symbol, const char *message);
static void write_405(http_conn_t *c, const char *allow);

static err_t srv_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t srv_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                         err_t err);
static err_t srv_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len);
static err_t srv_poll_cb(void *arg, struct tcp_pcb *pcb);
static void srv_err_cb(void *arg, err_t err);

// --- Public API ---

void http_server_init(void) {
  if (g_listen_pcb != NULL) {
    return;  // already running
  }

  g_boot_time = get_absolute_time();

  struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
  if (pcb == NULL) {
    DPRINTF("http_server: tcp_new failed\n");
    return;
  }
  err_t err = tcp_bind(pcb, IP_ANY_TYPE, HTTP_SERVER_PORT);
  if (err != ERR_OK) {
    DPRINTF("http_server: tcp_bind failed: %d\n", err);
    tcp_close(pcb);
    return;
  }
  struct tcp_pcb *listen_pcb = tcp_listen_with_backlog(pcb, 4);
  if (listen_pcb == NULL) {
    DPRINTF("http_server: tcp_listen failed\n");
    return;
  }
  tcp_accept(listen_pcb, srv_accept_cb);
  g_listen_pcb = listen_pcb;
  DPRINTF("http_server: listening on :%d\n", HTTP_SERVER_PORT);
}

void http_server_deinit(void) {
  if (g_listen_pcb != NULL) {
    tcp_close(g_listen_pcb);
    g_listen_pcb = NULL;
  }
  for (int i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
    if (g_conns[i].state != HC_FREE) {
      conn_close(&g_conns[i]);
    }
  }
}

// --- Connection lifecycle ---

static http_conn_t *conn_alloc(void) {
  for (int i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
    if (g_conns[i].state == HC_FREE) {
      memset(&g_conns[i], 0, sizeof(g_conns[i]));
      g_conns[i].state = HC_READ_HEADERS;
      return &g_conns[i];
    }
  }
  return NULL;
}

static void conn_free(http_conn_t *c) {
  c->pcb = NULL;
  c->state = HC_FREE;
}

static void conn_close(http_conn_t *c) {
  if (c->pcb != NULL) {
    struct tcp_pcb *pcb = c->pcb;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_err(pcb, NULL);
    err_t err = tcp_close(pcb);
    if (err != ERR_OK) {
      tcp_abort(pcb);
    }
  }
  conn_free(c);
}

// --- TCP callbacks ---

static err_t srv_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
  (void)arg;
  if (err != ERR_OK || newpcb == NULL) {
    return ERR_VAL;
  }
  http_conn_t *c = conn_alloc();
  if (c == NULL) {
    DPRINTF("http_server: connection pool full, rejecting\n");
    tcp_abort(newpcb);
    return ERR_ABRT;
  }
  c->pcb = newpcb;
  tcp_arg(newpcb, c);
  tcp_recv(newpcb, srv_recv_cb);
  tcp_sent(newpcb, srv_sent_cb);
  tcp_poll(newpcb, srv_poll_cb, HTTP_POLL_INTERVAL);
  tcp_err(newpcb, srv_err_cb);
  return ERR_OK;
}

static err_t srv_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                         err_t err) {
  http_conn_t *c = (http_conn_t *)arg;
  if (c == NULL) {
    if (p != NULL) {
      pbuf_free(p);
    }
    return ERR_VAL;
  }
  if (err != ERR_OK || p == NULL) {
    // Peer closed or error; either way the connection is done.
    if (p != NULL) {
      pbuf_free(p);
    }
    conn_close(c);
    return ERR_OK;
  }

  if (c->state == HC_READ_HEADERS) {
    size_t to_copy = p->tot_len;
    size_t remaining =
        (c->hdr_len < HTTP_HEADER_BUF_BYTES - 1)
            ? HTTP_HEADER_BUF_BYTES - 1 - c->hdr_len
            : 0;
    if (to_copy > remaining) {
      to_copy = remaining;
    }
    if (to_copy > 0) {
      pbuf_copy_partial(p, c->hdr + c->hdr_len, (u16_t)to_copy, 0);
      c->hdr_len += to_copy;
      c->hdr[c->hdr_len] = '\0';
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (strstr(c->hdr, "\r\n\r\n") != NULL) {
      parse_and_dispatch(c);
    } else if (c->hdr_len >= HTTP_HEADER_BUF_BYTES - 1) {
      write_error(c, 400, "Bad Request", "bad_request", "Headers too large");
    }
  } else {
    // We don't expect more bytes after headers in S1 (no body routes
    // yet). Drain and ignore.
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
  }
  return ERR_OK;
}

static err_t srv_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
  (void)pcb;
  http_conn_t *c = (http_conn_t *)arg;
  if (c == NULL) {
    return ERR_OK;
  }
  c->resp_sent += len;
  if (c->state == HC_WRITE_RESPONSE && c->resp_sent >= c->resp_len) {
    c->state = HC_DRAINING;
    conn_close(c);
  }
  return ERR_OK;
}

static err_t srv_poll_cb(void *arg, struct tcp_pcb *pcb) {
  (void)pcb;
  http_conn_t *c = (http_conn_t *)arg;
  if (c != NULL) {
    DPRINTF("http_server: idle timeout, closing connection\n");
    conn_close(c);
  }
  return ERR_OK;
}

static void srv_err_cb(void *arg, err_t err) {
  (void)err;
  http_conn_t *c = (http_conn_t *)arg;
  if (c != NULL) {
    // pcb is already gone per lwIP contract; just release the slot.
    c->pcb = NULL;
    conn_free(c);
  }
}

// --- Request parsing ---

static int strncasecmp_n(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') {
      ca = (char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (char)(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return ca - cb;
    }
    if (ca == '\0') {
      return 0;
    }
  }
  return 0;
}

static hc_method_t parse_method(const char *s, size_t n) {
  if (n == 3 && memcmp(s, "GET", 3) == 0) return HM_GET;
  if (n == 4 && memcmp(s, "HEAD", 4) == 0) return HM_HEAD;
  if (n == 4 && memcmp(s, "POST", 4) == 0) return HM_POST;
  if (n == 3 && memcmp(s, "PUT", 3) == 0) return HM_PUT;
  if (n == 6 && memcmp(s, "DELETE", 6) == 0) return HM_DELETE;
  return HM_UNKNOWN;
}

static void parse_and_dispatch(http_conn_t *c) {
  // Request line
  char *eol = strstr(c->hdr, "\r\n");
  if (eol == NULL) {
    write_error(c, 400, "Bad Request", "bad_request", "Malformed request line");
    return;
  }
  *eol = '\0';
  char *line = c->hdr;

  char *sp1 = strchr(line, ' ');
  if (sp1 == NULL) {
    write_error(c, 400, "Bad Request", "bad_request", "Malformed request line");
    return;
  }
  *sp1 = '\0';
  c->method = parse_method(line, (size_t)(sp1 - line));

  char *uri = sp1 + 1;
  char *sp2 = strchr(uri, ' ');
  if (sp2 == NULL) {
    write_error(c, 400, "Bad Request", "bad_request", "Malformed request line");
    return;
  }
  *sp2 = '\0';

  const char *ver = sp2 + 1;
  if (strcmp(ver, "HTTP/1.1") != 0) {
    write_error(c, 400, "Bad Request", "bad_request",
                "Only HTTP/1.1 supported");
    return;
  }

  char *qmark = strchr(uri, '?');
  if (qmark != NULL) {
    *qmark = '\0';
    snprintf(c->query, sizeof(c->query), "%s", qmark + 1);
  }
  snprintf(c->path, sizeof(c->path), "%s", uri);

  // Headers
  char *p = eol + 2;  // past CRLF
  while (1) {
    char *next_eol = strstr(p, "\r\n");
    if (next_eol == NULL) {
      break;
    }
    if (next_eol == p) {
      break;  // empty line — end of headers
    }
    *next_eol = '\0';

    char *colon = strchr(p, ':');
    if (colon != NULL) {
      *colon = '\0';
      const char *name = p;
      const char *value = colon + 1;
      while (*value == ' ' || *value == '\t') {
        value++;
      }

      if (strncasecmp_n(name, "Host", 5) == 0) {
        c->has_host = true;
      } else if (strncasecmp_n(name, "Transfer-Encoding", 18) == 0) {
        if (strncasecmp_n(value, "chunked", 8) == 0) {
          c->has_chunked_te = true;
        }
      } else if (strncasecmp_n(name, "Content-Length", 15) == 0) {
        c->content_length = (size_t)atoi(value);
      }
    }

    p = next_eol + 2;
  }

  // Cross-cutting validation
  if (!c->has_host) {
    write_error(c, 400, "Bad Request", "bad_request", "Missing Host header");
    return;
  }
  if (c->has_chunked_te) {
    write_error(c, 411, "Length Required", "length_required",
                "chunked transfer-encoding not supported; send Content-Length");
    return;
  }
  if (c->method == HM_UNKNOWN) {
    write_error(c, 405, "Method Not Allowed", "method_not_allowed",
                "Unknown HTTP method");
    return;
  }

  c->is_head = (c->method == HM_HEAD);
  route(c);
}

static void route(http_conn_t *c) {
  // S1: only one route. Anything else with a known path → 405; unknown
  // path → 404. The full route table arrives in S2.

  if (strcmp(c->path, "/api/v1/ping") == 0) {
    if (c->method != HM_GET && c->method != HM_HEAD) {
      write_405(c, "GET, HEAD");
      return;
    }
    uint64_t uptime_us =
        (uint64_t)absolute_time_diff_us(g_boot_time, get_absolute_time());
    uint64_t uptime_s = uptime_us / 1000000ULL;
    char body[160];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"version\":\"%s\",\"uptime_s\":%llu}\n",
                     RELEASE_VERSION, (unsigned long long)uptime_s);
    if (n < 0) {
      n = 0;
    }
    write_response(c, 200, "OK", "application/json", body, (size_t)n);
    return;
  }

  write_error(c, 404, "Not Found", "not_found", "Route not found");
}

// --- Response writers ---

static void send_buffered(http_conn_t *c) {
  c->state = HC_WRITE_RESPONSE;
  c->resp_sent = 0;
  err_t err = tcp_write(c->pcb, c->resp, (u16_t)c->resp_len,
                        TCP_WRITE_FLAG_COPY);
  if (err == ERR_OK) {
    tcp_output(c->pcb);
  } else {
    DPRINTF("http_server: tcp_write failed: %d\n", err);
    conn_close(c);
  }
}

static void write_response(http_conn_t *c, int status, const char *reason,
                           const char *content_type, const char *body,
                           size_t body_len) {
  char *out = c->resp;
  size_t cap = sizeof(c->resp);
  int n = snprintf(out, cap,
                   "HTTP/1.1 %d %s\r\n"
                   "Server: md-devops/%s\r\n"
                   "Connection: close\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "\r\n",
                   status, reason, RELEASE_VERSION, content_type, body_len);
  if (n < 0 || (size_t)n >= cap) {
    DPRINTF("http_server: response headers too large for buffer\n");
    conn_close(c);
    return;
  }
  if (!c->is_head && body != NULL && body_len > 0) {
    if ((size_t)n + body_len >= cap) {
      DPRINTF("http_server: response body too large for static buffer\n");
      conn_close(c);
      return;
    }
    memcpy(out + n, body, body_len);
    n += (int)body_len;
  }
  c->resp_len = (size_t)n;
  send_buffered(c);
}

static void write_error(http_conn_t *c, int status, const char *reason,
                        const char *code_symbol, const char *message) {
  char body[256];
  int n = snprintf(body, sizeof(body),
                   "{\"ok\":false,\"code\":\"%s\",\"message\":\"%s\"}\n",
                   code_symbol, message);
  if (n < 0) {
    n = 0;
  }
  write_response(c, status, reason, "application/json", body, (size_t)n);
}

static void write_405(http_conn_t *c, const char *allow) {
  // 405 needs an Allow header in addition to the standard set.
  char body[160];
  int bn = snprintf(body, sizeof(body),
                    "{\"ok\":false,\"code\":\"method_not_allowed\","
                    "\"message\":\"Method not allowed; see Allow header\"}\n");
  if (bn < 0) {
    bn = 0;
  }
  char *out = c->resp;
  size_t cap = sizeof(c->resp);
  int n = snprintf(out, cap,
                   "HTTP/1.1 405 Method Not Allowed\r\n"
                   "Server: md-devops/%s\r\n"
                   "Connection: close\r\n"
                   "Allow: %s\r\n"
                   "Content-Type: application/json\r\n"
                   "Content-Length: %d\r\n"
                   "\r\n",
                   RELEASE_VERSION, allow, bn);
  if (n < 0 || (size_t)n >= cap) {
    conn_close(c);
    return;
  }
  if (!c->is_head) {
    if ((size_t)n + (size_t)bn >= cap) {
      conn_close(c);
      return;
    }
    memcpy(out + n, body, (size_t)bn);
    n += bn;
  }
  c->resp_len = (size_t)n;
  send_buffered(c);
}
