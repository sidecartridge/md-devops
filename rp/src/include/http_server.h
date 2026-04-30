/**
 * File: http_server.h
 * Author: Diego Parrilla Santamaría
 * Date: April 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Custom HTTP/1.1 server for the md-devops Remote
 *              Management API (Epic 02). Built on lwIP's raw TCP API
 *              so it interleaves cleanly with the cartridge-bus poll
 *              loop on Core 0. The lwIP `httpd` app is intentionally
 *              not used — see docs/epics/02-http-api.md.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "pico.h"

#define HTTP_SERVER_PORT 80

// Per-connection request header buffer cap. RAM is at a premium —
// the cartridge ROM mirror lives in the same SRAM bank as the heap,
// so every extra BSS byte shrinks safe heap headroom. 1 KB still
// covers real-world curl headers comfortably.
#define HTTP_HEADER_BUF_BYTES 1024

// Maximum concurrent connections. Two is enough for a single-user
// management API; any more and the static pool starts pushing the
// heap into the ROM-in-RAM region.
#define HTTP_SERVER_MAX_CONNECTIONS 2

/**
 * @brief Bind the listener and start accepting connections.
 *
 * Idempotent — safe to call again after Wi-Fi reconnect. Must be
 * invoked after the cyw43 / lwIP stack is up.
 */
void http_server_init(void);

/**
 * @brief Tear down the listener and close any in-flight connections.
 */
void http_server_deinit(void);

#endif  // HTTP_SERVER_H
