#include "include/http_server.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "debug.h"
#include "ff.h"
#include "gconfig.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "settings.h"

// Per-conn response buffer. Sized to comfortably hold a full status
// line + headers + the largest non-streaming body we emit. RAM is
// extremely tight on this board: the cartridge ROM mirror sits at
// the top of the same SRAM bank as the heap (see memmap_rp.ld), so
// any extra BSS shrinks the heap and pushes runtime allocations
// toward the ROM mirror — corrupting it. Kept small at the cost of
// truncating large JSON responses; chunked streaming for big bodies
// arrives in S5.
#define HTTP_RESPONSE_BUF_BYTES 1024
#define HTTP_PATH_BUF_BYTES 192
#define HTTP_QUERY_BUF_BYTES 192

// Listing cap. Spec says 1000, but the response buffer is the real
// constraint — at ~80 bytes per entry the 1 KB resp lets us fit
// roughly 8-10 entries before truncated:true.
#define HTTP_LISTING_MAX_ENTRIES 1000

// FatFs absolute-path buffer. Smaller than FF_MAX_LFN (255) because
// our jailed root + normalised <rel> can't reach that limit anyway.
#define HTTP_FAT_PATH_BUF_BYTES 192

// Per-conn request body buffer. Tiny JSON bodies only (rename takes
// {"to":"..."} which is well under 200 bytes). S6 uploads stream
// straight into FatFs without buffering here.
#define HTTP_REQUEST_BODY_BUF_BYTES 256

// Idle-connection sweeper: tcp_poll fires every (4 × tcp_poll_interval) /
// 2 seconds. With interval 8, that's 16 seconds per tick; we close on
// the first tick, so a connection that never sends a complete request
// is closed in ~16 s.
#define HTTP_POLL_INTERVAL 8

typedef enum {
  HC_FREE = 0,
  HC_READ_HEADERS,
  HC_READ_BODY,
  HC_WRITE_RESPONSE,
  HC_STREAM_LISTING,
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
  bool content_type_json;  // request Content-Type starts with application/json
  size_t content_length;

  // Request body (POST/PUT for non-streaming routes only).
  char body[HTTP_REQUEST_BODY_BUF_BYTES];
  size_t body_received;

  // Response
  char resp[HTTP_RESPONSE_BUF_BYTES];
  size_t resp_len;
  size_t resp_sent;

  // Streaming directory listing state. Active while state ==
  // HC_STREAM_LISTING; the dir handle outlives a single TCP recv/sent
  // callback and is released by conn_close().
  DIR stream_dir;
  bool stream_dir_open;
  bool stream_first_entry;
  bool stream_truncated;
  bool stream_body_done;  // closing envelope already emitted
  uint16_t stream_entry_count;
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
static void write_response_ex(http_conn_t *c, int status, const char *reason,
                              const char *content_type,
                              const char *extra_headers, const char *body,
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

void __not_in_flash_func(http_server_init)(void) {
  DPRINTF("http_server: init entered (g_listen_pcb=%p)\n",
          (void *)g_listen_pcb);
  if (g_listen_pcb != NULL) {
    DPRINTF("http_server: already initialised, returning\n");
    return;  // already running
  }

  g_boot_time = get_absolute_time();

  DPRINTF("http_server: tcp_new_ip_type...\n");
  struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
  if (pcb == NULL) {
    DPRINTF("http_server: tcp_new failed\n");
    return;
  }
  DPRINTF("http_server: tcp_bind on port %d...\n", HTTP_SERVER_PORT);
  err_t err = tcp_bind(pcb, IP_ANY_TYPE, HTTP_SERVER_PORT);
  if (err != ERR_OK) {
    DPRINTF("http_server: tcp_bind failed: %d\n", err);
    tcp_close(pcb);
    return;
  }
  DPRINTF("http_server: tcp_listen_with_backlog...\n");
  struct tcp_pcb *listen_pcb = tcp_listen_with_backlog(pcb, 4);
  if (listen_pcb == NULL) {
    DPRINTF("http_server: tcp_listen failed\n");
    return;
  }
  tcp_accept(listen_pcb, srv_accept_cb);
  g_listen_pcb = listen_pcb;
  DPRINTF("http_server: listening on :%d\n", HTTP_SERVER_PORT);
}

void __not_in_flash_func(http_server_deinit)(void) {
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

static http_conn_t *__not_in_flash_func(conn_alloc)(void) {
  for (int i = 0; i < HTTP_SERVER_MAX_CONNECTIONS; i++) {
    if (g_conns[i].state == HC_FREE) {
      memset(&g_conns[i], 0, sizeof(g_conns[i]));
      g_conns[i].state = HC_READ_HEADERS;
      return &g_conns[i];
    }
  }
  return NULL;
}

static void __not_in_flash_func(conn_free)(http_conn_t *c) {
  c->pcb = NULL;
  c->state = HC_FREE;
}

static void __not_in_flash_func(conn_close)(http_conn_t *c) {
  if (c->stream_dir_open) {
    f_closedir(&c->stream_dir);
    c->stream_dir_open = false;
  }
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

static err_t __not_in_flash_func(srv_accept_cb)(void *arg, struct tcp_pcb *newpcb, err_t err) {
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

static err_t __not_in_flash_func(srv_recv_cb)(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
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
  } else if (c->state == HC_READ_BODY) {
    size_t need = c->content_length - c->body_received;
    size_t take = (p->tot_len < need) ? p->tot_len : need;
    if (take > 0) {
      pbuf_copy_partial(p, c->body + c->body_received, (u16_t)take, 0);
      c->body_received += take;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (c->body_received >= c->content_length) {
      route(c);
    }
  } else {
    // Already writing the response (or draining); discard any extra
    // bytes the client sent.
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
  }
  return ERR_OK;
}

// Forward decl — defined alongside the listing handler below.
static void stream_listing_drive(http_conn_t *c);

static err_t __not_in_flash_func(srv_sent_cb)(void *arg, struct tcp_pcb *pcb, u16_t len) {
  (void)pcb;
  http_conn_t *c = (http_conn_t *)arg;
  if (c == NULL) {
    return ERR_OK;
  }
  c->resp_sent += len;
  if (c->state == HC_WRITE_RESPONSE && c->resp_sent >= c->resp_len) {
    c->state = HC_DRAINING;
    conn_close(c);
  } else if (c->state == HC_STREAM_LISTING) {
    // Peer ack'd some bytes — keep pumping more chunks.
    stream_listing_drive(c);
  } else if (c->state == HC_DRAINING && c->stream_body_done) {
    // Streaming finished and the chunked terminator has now been
    // ack'd; safe to close.
    conn_close(c);
  }
  return ERR_OK;
}

static err_t __not_in_flash_func(srv_poll_cb)(void *arg, struct tcp_pcb *pcb) {
  (void)pcb;
  http_conn_t *c = (http_conn_t *)arg;
  if (c != NULL) {
    DPRINTF("http_server: idle timeout, closing connection\n");
    conn_close(c);
  }
  return ERR_OK;
}

static void __not_in_flash_func(srv_err_cb)(void *arg, err_t err) {
  (void)err;
  http_conn_t *c = (http_conn_t *)arg;
  if (c != NULL) {
    // pcb is already gone per lwIP contract; just release the slot.
    c->pcb = NULL;
    conn_free(c);
  }
}

// --- Request parsing ---

static int __not_in_flash_func(strncasecmp_n)(const char *a, const char *b, size_t n) {
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

static hc_method_t __not_in_flash_func(parse_method)(const char *s, size_t n) {
  if (n == 3 && memcmp(s, "GET", 3) == 0) return HM_GET;
  if (n == 4 && memcmp(s, "HEAD", 4) == 0) return HM_HEAD;
  if (n == 4 && memcmp(s, "POST", 4) == 0) return HM_POST;
  if (n == 3 && memcmp(s, "PUT", 3) == 0) return HM_PUT;
  if (n == 6 && memcmp(s, "DELETE", 6) == 0) return HM_DELETE;
  return HM_UNKNOWN;
}

static void __not_in_flash_func(parse_and_dispatch)(http_conn_t *c) {
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
      } else if (strncasecmp_n(name, "Content-Type", 13) == 0) {
        // Match the media-type prefix; ignore any "; charset=..." tail.
        if (strncasecmp_n(value, "application/json", 16) == 0) {
          c->content_type_json = true;
        }
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

  // For body-bearing methods (POST, PUT) with Content-Length > 0, peel
  // any leftover bytes from the header buffer that fell after
  // \r\n\r\n into the body buffer, then transition to HC_READ_BODY
  // (or dispatch immediately if the entire body was already in the
  // first segment).
  bool body_bearing = (c->method == HM_POST) || (c->method == HM_PUT);
  if (body_bearing && c->content_length > 0) {
    if (c->content_length > HTTP_REQUEST_BODY_BUF_BYTES) {
      write_error(c, 413, "Payload Too Large", "payload_too_large",
                  "Body too large for this route");
      return;
    }
    // p points at the CRLF that ends the empty (terminator) line; the
    // body starts two bytes later. The header parser nul-terminated
    // each header it walked, so c->hdr_len is the original byte count
    // we copied off the wire — anything past (p+2) is body.
    char *body_start = p + 2;
    size_t header_consumed = (size_t)(body_start - c->hdr);
    size_t leftover = (c->hdr_len > header_consumed)
                          ? c->hdr_len - header_consumed
                          : 0;
    if (leftover > c->content_length) leftover = c->content_length;
    if (leftover > 0) {
      memcpy(c->body, body_start, leftover);
      c->body_received = leftover;
    }
    if (c->body_received >= c->content_length) {
      route(c);
      return;
    }
    c->state = HC_READ_BODY;
    return;
  }

  route(c);
}

// --- URL decoding + query parsing ---

static int __not_in_flash_func(hex_digit)(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decode percent-encoding in `src` into `dst`. Treats '+' as literal
// (we don't accept form-encoded query strings as space-encoded).
// Returns false on bad encoding, NUL byte in result, or buffer
// overflow.
static bool __not_in_flash_func(url_decode)(const char *src, char *dst, size_t dst_cap) {
  size_t out = 0;
  while (*src != '\0') {
    if (out + 1 >= dst_cap) return false;
    char c = *src++;
    if (c == '%') {
      int hi = hex_digit((unsigned char)*src);
      if (hi < 0) return false;
      src++;
      int lo = hex_digit((unsigned char)*src);
      if (lo < 0) return false;
      src++;
      int v = (hi << 4) | lo;
      if (v == 0) return false;  // NUL not allowed in paths
      dst[out++] = (char)v;
    } else {
      dst[out++] = c;
    }
  }
  dst[out] = '\0';
  return true;
}

// Find `key` in a URL-encoded query string `?a=1&b=2&...`. Writes the
// URL-decoded value into `out`. Returns true on hit (even when value
// is empty), false if key not present or buffer would overflow.
static bool __not_in_flash_func(query_get)(const char *query, const char *key, char *out,
                      size_t out_cap) {
  size_t key_len = strlen(key);
  const char *p = query;
  while (*p != '\0') {
    const char *amp = strchr(p, '&');
    const char *eq = strchr(p, '=');
    const char *seg_end = (amp != NULL) ? amp : (p + strlen(p));
    if (eq != NULL && eq < seg_end && (size_t)(eq - p) == key_len &&
        memcmp(p, key, key_len) == 0) {
      // Decode value [eq+1 .. seg_end).
      char tmp[HTTP_QUERY_BUF_BYTES];
      size_t v_len = (size_t)(seg_end - (eq + 1));
      if (v_len >= sizeof(tmp)) return false;
      memcpy(tmp, eq + 1, v_len);
      tmp[v_len] = '\0';
      return url_decode(tmp, out, out_cap);
    }
    if (amp == NULL) break;
    p = amp + 1;
  }
  return false;
}

// --- Path normalisation and jailing ---

// Reads gconfig PARAM_HOSTNAME (used for the API's mDNS name); we
// don't actually need it here but the GEMDRIVE_FOLDER lookup is the
// real prize.
static const char *__not_in_flash_func(get_gemdrive_folder)(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_GEMDRIVE_FOLDER);
  if (entry == NULL || entry->value[0] == '\0') return "/devops";
  return entry->value;
}

// Normalise `rel` (already URL-decoded) into `out` as a forward-slash-
// separated relative path with leading slash. Collapses "//" and "/.",
// strips trailing "/". Rejects "..", non-ASCII, and control chars.
// Result is always "/" or "/foo[/bar...]" — never empty.
typedef enum {
  NORM_OK = 0,
  NORM_BAD_PATH,
  NORM_TOO_LONG,
} norm_status_t;

static norm_status_t __not_in_flash_func(normalize_rel)(const char *rel, char *out,
                                   size_t out_cap) {
  if (out_cap < 2) return NORM_TOO_LONG;
  out[0] = '/';
  out[1] = '\0';
  if (rel == NULL || rel[0] == '\0' || strcmp(rel, "/") == 0) {
    return NORM_OK;
  }
  if (out_cap < 2) return NORM_TOO_LONG;

  size_t i = 0;
  size_t out_len = 0;
  out[0] = '\0';

  // Skip a single leading slash (we always re-emit one).
  if (rel[i] == '/') i++;

  while (rel[i] != '\0') {
    // Find next segment end.
    size_t seg_start = i;
    while (rel[i] != '\0' && rel[i] != '/') {
      char ch = rel[i];
      if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x80) {
        return NORM_BAD_PATH;
      }
      i++;
    }
    size_t seg_len = i - seg_start;
    if (seg_len == 0) {
      // Repeated slash; skip.
    } else if (seg_len == 1 && rel[seg_start] == '.') {
      // "." segment; skip.
    } else if (seg_len == 2 && rel[seg_start] == '.' &&
               rel[seg_start + 1] == '.') {
      return NORM_BAD_PATH;  // ".." escapes the root.
    } else {
      if (out_len + 1 + seg_len + 1 > out_cap) return NORM_TOO_LONG;
      out[out_len++] = '/';
      // FAT 8.3 names are stored uppercase. Case-fold ASCII letters
      // here so every FatFs call (mkdir / unlink / rename / stat /
      // open) goes through with the canonical uppercase form. Names
      // pre-existing on disk in mixed case still match because LFN
      // matching is case-insensitive.
      for (size_t k = 0; k < seg_len; k++) {
        char ch = rel[seg_start + k];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        out[out_len++] = ch;
      }
      out[out_len] = '\0';
    }
    if (rel[i] == '/') i++;
  }

  if (out_len == 0) {
    out[0] = '/';
    out[1] = '\0';
  }
  return NORM_OK;
}

// Build absolute on-disk path: GEMDRIVE_FOLDER + normalised rel.
// rel can be NULL/empty (treated as "/").
static norm_status_t __not_in_flash_func(resolve_jail)(const char *rel, char *out,
                                  size_t out_cap) {
  char norm[HTTP_PATH_BUF_BYTES];
  norm_status_t s = normalize_rel(rel, norm, sizeof(norm));
  if (s != NORM_OK) return s;

  const char *root = get_gemdrive_folder();
  size_t root_len = strlen(root);
  size_t norm_len = strlen(norm);

  // GEMDRIVE_FOLDER is expected to be "/foo" (no trailing slash). If
  // norm is just "/", drop the duplicate slash to yield exactly the
  // root.
  if (norm_len == 1) {
    if (root_len + 1 > out_cap) return NORM_TOO_LONG;
    memcpy(out, root, root_len + 1);
    return NORM_OK;
  }
  if (root_len + norm_len + 1 > out_cap) return NORM_TOO_LONG;
  memcpy(out, root, root_len);
  memcpy(out + root_len, norm, norm_len + 1);
  return NORM_OK;
}

// --- 8.3 filename validation ---

// Returns true iff `c` is in the FAT 8.3 illegal-character set. We
// accept the strict set: control chars, the ten classic separators
// the FAT spec calls out, and a few extras that DOS/Windows
// historically rejected ('+', ',', ';', '=', '[', ']'). Lower-case
// letters are allowed at validation time and case-folded by FatFs;
// our jail returns the canonical uppercase form to clients.
static bool __not_in_flash_func(fat_illegal_char)(unsigned char ch) {
  if (ch < 0x20) return true;
  if (ch >= 0x80) return true;
  static const char banned[] = "\"*/:<>?\\|+,;=[]";
  for (const char *q = banned; *q != '\0'; q++) {
    if (ch == (unsigned char)*q) return true;
  }
  return false;
}

// Validate the LAST segment of a normalised relative path against
// FAT 8.3 (stem ≤ 8, ext ≤ 3, illegal chars rejected). On success
// returns NORM_OK; on stem/ext length violation returns NORM_TOO_LONG
// (caller maps to name_too_long); on illegal-char or empty/dot-only
// names returns NORM_BAD_PATH (caller maps to bad_path).
//
// `norm` is the leading-slash form produced by normalize_rel (e.g.
// "/foo/bar.txt"). This function only inspects the segment after the
// last '/'.
static norm_status_t __not_in_flash_func(validate_8_3_last)(const char *norm) {
  const char *last_slash = strrchr(norm, '/');
  const char *seg = (last_slash != NULL) ? last_slash + 1 : norm;
  size_t seg_len = strlen(seg);
  if (seg_len == 0) return NORM_BAD_PATH;
  if (seg[0] == '.' || seg[0] == ' ') return NORM_BAD_PATH;
  if (seg[seg_len - 1] == ' ' || seg[seg_len - 1] == '.') {
    return NORM_BAD_PATH;
  }

  // Walk the segment counting stem and ext lengths, with at most one
  // dot allowed.
  size_t stem_len = 0;
  size_t ext_len = 0;
  bool seen_dot = false;
  for (size_t i = 0; i < seg_len; i++) {
    unsigned char ch = (unsigned char)seg[i];
    if (ch == '.') {
      if (seen_dot) return NORM_BAD_PATH;  // multiple dots
      seen_dot = true;
      continue;
    }
    if (fat_illegal_char(ch)) return NORM_BAD_PATH;
    if (!seen_dot) {
      stem_len++;
    } else {
      ext_len++;
    }
  }
  if (stem_len == 0) return NORM_BAD_PATH;     // ".ext"-style
  if (stem_len > 8) return NORM_TOO_LONG;
  if (ext_len > 3) return NORM_TOO_LONG;
  return NORM_OK;
}

// --- Tiny JSON {"key":"value"} extractor ---
//
// Hand-rolled per the locked decision: scoped to flat string-valued
// objects, no arrays, no numbers, no Unicode escapes. Tolerates
// whitespace and recognises \", \\ and \/ within string values. Used
// by rename routes to read the `to` field and nothing else.

typedef enum {
  JSON_OK = 0,
  JSON_BAD,           // malformed (return as bad_json)
  JSON_KEY_MISSING,   // syntactically valid but key absent / not a string
} json_status_t;

static const char *__not_in_flash_func(skip_ws)(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
    p++;
  }
  return p;
}

static json_status_t __not_in_flash_func(json_extract_string)(const char *body, size_t body_len,
                                         const char *key, char *out,
                                         size_t out_cap) {
  const char *p = body;
  const char *end = body + body_len;
  size_t key_len = strlen(key);

  p = skip_ws(p, end);
  if (p >= end || *p != '{') return JSON_BAD;
  p++;

  while (1) {
    p = skip_ws(p, end);
    if (p >= end) return JSON_BAD;
    if (*p == '}') return JSON_KEY_MISSING;
    if (*p != '"') return JSON_BAD;
    p++;

    // Read key.
    const char *key_start = p;
    while (p < end && *p != '"') {
      if (*p == '\\') p++;  // skip escape
      if (p < end) p++;
    }
    if (p >= end || *p != '"') return JSON_BAD;
    size_t this_key_len = (size_t)(p - key_start);
    bool key_match = (this_key_len == key_len) &&
                     (memcmp(key_start, key, key_len) == 0);
    p++;  // past closing quote

    p = skip_ws(p, end);
    if (p >= end || *p != ':') return JSON_BAD;
    p++;
    p = skip_ws(p, end);

    // Value: only string values supported. A non-string value on the
    // sought key is unprocessable; on an unsought key we still need
    // to skip it.
    if (p >= end) return JSON_BAD;
    if (*p != '"') {
      if (key_match) return JSON_KEY_MISSING;
      // Skip a primitive-or-object value crudely.
      int depth = 0;
      bool started = false;
      while (p < end) {
        char ch = *p;
        if (ch == '{' || ch == '[') {
          depth++;
          started = true;
        } else if (ch == '}' || ch == ']') {
          if (depth == 0) break;
          depth--;
        } else if (depth == 0 && (ch == ',' || ch == '\r' || ch == '\n')) {
          break;
        }
        p++;
        if (started && depth == 0) break;
      }
    } else {
      p++;  // opening quote
      // Read value (with escape handling).
      size_t out_off = 0;
      while (p < end && *p != '"') {
        char ch = *p;
        if (ch == '\\' && (p + 1) < end) {
          p++;
          char esc = *p;
          if (esc == '"' || esc == '\\' || esc == '/') {
            ch = esc;
          } else if (esc == 'n') {
            ch = '\n';
          } else if (esc == 't') {
            ch = '\t';
          } else if (esc == 'r') {
            ch = '\r';
          } else {
            return JSON_BAD;
          }
        }
        if (key_match) {
          if (out_off + 1 >= out_cap) return JSON_BAD;
          out[out_off++] = ch;
        }
        p++;
      }
      if (p >= end || *p != '"') return JSON_BAD;
      p++;
      if (key_match) {
        out[out_off] = '\0';
        return JSON_OK;
      }
    }

    p = skip_ws(p, end);
    if (p >= end) return JSON_BAD;
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p == '}') {
      return JSON_KEY_MISSING;
    }
    return JSON_BAD;
  }
}

// FAT date+time → ISO-8601 "YYYY-MM-DDTHH:MM:SS". Returns false if
// the date field is zero (FatFs uses 0 for "no date set") so the
// caller can emit `null`.
static bool __not_in_flash_func(fat_to_iso8601)(WORD fdate, WORD ftime, char *out,
                           size_t out_cap) {
  if (fdate == 0) return false;
  unsigned year = 1980u + ((fdate >> 9) & 0x7F);
  unsigned month = (fdate >> 5) & 0x0F;
  unsigned day = fdate & 0x1F;
  unsigned hour = (ftime >> 11) & 0x1F;
  unsigned minute = (ftime >> 5) & 0x3F;
  unsigned second = (ftime & 0x1F) * 2;
  int n = snprintf(out, out_cap, "%04u-%02u-%02uT%02u:%02u:%02u", year,
                   month, day, hour, minute, second);
  return (n > 0 && (size_t)n < out_cap);
}

// --- Route handlers ---

static void __not_in_flash_func(handle_ping)(http_conn_t *c) {
  uint64_t uptime_us =
      (uint64_t)absolute_time_diff_us(g_boot_time, get_absolute_time());
  uint64_t uptime_s = uptime_us / 1000000ULL;
  char body[160];
  int n = snprintf(body, sizeof(body),
                   "{\"ok\":true,\"version\":\"%s\",\"uptime_s\":%llu}\n",
                   RELEASE_VERSION, (unsigned long long)uptime_s);
  if (n < 0) n = 0;
  write_response(c, 200, "OK", "application/json", body, (size_t)n);
}

static const char *__not_in_flash_func(fat_type_name)(BYTE fs_type) {
  switch (fs_type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "EXFAT";
    default: return "UNKNOWN";
  }
}

static void __not_in_flash_func(handle_volume)(http_conn_t *c) {
  FATFS *fs = NULL;
  DWORD free_clusters = 0;
  FRESULT res = f_getfree("", &free_clusters, &fs);
  if (res != FR_OK || fs == NULL) {
    DPRINTF("http_server: /volume f_getfree failed (%d)\n", (int)res);
    write_error(c, 503, "Service Unavailable", "busy",
                "SD filesystem unavailable");
    return;
  }
  uint64_t bytes_per_sector =
#if FF_MAX_SS == FF_MIN_SS
      (uint64_t)FF_MIN_SS;
#else
      (uint64_t)fs->ssize;
#endif
  uint64_t cluster_bytes = (uint64_t)fs->csize * bytes_per_sector;
  uint64_t total_bytes = (uint64_t)(fs->n_fatent - 2) * cluster_bytes;
  uint64_t free_bytes = (uint64_t)free_clusters * cluster_bytes;
  char body[200];
  int n = snprintf(body, sizeof(body),
                   "{\"ok\":true,\"total_b\":%llu,\"free_b\":%llu,"
                   "\"fs_type\":\"%s\"}\n",
                   (unsigned long long)total_bytes,
                   (unsigned long long)free_bytes, fat_type_name(fs->fs_type));
  if (n < 0) n = 0;
  write_response(c, 200, "OK", "application/json", body, (size_t)n);
}

// Append printf-formatted text to the response body buffer, advancing
// *body_len. Returns false if the buffer is full (caller should set
// truncated and stop appending).
static bool body_appendf(char *body, size_t cap, size_t *len,
                         const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static bool __not_in_flash_func(body_appendf)(char *body, size_t cap, size_t *len,
                         const char *fmt, ...) {
  if (*len >= cap) return false;
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(body + *len, cap - *len, fmt, args);
  va_end(args);
  if (n < 0 || (size_t)n >= cap - *len) {
    return false;
  }
  *len += (size_t)n;
  return true;
}

// --- Streaming directory listing (chunked transfer-encoding) ---
//
// The response is built as multiple HTTP/1.1 chunked-encoding chunks
// drained out as fast as the TCP send buffer can absorb them. Per
// chunk: `<hex-size>\r\n<body>\r\n`. End-of-body: `0\r\n\r\n`.
//
// We keep the FatFs DIR open across recv/sent callbacks; conn_close
// (also invoked on error/abort) releases it.

// Bytes reserved at the tail of each chunk so the closing JSON
// envelope (`],"truncated":<bool>}\n`) can always fit on whichever
// chunk turns out to be the last.
#define STREAM_CLOSE_RESERVE 32

// Append one entry (with its leading comma when needed) to `out`.
// Returns false if the formatted entry didn't fit.
static bool __not_in_flash_func(stream_append_entry)(http_conn_t *c,
                                                     char *out,
                                                     size_t out_cap,
                                                     size_t *out_len,
                                                     const FILINFO *finfo) {
  // FAT 8.3 names are uppercase by convention. Some entries on disk
  // may have been written in mixed case (LFN preserves case); we
  // case-fold to upper for the wire so the API always returns the
  // canonical form regardless of how the entry was stored.
  char name_upper[FF_MAX_LFN + 1];
  size_t i = 0;
  for (; finfo->fname[i] != '\0' && i < sizeof(name_upper) - 1; i++) {
    char ch = finfo->fname[i];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    name_upper[i] = ch;
  }
  name_upper[i] = '\0';

  char mtime[24];
  bool has_mtime =
      fat_to_iso8601(finfo->fdate, finfo->ftime, mtime, sizeof(mtime));
  bool is_dir = (finfo->fattrib & AM_DIR) != 0;
  uint32_t size = is_dir ? 0u : (uint32_t)finfo->fsize;
  const char *sep = c->stream_first_entry ? "" : ",";
  int n;
  if (has_mtime) {
    n = snprintf(out + *out_len, out_cap - *out_len,
                 "%s{\"name\":\"%s\",\"size\":%lu,\"is_dir\":%s,"
                 "\"mtime\":\"%s\"}",
                 sep, name_upper, (unsigned long)size,
                 is_dir ? "true" : "false", mtime);
  } else {
    n = snprintf(out + *out_len, out_cap - *out_len,
                 "%s{\"name\":\"%s\",\"size\":%lu,\"is_dir\":%s,"
                 "\"mtime\":null}",
                 sep, name_upper, (unsigned long)size,
                 is_dir ? "true" : "false");
  }
  if (n < 0 || (size_t)n >= out_cap - *out_len) return false;
  *out_len += (size_t)n;
  c->stream_first_entry = false;
  c->stream_entry_count++;
  return true;
}

// Build the body of one chunk. The optional `prefix` is emitted once
// at the very start (the `{"ok":true,"path":"<norm>","entries":[`
// envelope opener for the first chunk; NULL for subsequent chunks).
// Returns the body length. Side effects: may set
// c->stream_truncated and c->stream_body_done.
static size_t __not_in_flash_func(stream_build_body)(http_conn_t *c,
                                                     const char *prefix,
                                                     char *out,
                                                     size_t out_cap) {
  size_t out_len = 0;
  if (prefix != NULL) {
    int n = snprintf(out, out_cap, "%s", prefix);
    if (n < 0 || (size_t)n >= out_cap) return 0;
    out_len = (size_t)n;
  }

  // Walk entries until cap, dir exhausted, or buffer ~full.
  while (1) {
    if (c->stream_entry_count >= HTTP_LISTING_MAX_ENTRIES) {
      c->stream_truncated = true;
      break;
    }
    if (out_len + STREAM_CLOSE_RESERVE >= out_cap) {
      // No room for one more entry plus closing envelope; stop here
      // and let the next chunk pick up. (Not done — body_done stays
      // false so we'll come back.)
      return out_len;
    }
    FILINFO finfo;
    FRESULT fr = f_readdir(&c->stream_dir, &finfo);
    if (fr != FR_OK || finfo.fname[0] == '\0') break;  // dir exhausted
    if (!stream_append_entry(c, out, out_cap, &out_len, &finfo)) {
      // Entry didn't fit even though we had reserve space — abnormal
      // (LFN ate more than expected). Drop and mark truncated.
      c->stream_truncated = true;
      break;
    }
  }

  // Dir exhausted (or cap hit). Append closing envelope.
  int n = snprintf(out + out_len, out_cap - out_len,
                   "],\"truncated\":%s}\n",
                   c->stream_truncated ? "true" : "false");
  if (n > 0 && (size_t)n < out_cap - out_len) {
    out_len += (size_t)n;
    c->stream_body_done = true;
  }
  return out_len;
}

// Send one chunk: `<hex-size>\r\n<body>\r\n`. Returns lwIP err.
static err_t __not_in_flash_func(stream_send_chunk)(http_conn_t *c,
                                                    const char *body,
                                                    size_t body_len) {
  char hdr[16];
  int hn = snprintf(hdr, sizeof(hdr), "%lX\r\n", (unsigned long)body_len);
  if (hn < 0 || hn >= (int)sizeof(hdr)) return ERR_VAL;
  err_t err = tcp_write(c->pcb, hdr, (u16_t)hn,
                        TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
  if (err != ERR_OK) return err;
  if (body_len > 0) {
    err = tcp_write(c->pcb, body, (u16_t)body_len,
                    TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
    if (err != ERR_OK) return err;
  }
  err = tcp_write(c->pcb, "\r\n", 2, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK) return err;
  tcp_output(c->pcb);
  return ERR_OK;
}

// Send the chunked-encoding terminator (zero-length chunk).
static err_t __not_in_flash_func(stream_send_terminator)(http_conn_t *c) {
  err_t err = tcp_write(c->pcb, "0\r\n\r\n", 5, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK) return err;
  tcp_output(c->pcb);
  return ERR_OK;
}

// Pump one round of streaming: build the next body, send it as a
// chunk, decide whether to also send the terminator and close.
// Called both from handle_files_list (initial round) and from
// srv_sent_cb (subsequent rounds, driven by peer acks).
static void __not_in_flash_func(stream_listing_drive)(http_conn_t *c) {
  // Worst-case bytes we'd queue this round: chunk header + body +
  // CRLF + terminator. Need send buffer headroom for it.
  size_t needed = HTTP_RESPONSE_BUF_BYTES + 16 + 8;
  if (tcp_sndbuf(c->pcb) < needed) {
    return;  // try again on next sent_cb
  }

  if (c->stream_body_done) {
    // Body emitted on a previous round; just send the terminator and
    // close once it's ack'd.
    if (stream_send_terminator(c) != ERR_OK) {
      conn_close(c);
      return;
    }
    c->state = HC_DRAINING;
    return;
  }

  size_t body_len =
      stream_build_body(c, NULL, c->resp, sizeof(c->resp));
  if (stream_send_chunk(c, c->resp, body_len) != ERR_OK) {
    conn_close(c);
    return;
  }
  if (c->stream_body_done) {
    if (stream_send_terminator(c) != ERR_OK) {
      conn_close(c);
      return;
    }
    c->state = HC_DRAINING;
  }
}

static void __not_in_flash_func(handle_files_list)(http_conn_t *c) {
  // Extract ?path=, default to "/".
  char rel[HTTP_PATH_BUF_BYTES];
  rel[0] = '\0';
  if (c->query[0] != '\0') {
    if (!query_get(c->query, "path", rel, sizeof(rel))) {
      if (strstr(c->query, "path=") != NULL) {
        write_error(c, 400, "Bad Request", "bad_path",
                    "Malformed path query parameter");
        return;
      }
    }
  }

  char abs_path[HTTP_FAT_PATH_BUF_BYTES];
  norm_status_t s = resolve_jail(rel, abs_path, sizeof(abs_path));
  if (s == NORM_BAD_PATH) {
    write_error(c, 400, "Bad Request", "bad_path", "Path not allowed");
    return;
  }
  if (s == NORM_TOO_LONG) {
    write_error(c, 400, "Bad Request", "bad_path", "Path too long");
    return;
  }

  FILINFO info;
  FRESULT fr = f_stat(abs_path, &info);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    bool is_root = (rel[0] == '\0' || strcmp(rel, "/") == 0);
    if (!is_root) {
      write_error(c, 404, "Not Found", "not_found", "Path not found");
      return;
    }
  } else if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_stat failed");
    return;
  } else if (!(info.fattrib & AM_DIR)) {
    write_error(c, 422, "Unprocessable Entity", "is_file",
                "Path is a file; use GET /api/v1/files/<rel> to download");
    return;
  }

  // Open the directory for streaming. conn_close releases it.
  fr = f_opendir(&c->stream_dir, abs_path);
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_opendir failed");
    return;
  }
  c->stream_dir_open = true;
  c->stream_first_entry = true;
  c->stream_entry_count = 0;
  c->stream_truncated = false;
  c->stream_body_done = false;

  // Canonical normalised path to echo in the envelope.
  char norm[HTTP_PATH_BUF_BYTES];
  if (normalize_rel(rel, norm, sizeof(norm)) != NORM_OK) {
    norm[0] = '/';
    norm[1] = '\0';
  }

  // Send status line + headers (chunked encoding, no Content-Length).
  int hn = snprintf(c->resp, sizeof(c->resp),
                    "HTTP/1.1 200 OK\r\n"
                    "Server: md-devops/%s\r\n"
                    "Connection: close\r\n"
                    "Content-Type: application/json\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n",
                    RELEASE_VERSION);
  if (hn < 0 || (size_t)hn >= sizeof(c->resp)) {
    conn_close(c);
    return;
  }
  err_t err = tcp_write(c->pcb, c->resp, (u16_t)hn,
                        TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
  if (err != ERR_OK) {
    conn_close(c);
    return;
  }

  // HEAD: emit the chunked terminator and close.
  if (c->is_head) {
    (void)stream_send_terminator(c);
    conn_close(c);
    return;
  }

  // First chunk: envelope opener + first batch of entries.
  char prefix[80];
  int pn = snprintf(prefix, sizeof(prefix),
                    "{\"ok\":true,\"path\":\"%s\",\"entries\":[", norm);
  if (pn < 0 || (size_t)pn >= sizeof(prefix)) {
    conn_close(c);
    return;
  }
  size_t body_len =
      stream_build_body(c, prefix, c->resp, sizeof(c->resp));
  if (stream_send_chunk(c, c->resp, body_len) != ERR_OK) {
    conn_close(c);
    return;
  }

  c->state = HC_STREAM_LISTING;
  if (c->stream_body_done) {
    if (stream_send_terminator(c) != ERR_OK) {
      conn_close(c);
      return;
    }
    c->state = HC_DRAINING;
  }
}

// --- Folder mutation handlers (S3) ---
//
// `rel` is the URL path component AFTER /api/v1/folders/, possibly
// still containing percent-encoded bytes. The handlers URL-decode,
// normalise, jail, validate 8.3 on the last segment, then call FatFs.

static void __not_in_flash_func(write_path_error)(http_conn_t *c, norm_status_t s) {
  if (s == NORM_TOO_LONG) {
    write_error(c, 400, "Bad Request", "name_too_long",
                "Path or name exceeds FAT 8.3 limits");
  } else {
    write_error(c, 400, "Bad Request", "bad_path", "Path not allowed");
  }
}

// Resolve a URL-decoded relative path into normalised form (norm) and
// FatFs absolute form (abs). Returns NORM_OK on success or a status
// the caller maps to the right HTTP error.
static norm_status_t __not_in_flash_func(resolve_pair)(const char *url_rel, char *norm,
                                  size_t norm_cap, char *abs,
                                  size_t abs_cap) {
  char decoded[HTTP_PATH_BUF_BYTES];
  if (!url_decode(url_rel, decoded, sizeof(decoded))) return NORM_BAD_PATH;
  norm_status_t s = normalize_rel(decoded, norm, norm_cap);
  if (s != NORM_OK) return s;
  // Re-jail the now-normalised relative path.
  return resolve_jail(norm, abs, abs_cap);
}

// Test whether the FILINFO returned by f_stat indicates a directory.
// Only valid when f_stat returned FR_OK.
static bool __not_in_flash_func(finfo_is_dir)(const FILINFO *info) {
  return (info->fattrib & AM_DIR) != 0;
}

static void __not_in_flash_func(handle_folder_create)(http_conn_t *c, const char *url_rel) {
  char norm[HTTP_PATH_BUF_BYTES];
  char abs_path[HTTP_FAT_PATH_BUF_BYTES];
  norm_status_t s = resolve_pair(url_rel, norm, sizeof(norm), abs_path,
                                 sizeof(abs_path));
  if (s != NORM_OK) {
    write_path_error(c, s);
    return;
  }
  // Root is always present; can't create.
  if (strcmp(norm, "/") == 0) {
    write_error(c, 409, "Conflict", "conflict",
                "Root folder already exists");
    return;
  }
  // 8.3 check on the last segment (the new folder name).
  norm_status_t v = validate_8_3_last(norm);
  if (v != NORM_OK) {
    if (v == NORM_TOO_LONG) {
      write_error(c, 400, "Bad Request", "name_too_long",
                  "Folder name exceeds FAT 8.3 limits");
    } else {
      write_error(c, 400, "Bad Request", "bad_path",
                  "Folder name has illegal characters");
    }
    return;
  }
  // Check existence to disambiguate 409 (exists) vs 404 (parent missing).
  FILINFO info;
  FRESULT fr = f_stat(abs_path, &info);
  if (fr == FR_OK) {
    write_error(c, 409, "Conflict", "conflict", "Folder already exists");
    return;
  }
  if (fr != FR_NO_FILE && fr != FR_NO_PATH) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_stat failed");
    return;
  }
  fr = f_mkdir(abs_path);
  if (fr == FR_NO_PATH) {
    write_error(c, 404, "Not Found", "not_found",
                "Parent folder does not exist");
    return;
  }
  if (fr == FR_EXIST) {
    write_error(c, 409, "Conflict", "conflict", "Folder already exists");
    return;
  }
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error", "f_mkdir failed");
    return;
  }

  char body[200];
  int n = snprintf(body, sizeof(body), "{\"ok\":true,\"path\":\"%s\"}\n",
                   norm);
  if (n < 0) n = 0;
  char extra[256];
  int en = snprintf(extra, sizeof(extra),
                    "Location: /api/v1/folders%s\r\n", norm);
  if (en < 0 || (size_t)en >= sizeof(extra)) extra[0] = '\0';
  write_response_ex(c, 201, "Created", "application/json",
                    (extra[0] != '\0') ? extra : NULL, body, (size_t)n);
}

static void __not_in_flash_func(handle_folder_delete)(http_conn_t *c, const char *url_rel) {
  char norm[HTTP_PATH_BUF_BYTES];
  char abs_path[HTTP_FAT_PATH_BUF_BYTES];
  norm_status_t s = resolve_pair(url_rel, norm, sizeof(norm), abs_path,
                                 sizeof(abs_path));
  if (s != NORM_OK) {
    write_path_error(c, s);
    return;
  }
  if (strcmp(norm, "/") == 0) {
    write_error(c, 409, "Conflict", "conflict", "Cannot delete root");
    return;
  }

  FILINFO info;
  FRESULT fr = f_stat(abs_path, &info);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    write_error(c, 404, "Not Found", "not_found", "Folder not found");
    return;
  }
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_stat failed");
    return;
  }
  if (!finfo_is_dir(&info)) {
    write_error(c, 404, "Not Found", "is_file",
                "Path is a file; use DELETE /api/v1/files/<rel>");
    return;
  }
  fr = f_unlink(abs_path);
  if (fr == FR_DENIED) {
    write_error(c, 409, "Conflict", "conflict", "Folder is not empty");
    return;
  }
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_unlink failed");
    return;
  }
  // 204 No Content — body intentionally empty.
  write_response(c, 204, "No Content", "application/json", NULL, 0);
}

static void __not_in_flash_func(handle_folder_rename)(http_conn_t *c, const char *url_rel) {
  if (c->content_length == 0) {
    write_error(c, 411, "Length Required", "length_required",
                "JSON body required");
    return;
  }
  // Per spec rename routes accept application/json only. The parser
  // captures the flag as it walks headers (in-place NUL-termination
  // means we can't re-scan c->hdr afterwards).
  if (!c->content_type_json) {
    write_error(c, 415, "Unsupported Media Type", "unsupported_media",
                "Content-Type must be application/json");
    return;
  }

  char to_url[HTTP_PATH_BUF_BYTES];
  json_status_t js = json_extract_string(c->body, c->body_received, "to",
                                         to_url, sizeof(to_url));
  if (js == JSON_BAD) {
    write_error(c, 422, "Unprocessable Entity", "bad_json",
                "Malformed JSON body");
    return;
  }
  if (js != JSON_OK) {
    write_error(c, 422, "Unprocessable Entity", "unprocessable",
                "Missing or non-string `to` field");
    return;
  }

  // Resolve source.
  char src_norm[HTTP_PATH_BUF_BYTES];
  char src_abs[HTTP_FAT_PATH_BUF_BYTES];
  norm_status_t s = resolve_pair(url_rel, src_norm, sizeof(src_norm), src_abs,
                                 sizeof(src_abs));
  if (s != NORM_OK) {
    write_path_error(c, s);
    return;
  }
  if (strcmp(src_norm, "/") == 0) {
    write_error(c, 409, "Conflict", "conflict", "Cannot rename root");
    return;
  }

  // Resolve target. `to` is taken as-is (already-decoded JSON string).
  char dst_norm[HTTP_PATH_BUF_BYTES];
  s = normalize_rel(to_url, dst_norm, sizeof(dst_norm));
  if (s != NORM_OK) {
    write_path_error(c, s);
    return;
  }
  char dst_abs[HTTP_FAT_PATH_BUF_BYTES];
  s = resolve_jail(dst_norm, dst_abs, sizeof(dst_abs));
  if (s != NORM_OK) {
    write_path_error(c, s);
    return;
  }
  if (strcmp(dst_norm, "/") == 0) {
    write_error(c, 409, "Conflict", "conflict",
                "Cannot rename onto root");
    return;
  }
  // 8.3 on the new last segment.
  norm_status_t v = validate_8_3_last(dst_norm);
  if (v != NORM_OK) {
    write_path_error(c, v);
    return;
  }

  // No-op rename.
  if (strcmp(src_norm, dst_norm) == 0) {
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"from\":\"%s\",\"to\":\"%s\"}\n", src_norm,
                     dst_norm);
    if (n < 0) n = 0;
    write_response(c, 200, "OK", "application/json", body, (size_t)n);
    return;
  }

  // Cycle: target inside source subtree.
  size_t src_len = strlen(src_norm);
  if (strncmp(dst_norm, src_norm, src_len) == 0 && dst_norm[src_len] == '/') {
    write_error(c, 422, "Unprocessable Entity", "unprocessable",
                "Cannot rename into own descendant");
    return;
  }

  // Source must exist as a directory.
  FILINFO info;
  FRESULT fr = f_stat(src_abs, &info);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    write_error(c, 404, "Not Found", "not_found", "Folder not found");
    return;
  }
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_stat failed");
    return;
  }
  if (!finfo_is_dir(&info)) {
    write_error(c, 404, "Not Found", "is_file",
                "Path is a file; use POST /api/v1/files/<rel>/rename");
    return;
  }

  // Target must NOT exist.
  fr = f_stat(dst_abs, &info);
  if (fr == FR_OK) {
    write_error(c, 409, "Conflict", "conflict", "Target already exists");
    return;
  }
  if (fr != FR_NO_FILE && fr != FR_NO_PATH) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_stat failed (target)");
    return;
  }

  fr = f_rename(src_abs, dst_abs);
  if (fr == FR_NO_PATH) {
    write_error(c, 404, "Not Found", "not_found",
                "Target parent does not exist");
    return;
  }
  if (fr == FR_EXIST) {
    write_error(c, 409, "Conflict", "conflict", "Target already exists");
    return;
  }
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_rename failed");
    return;
  }

  char body[400];
  int n = snprintf(body, sizeof(body),
                   "{\"ok\":true,\"from\":\"%s\",\"to\":\"%s\"}\n", src_norm,
                   dst_norm);
  if (n < 0) n = 0;
  write_response(c, 200, "OK", "application/json", body, (size_t)n);
}

// --- File mutation handlers (S4) ---
//
// Mirror the folder routes but enforce that the resolved path is a
// regular file. Cross-namespace responses (e.g. DELETE /files/<dir>)
// return 404 with an `is_directory` code so the client can detect it
// and switch namespace.

static void __not_in_flash_func(handle_file_delete)(http_conn_t *c,
                                                    const char *url_rel) {
  char norm[HTTP_PATH_BUF_BYTES];
  char abs_path[HTTP_FAT_PATH_BUF_BYTES];
  norm_status_t s = resolve_pair(url_rel, norm, sizeof(norm), abs_path,
                                 sizeof(abs_path));
  if (s != NORM_OK) {
    write_path_error(c, s);
    return;
  }
  if (strcmp(norm, "/") == 0) {
    write_error(c, 404, "Not Found", "not_found", "Cannot delete root");
    return;
  }
  FILINFO info;
  FRESULT fr = f_stat(abs_path, &info);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    write_error(c, 404, "Not Found", "not_found", "File not found");
    return;
  }
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_stat failed");
    return;
  }
  if (finfo_is_dir(&info)) {
    write_error(c, 404, "Not Found", "is_directory",
                "Path is a directory; use DELETE /api/v1/folders/<rel>");
    return;
  }
  fr = f_unlink(abs_path);
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_unlink failed");
    return;
  }
  // 204 No Content.
  write_response(c, 204, "No Content", "application/json", NULL, 0);
}

static void __not_in_flash_func(handle_file_rename)(http_conn_t *c,
                                                    const char *url_rel) {
  if (c->content_length == 0) {
    write_error(c, 411, "Length Required", "length_required",
                "JSON body required");
    return;
  }
  if (!c->content_type_json) {
    write_error(c, 415, "Unsupported Media Type", "unsupported_media",
                "Content-Type must be application/json");
    return;
  }

  char to_url[HTTP_PATH_BUF_BYTES];
  json_status_t js = json_extract_string(c->body, c->body_received, "to",
                                         to_url, sizeof(to_url));
  if (js == JSON_BAD) {
    write_error(c, 422, "Unprocessable Entity", "bad_json",
                "Malformed JSON body");
    return;
  }
  if (js != JSON_OK) {
    write_error(c, 422, "Unprocessable Entity", "unprocessable",
                "Missing or non-string `to` field");
    return;
  }

  // Resolve source.
  char src_norm[HTTP_PATH_BUF_BYTES];
  char src_abs[HTTP_FAT_PATH_BUF_BYTES];
  norm_status_t s = resolve_pair(url_rel, src_norm, sizeof(src_norm), src_abs,
                                 sizeof(src_abs));
  if (s != NORM_OK) {
    write_path_error(c, s);
    return;
  }
  if (strcmp(src_norm, "/") == 0) {
    write_error(c, 404, "Not Found", "not_found", "Cannot rename root");
    return;
  }

  // Resolve target. `to` was JSON-decoded already so we run it through
  // normalize_rel directly (no URL-decoding needed).
  char dst_norm[HTTP_PATH_BUF_BYTES];
  s = normalize_rel(to_url, dst_norm, sizeof(dst_norm));
  if (s != NORM_OK) {
    write_path_error(c, s);
    return;
  }
  char dst_abs[HTTP_FAT_PATH_BUF_BYTES];
  s = resolve_jail(dst_norm, dst_abs, sizeof(dst_abs));
  if (s != NORM_OK) {
    write_path_error(c, s);
    return;
  }
  if (strcmp(dst_norm, "/") == 0) {
    write_error(c, 409, "Conflict", "conflict",
                "Cannot rename onto root");
    return;
  }
  norm_status_t v = validate_8_3_last(dst_norm);
  if (v != NORM_OK) {
    write_path_error(c, v);
    return;
  }

  // Source must exist as a regular file.
  FILINFO info;
  FRESULT fr = f_stat(src_abs, &info);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    write_error(c, 404, "Not Found", "not_found", "File not found");
    return;
  }
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_stat failed");
    return;
  }
  if (finfo_is_dir(&info)) {
    write_error(c, 404, "Not Found", "is_directory",
                "Path is a directory; use POST /api/v1/folders/<rel>/rename");
    return;
  }

  // No-op rename.
  if (strcmp(src_norm, dst_norm) == 0) {
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"from\":\"%s\",\"to\":\"%s\"}\n", src_norm,
                     dst_norm);
    if (n < 0) n = 0;
    write_response(c, 200, "OK", "application/json", body, (size_t)n);
    return;
  }

  // Target must NOT exist.
  fr = f_stat(dst_abs, &info);
  if (fr == FR_OK) {
    write_error(c, 409, "Conflict", "conflict", "Target already exists");
    return;
  }
  if (fr != FR_NO_FILE && fr != FR_NO_PATH) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_stat failed (target)");
    return;
  }

  fr = f_rename(src_abs, dst_abs);
  if (fr == FR_NO_PATH) {
    write_error(c, 404, "Not Found", "not_found",
                "Target parent does not exist");
    return;
  }
  if (fr == FR_EXIST) {
    write_error(c, 409, "Conflict", "conflict", "Target already exists");
    return;
  }
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_rename failed");
    return;
  }

  char body[400];
  int n = snprintf(body, sizeof(body),
                   "{\"ok\":true,\"from\":\"%s\",\"to\":\"%s\"}\n", src_norm,
                   dst_norm);
  if (n < 0) n = 0;
  write_response(c, 200, "OK", "application/json", body, (size_t)n);
}

// --- Route table ---

#define M_GET (1u << HM_GET)
#define M_HEAD (1u << HM_HEAD)
#define M_POST (1u << HM_POST)
#define M_PUT (1u << HM_PUT)
#define M_DELETE (1u << HM_DELETE)

typedef void (*route_handler_fn)(http_conn_t *c);

typedef struct {
  const char *path;        // exact match
  uint8_t methods_mask;    // bitmask of allowed verbs (M_*)
  route_handler_fn handler;
} route_t;

static const route_t g_routes[] = {
    {"/api/v1/ping", M_GET | M_HEAD, handle_ping},
    {"/api/v1/volume", M_GET | M_HEAD, handle_volume},
    {"/api/v1/files", M_GET | M_HEAD, handle_files_list},
};

#define ROUTES_COUNT (sizeof(g_routes) / sizeof(g_routes[0]))

// Build an "Allow:" header value string from a methods mask.
static void __not_in_flash_func(format_allow)(uint8_t mask, char *out, size_t out_cap) {
  out[0] = '\0';
  size_t off = 0;
  const struct {
    uint8_t bit;
    const char *name;
  } table[] = {
      {M_GET, "GET"},     {M_HEAD, "HEAD"},     {M_POST, "POST"},
      {M_PUT, "PUT"},     {M_DELETE, "DELETE"},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (mask & table[i].bit) {
      int n = snprintf(out + off, out_cap - off, "%s%s",
                       (off == 0) ? "" : ", ", table[i].name);
      if (n < 0 || (size_t)n >= out_cap - off) return;
      off += (size_t)n;
    }
  }
}

// Detect a trailing "/rename" action suffix and (if present) chop it
// off in place by NUL-terminating before the slash. Returns true if
// the action was chopped.
static bool __not_in_flash_func(strip_rename_action)(char *rel) {
  size_t len = strlen(rel);
  static const char suffix[] = "/rename";
  static const size_t suffix_len = sizeof(suffix) - 1;
  if (len > suffix_len && memcmp(rel + len - suffix_len, suffix, suffix_len) == 0) {
    rel[len - suffix_len] = '\0';
    return true;
  }
  return false;
}

static void __not_in_flash_func(route)(http_conn_t *c) {
  // HEAD is treated as GET for matching purposes (the dispatcher
  // suppresses the body during write).
  uint8_t method_bit =
      (uint8_t)(1u << (c->method == HM_HEAD ? (uint8_t)HM_GET
                                            : (uint8_t)c->method));

  // Exact-match routes (read-only endpoints from S1+S2).
  for (size_t i = 0; i < ROUTES_COUNT; i++) {
    if (strcmp(c->path, g_routes[i].path) != 0) continue;
    uint8_t allowed = g_routes[i].methods_mask;
    bool verb_ok = (c->method == HM_HEAD) ? ((allowed & M_GET) != 0)
                                          : ((allowed & method_bit) != 0);
    if (verb_ok) {
      g_routes[i].handler(c);
      return;
    }
    char allow[40];
    format_allow(allowed, allow, sizeof(allow));
    write_405(c, allow);
    return;
  }

  // Prefix routes: /api/v1/folders/<rel> [+ /rename action].
  static const char folders_prefix[] = "/api/v1/folders/";
  static const size_t folders_prefix_len = sizeof(folders_prefix) - 1;
  if (strncmp(c->path, folders_prefix, folders_prefix_len) == 0) {
    char *rel = c->path + folders_prefix_len;
    bool is_rename = strip_rename_action(rel);
    if (rel[0] == '\0') {
      write_error(c, 404, "Not Found", "not_found", "Route not found");
      return;
    }
    if (is_rename) {
      if (c->method == HM_POST) {
        handle_folder_rename(c, rel);
        return;
      }
      write_405(c, "POST");
      return;
    }
    if (c->method == HM_POST) {
      handle_folder_create(c, rel);
      return;
    }
    if (c->method == HM_DELETE) {
      handle_folder_delete(c, rel);
      return;
    }
    write_405(c, "POST, DELETE");
    return;
  }

  // Prefix routes: /api/v1/files/<rel> [+ /rename action]. The
  // exact-match /api/v1/files (no trailing slash) is the listing
  // endpoint and is handled by g_routes above; only the slash-prefixed
  // form falls through here.
  static const char files_prefix[] = "/api/v1/files/";
  static const size_t files_prefix_len = sizeof(files_prefix) - 1;
  if (strncmp(c->path, files_prefix, files_prefix_len) == 0) {
    char *rel = c->path + files_prefix_len;
    bool is_rename = strip_rename_action(rel);
    if (rel[0] == '\0') {
      write_error(c, 404, "Not Found", "not_found", "Route not found");
      return;
    }
    if (is_rename) {
      if (c->method == HM_POST) {
        handle_file_rename(c, rel);
        return;
      }
      write_405(c, "POST");
      return;
    }
    // S5 will add GET (download) and S6 will add PUT (upload). For
    // now, only DELETE is handled here.
    if (c->method == HM_DELETE) {
      handle_file_delete(c, rel);
      return;
    }
    write_405(c, "DELETE");
    return;
  }

  write_error(c, 404, "Not Found", "not_found", "Route not found");
}

// --- Response writers ---

static void __not_in_flash_func(send_buffered)(http_conn_t *c) {
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

// Write a response with optional extra header lines (each must end
// with "\r\n"). Pass NULL for no extras. Used for 201 Created (which
// also emits a Location header) and 405 (which emits Allow:).
static void __not_in_flash_func(write_response_ex)(http_conn_t *c, int status, const char *reason,
                              const char *content_type,
                              const char *extra_headers, const char *body,
                              size_t body_len) {
  char *out = c->resp;
  size_t cap = sizeof(c->resp);
  int n = snprintf(out, cap,
                   "HTTP/1.1 %d %s\r\n"
                   "Server: md-devops/%s\r\n"
                   "Connection: close\r\n"
                   "%s"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "\r\n",
                   status, reason, RELEASE_VERSION,
                   (extra_headers != NULL) ? extra_headers : "", content_type,
                   body_len);
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

static void __not_in_flash_func(write_response)(http_conn_t *c, int status, const char *reason,
                           const char *content_type, const char *body,
                           size_t body_len) {
  write_response_ex(c, status, reason, content_type, NULL, body, body_len);
}

static void __not_in_flash_func(write_error)(http_conn_t *c, int status, const char *reason,
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

static void __not_in_flash_func(write_405)(http_conn_t *c, const char *allow) {
  char body[160];
  int bn = snprintf(body, sizeof(body),
                    "{\"ok\":false,\"code\":\"method_not_allowed\","
                    "\"message\":\"Method not allowed; see Allow header\"}\n");
  if (bn < 0) bn = 0;
  char extra[64];
  int en = snprintf(extra, sizeof(extra), "Allow: %s\r\n", allow);
  if (en < 0 || (size_t)en >= sizeof(extra)) {
    conn_close(c);
    return;
  }
  write_response_ex(c, 405, "Method Not Allowed", "application/json", extra,
                    body, (size_t)bn);
}
