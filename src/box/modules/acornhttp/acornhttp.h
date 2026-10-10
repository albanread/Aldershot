/* Copyright 1998 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation of RISC OS Open's AcornHTTP
 * (Sources/Networking/Fetchers/HTTP).
 */

/* acornhttp.h -- AcornHTTP over libcurl (acornhttp.c), and what its cookie
 * jar (cookie.c, ported from RISC OS's AcornHTTP) shares with it.
 */
#ifndef ROSGD_ACORNHTTP_H
#define ROSGD_ACORNHTTP_H

#include <stddef.h>
#include <stdint.h>

#include "rosgd/error.h"

/* The module, for the ROM's list of native modules. */
extern struct ros_module acornhttp_module;

typedef os_error _kernel_oserror;

#define Module_Help "Acorn_HTTP"
#define Module_VersionString "1.09"

/* HTTP_GetData's flags (h.module) */
enum {
    flags_USER_AGENT_IN_R6 = 1,
    flags_DATA_LENGTH_IN_R5 = 2,
    flags_USING_HTTPS = 1 << 29,
    flags_NO_COOKIES = 1 << 30,
    flags_PROXY = (int)(1u << 31),
};

/* A header list, in order, duplicates allowed (c.header) */
typedef struct http_header http_header;
struct http_header {
    http_header *next;
    char *header;
    char *value;
};
http_header *http_add_header(http_header **list, const char *header, const char *value);
http_header *http_find_header(http_header *list, const char *header);
void http_delete_header(http_header **list, http_header *which);
void http_free_headers(http_header **list);

/* What the cookie jar reads of a fetch: the host (and the proxied one), the
 * URL and its path, the flags, and the request's headers, to which it adds
 * Cookie */
typedef struct Session {
    char *host, *endhost;
    char *url, *uri;
    int flags;
    http_header *headers;
} Session;

/* From acornhttp.c: named as the original's, each module its own */
#define make_error http_make_error
#define Strdup http_Strdup
#define Strcmp_ci http_Strcmp_ci
#define Strncmp_ci http_Strncmp_ci
_kernel_oserror *make_error(int which, int unused);
char *Strdup(const char *s);
int Strcmp_ci(const char *a, const char *b);
int Strncmp_ci(const char *a, const char *b, size_t n);
char *http_getenv(const char *name);            /* a system variable, GSTrans'd; NULL if unset */

/* The cookie jar (cookie.c) */
void read_cookie_file(void);
char *send_cookies_to_domain(Session *s);
void cookie_set_cookie(char *cookie, Session *s);
void move_cookies_from_queue_to_list(int check_variable);
_kernel_oserror *enumerate_cookies(uint32_t r[8]);
_kernel_oserror *consume_cookie(uint32_t r[8]);
_kernel_oserror *add_cookie(uint32_t r[8]);
void cookie_final(void);

/* AcornHTTP's errors, &80DE20 + n */
enum { HTTP_HOST_NOT_FOUND, HTTP_HOST_CONNECT_ERROR, HTTP_DATA_READ_ERROR, HTTP_GENERAL_ERROR,
       HTTP_BAD_SESSION_ERROR, HTTP_CONNECTION_FAILED, HTTP_METHOD_UNSUPPORTED, HTTP_METHOD_INIT_ERR,
       HTTP_BAD_URL_PARSE, HTTP_PROXY_NOT_FOUND, HTTP_NO_SECURITY, HTTP_BAD_PARAMETER, HTTP_NO_RESOURCES };

#endif
