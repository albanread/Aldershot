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
 * This file is a reimplementation of RISC OS Open's URL fetcher
 * (Sources/Networking/Fetchers/URL).
 */

/* parseurl.h -- URL_ParseURL's resolver (parseurl.c, ported from RISC OS's
 * URL module), and what it needs from the rest of URL_Fetcher.
 *
 * The URL union is the original's (h.URLstruct): host pointers and sizes,
 * which parseurl.c's SWI entry converts to and from the client's block of
 * 32-bit words.
 */
#ifndef ROSGD_PARSEURL_H
#define ROSGD_PARSEURL_H

#include <stddef.h>
#include <stdint.h>

#include "rosgd/error.h"

typedef os_error _kernel_oserror;

struct URL_ptrs {
    char *full, *scheme, *host, *port, *user, *password, *account, *path, *query, *fragment, *minimal;
};
struct URL_lengths {
    size_t full, scheme, host, port, user, password, account, path, query, fragment, minimal;
};
typedef union URL {
    struct URL_lengths lengths;
    size_t len[sizeof(struct URL_ptrs) / sizeof(size_t)];
    struct URL_ptrs data;
    char *field[sizeof(struct URL_ptrs) / sizeof(size_t)];
} URL;

enum { url_parseurl_buffer_lengths, url_parseurl_return_data, url_parseurl_compose_from_components,
       url_parseurl_quick_resolve };

enum { parseurlflags_FIELDCOUNT_IN_R5 = 1, parseurlflags_APPLY_HEX_ENCODE = 2 };

/* A protocol's flags word (URL_ProtocolRegister's R5) */
enum { proto_PATH_NOT_UNIX = 1, proto_DOES_NOT_PARSE = 2, proto_HAS_USER = 4, proto_HOST_ALLOW_HASH = 8,
       proto_HAS_NO_NETLOC = 16, proto_STRIP_DOT_DOT = 32 };

/* URL_Fetcher's errors, &80DE00 + n */
enum { url_ERROR_CLIENT_ID_NOT_FOUND, url_ERROR_MEMORY_EXHAUSTED, url_ERROR_NO_FETCHER_SERVICE,
       url_ERROR_SWI_NOT_FOUND, url_ERROR_ALREADY_CONNECTED, url_ERROR_NOT_CONNECTED, url_ERROR_PROTOCOL_EXISTS,
       url_ERROR_NOT_IN_PROGRESS, url_ERROR_MESSAGE_NOT_FOUND, url_ERROR_SESSION_INACTIVE, url_ERROR_NO_PARSE_URL };

/* From urlfetcher.c: named as the original's, each module its own */
#define make_error url_make_error
#define Strdup url_Strdup
_kernel_oserror *make_error(int which, int unused);
char *Strdup(const char *s);
int protocol_get_flags(const char *url);
unsigned int protocol_get_default_port(const char *url);

/* URL_ParseURL, R0-R5 the SWI's registers (32-bit words, pointers arena
 * addresses); and resolution for URL_GetURL.  A malloc'ed string, or NULL. */
_kernel_oserror *parse_url(uint32_t r[6]);
char *URL_canonicalise(int flags, const char *url);
char *URL_resolve(int flags, const char *url, const char *rel);

#endif
