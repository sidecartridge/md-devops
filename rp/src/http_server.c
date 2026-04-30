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
// line + headers + the largest non-streaming body we emit (the
// directory listing JSON). At ~80 bytes per entry, 8 KB lets us emit
// ~95 entries before tripping the truncated flag — enough for any
// realistic SD card folder. True streaming for multi-MB downloads
// arrives in S5.
#define HTTP_RESPONSE_BUF_BYTES 8192
#define HTTP_PATH_BUF_BYTES 256
#define HTTP_QUERY_BUF_BYTES 256

// Listing cap. Spec says 1000, but the response buffer is the real
// constraint right now — whichever cap we hit first sets truncated.
#define HTTP_LISTING_MAX_ENTRIES 1000

// FatFs LFN buffer cap. Match FF_MAX_LFN from ffconf.h (255).
#define HTTP_FAT_PATH_BUF_BYTES 256

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

// --- URL decoding + query parsing ---

static int hex_digit(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decode percent-encoding in `src` into `dst`. Treats '+' as literal
// (we don't accept form-encoded query strings as space-encoded).
// Returns false on bad encoding, NUL byte in result, or buffer
// overflow.
static bool url_decode(const char *src, char *dst, size_t dst_cap) {
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
static bool query_get(const char *query, const char *key, char *out,
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
static const char *get_gemdrive_folder(void) {
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

static norm_status_t normalize_rel(const char *rel, char *out,
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
      memcpy(out + out_len, rel + seg_start, seg_len);
      out_len += seg_len;
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
static norm_status_t resolve_jail(const char *rel, char *out,
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

// FAT date+time → ISO-8601 "YYYY-MM-DDTHH:MM:SS". Returns false if
// the date field is zero (FatFs uses 0 for "no date set") so the
// caller can emit `null`.
static bool fat_to_iso8601(WORD fdate, WORD ftime, char *out,
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

static void handle_ping(http_conn_t *c) {
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

static const char *fat_type_name(BYTE fs_type) {
  switch (fs_type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "EXFAT";
    default: return "UNKNOWN";
  }
}

static void handle_volume(http_conn_t *c) {
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

static bool body_appendf(char *body, size_t cap, size_t *len,
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

static void handle_files_list(http_conn_t *c) {
  // Extract ?path=, default to "/".
  char rel[HTTP_PATH_BUF_BYTES];
  rel[0] = '\0';
  if (c->query[0] != '\0') {
    if (!query_get(c->query, "path", rel, sizeof(rel))) {
      // Either the key is missing (ok, default to "/") or the value
      // failed to decode (bad_path). query_get returns false in both
      // cases; disambiguate by looking for "path=" literal.
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

  // Stat the path: must exist and be a directory.
  FILINFO info;
  FRESULT fr = f_stat(abs_path, &info);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
    // f_stat on the GEMDRIVE_FOLDER root returns FR_NO_FILE because
    // root has no parent entry to stat — that's expected and means the
    // root exists.
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

  // Build response into resp[]; reserve room for status + headers, fill
  // body in a temporary buffer, then assemble.
  // Worst-case headers + envelope ≈ 256 bytes; leave the rest for body.
  char body[HTTP_RESPONSE_BUF_BYTES - 384];
  size_t body_len = 0;
  bool truncated = false;

  // Recompute the canonical (normalised, root-stripped) path to echo.
  char norm[HTTP_PATH_BUF_BYTES];
  if (normalize_rel(rel, norm, sizeof(norm)) != NORM_OK) {
    norm[0] = '/';
    norm[1] = '\0';
  }

  if (!body_appendf(body, sizeof(body), &body_len,
                    "{\"ok\":true,\"path\":\"%s\",\"entries\":[", norm)) {
    write_error(c, 500, "Internal Server Error", "internal_error",
                "Response buffer overflow");
    return;
  }

  DIR dir;
  fr = f_opendir(&dir, abs_path);
  if (fr != FR_OK) {
    write_error(c, 500, "Internal Server Error", "disk_error",
                "f_opendir failed");
    return;
  }

  uint16_t entry_count = 0;
  bool first = true;
  while (entry_count < HTTP_LISTING_MAX_ENTRIES) {
    FILINFO finfo;
    fr = f_readdir(&dir, &finfo);
    if (fr != FR_OK || finfo.fname[0] == '\0') break;

    char mtime[24];
    bool has_mtime = fat_to_iso8601(finfo.fdate, finfo.ftime, mtime,
                                    sizeof(mtime));
    bool is_dir = (finfo.fattrib & AM_DIR) != 0;
    uint32_t size = is_dir ? 0u : (uint32_t)finfo.fsize;

    // Save state in case the entry overflows the buffer; we want to
    // emit truncated:true and not leave a dangling comma.
    size_t saved_len = body_len;
    bool ok = true;
    if (!first) {
      ok = body_appendf(body, sizeof(body), &body_len, ",");
    }
    if (ok) {
      if (has_mtime) {
        ok = body_appendf(body, sizeof(body), &body_len,
                          "{\"name\":\"%s\",\"size\":%lu,\"is_dir\":%s,"
                          "\"mtime\":\"%s\"}",
                          finfo.fname, (unsigned long)size,
                          is_dir ? "true" : "false", mtime);
      } else {
        ok = body_appendf(body, sizeof(body), &body_len,
                          "{\"name\":\"%s\",\"size\":%lu,\"is_dir\":%s,"
                          "\"mtime\":null}",
                          finfo.fname, (unsigned long)size,
                          is_dir ? "true" : "false");
      }
    }
    if (!ok) {
      body_len = saved_len;
      truncated = true;
      break;
    }
    first = false;
    entry_count++;
  }
  f_closedir(&dir);
  if (entry_count >= HTTP_LISTING_MAX_ENTRIES) truncated = true;

  if (!body_appendf(body, sizeof(body), &body_len,
                    "],\"truncated\":%s}\n",
                    truncated ? "true" : "false")) {
    // Trailer didn't fit — last-resort: cut some entries to make room.
    // Worst case scenario; acknowledge by returning 500.
    write_error(c, 500, "Internal Server Error", "internal_error",
                "Response buffer overflow");
    return;
  }

  write_response(c, 200, "OK", "application/json", body, body_len);
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
static void format_allow(uint8_t mask, char *out, size_t out_cap) {
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

static void route(http_conn_t *c) {
  // HEAD is treated as GET for matching purposes (the dispatcher
  // suppresses the body during write).
  uint8_t method_bit =
      (uint8_t)(1u << (c->method == HM_HEAD ? (uint8_t)HM_GET
                                            : (uint8_t)c->method));

  for (size_t i = 0; i < ROUTES_COUNT; i++) {
    if (strcmp(c->path, g_routes[i].path) != 0) continue;
    // Path matched. Check verb. HEAD is allowed wherever GET is.
    uint8_t allowed = g_routes[i].methods_mask;
    bool verb_ok = false;
    if (c->method == HM_HEAD) {
      verb_ok = (allowed & M_GET) != 0;
    } else {
      verb_ok = (allowed & method_bit) != 0;
    }
    if (verb_ok) {
      g_routes[i].handler(c);
      return;
    }
    char allow[40];
    format_allow(allowed, allow, sizeof(allow));
    write_405(c, allow);
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
