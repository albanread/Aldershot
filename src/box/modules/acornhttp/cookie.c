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
 */
/*
 * HTTP (c.cookie)
 *
 *  © Acorn Computers Ltd. 1996-1998
 *
 * Ported to ROSGD (A7232ToolChain rosgd/modules/acornhttp), 2026: the cookie
 * jar is AcornHTTP's own -- parsing, domains, paths, expiry, the queue of
 * cookies awaiting HTTP_ConsumeCookie, Browse$AcceptAllCookies, the cookie
 * file and its two formats -- unchanged.  Changed: the includes; getenv,
 * which reads RISC OS system variables as the C library's did (http_getenv);
 * the cookie file's opening and closing, through FileSwitch (the RISC OS
 * names of config.c); a cookie handle, a number, since the SWIs' registers
 * are 32 bits and the original's handles were pointers; the SWIs'
 * entries, whose strings come back in the RMA; and three small corrections
 * the compiler asks for (two casts, and a character stored as NULL).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#include "acornhttp.h"
#include "dates.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define COOKIE
#define TRUE  1
#define FALSE 0
#define getenv(name) http_getenv(name)

/* config.h and config.c, as the original has them */
#define CHOICES_DIR		"<Choices$Write>.WWW"
#define DIR_SUFFIX		".AcornHTTP"
#define COOKIE_FILE_FORMAT_ENV "Browse$CookieFileFormat"
#define COOKIE_FILE_NAME	"<Browse$CookieFile>"

typedef struct _Cookie Cookie;          /* h.cookie */
#define module_realloc realloc          /* c.module: RISC OS 3.1's broken realloc */

/* c.generic */
static char *Strdup_ext(const char *s1, const size_t extra)
{
	if (s1 == NULL) return NULL;
	{
		const size_t length = strlen(s1) + 1;
		char *const s2 = malloc(length + extra);
		if (s2 == NULL) return NULL;
		return memcpy(s2, s1, length);
	}
}
enum { consume_cookies_ACCEPT = 1 };
enum { enumerate_cookies_DOMAIN_LIST = 1, enumerate_cookies_SECURE_COOKIE = 1 };

/* A RISC OS file, read whole through FileSwitch, as a stream */
static FILE *riscos_fopen_read(const char *name)
{
	size_t nl = strlen(name) + 1;
	char *b = ros_rma_alloc(nl);
	struct ros_cpu s;
	FILE *f = NULL;
	if (b == NULL) return NULL;
	memcpy(b, name, nl);
	ros_cpu_enter(&s);
	s.r[0] = 17, s.r[1] = ros_addr(b);
	ros_swi(&s, ROS_X_BIT | 0x08);                  /* OS_File 17 */
	if (!s.v && s.r[0] == 1) {
		uint32_t len = s.r[4];
		char *buf = ros_rma_alloc(len + 1);
		if (buf) {
			ros_cpu_enter(&s);
			s.r[0] = 16, s.r[1] = ros_addr(b), s.r[2] = ros_addr(buf), s.r[3] = 0;
			ros_swi(&s, ROS_X_BIT | 0x08);  /* OS_File 16 */
			if (!s.v) {
				char *copy = malloc(len + 1);
				if (copy) {
					memcpy(copy, buf, len);
					copy[len] = 0;
					f = fmemopen(copy, len + 1, "r");
					if (f == NULL) free(copy);
					/* the copy lives as long as the stream: leaked on close, a few K */
				}
			}
			ros_rma_free(buf);
		}
	}
	ros_rma_free(b);
	return f;
}

/* The one stream open for writing: saved through FileSwitch at fclose */
static struct {
	FILE *f;
	char *data;
	size_t size;
	char name[128];
} writing;

static FILE *riscos_fopen_write(const char *name)
{
	writing.data = NULL, writing.size = 0;
	snprintf(writing.name, sizeof writing.name, "%s", name);
	writing.f = open_memstream(&writing.data, &writing.size);
	return writing.f;
}

static void riscos_mkdir(const char *name)
{
	size_t nl = strlen(name) + 1;
	char *b = ros_rma_alloc(nl);
	struct ros_cpu s;
	if (b == NULL) return;
	memcpy(b, name, nl);
	ros_cpu_enter(&s);
	s.r[0] = 8, s.r[1] = ros_addr(b), s.r[4] = 10;
	ros_swi(&s, ROS_X_BIT | 0x08);                  /* OS_File 8 */
	ros_rma_free(b);
}

static int riscos_fclose(FILE *f)
{
	if (f != writing.f || f == NULL) return fclose(f);
	fclose(f);
	writing.f = NULL;
	riscos_mkdir(CHOICES_DIR);
	riscos_mkdir(CHOICES_DIR DIR_SUFFIX);
	{
		size_t nl = strlen(writing.name) + 1;
		char *b = ros_rma_alloc(nl + writing.size + 4);
		if (b) {
			struct ros_cpu s;
			memcpy(b, writing.name, nl);
			memcpy(b + nl, writing.data, writing.size);
			ros_cpu_enter(&s);
			s.r[0] = 10, s.r[1] = ros_addr(b), s.r[2] = 0xFFF;      /* OS_File 10: Text */
			s.r[4] = ros_addr(b + nl), s.r[5] = ros_addr(b + nl + writing.size);
			ros_swi(&s, ROS_X_BIT | 0x08);
			ros_rma_free(b);
		}
	}
	free(writing.data);
	writing.data = NULL;
	return 0;
}

static FILE *config_open_cookies_for_write(void)
{
	return riscos_fopen_write(CHOICES_DIR DIR_SUFFIX ".Cookies");
}

static FILE *config_open_cookies_for_read(void)
{
	return riscos_fopen_read(CHOICES_DIR DIR_SUFFIX ".Cookies");
}

#define fopen(name, mode) riscos_fopen_read(name)
#define fclose(f) riscos_fclose(f)

/* StB 10/10/97 - allow inclusion of cookie code to be a compile time option */
#ifdef COOKIE


/* Defining this will give you a module that passes more of the test suite
 * but that introduces a serious security risk in the module.
 * (RFC2109, section 4.3.2)
 *
 * Note that this is required to let some sites work though so you must
 * define it or loads if sites won't work any more :-(
 */
#define RELAX_DOMAIN_NEEDS_LEADING_DOT



/* Set to 1 if you want cookies stored in Choices:WWW. otherwise !Scrap will be used */
#define SAVE_COOKIES_IN_CHOICES	0


#define DEFAULT_EXPIRY  31536000 /* Set default expiry of a cookie to 1 year */

/* Defined minimums in RFC2109, can always increase it */
#define MAX_COOKIE_SIZE		4096
#define MAX_COOKIES_PER_DOMAIN	20
#define MAX_COOKIES		300
#define MAX_COOKIES_IN_QUEUE	100


/* Arbitary dummy names for host and path when reading in file */
#define NULL_HOST		"noneset"
#define NULL_PATH		"noneset"

/* Tells remove_cookie whether to remove an empty domain or not */
#define NOT_DOMAIN		0
#define DOMAIN_TOO		1


#define ACCEPT_ALL_ENV "Browse$AcceptAllCookies"


/* Define the structures used for storing cookie data */
struct _Cookie {
	char	*name;		/* The NAME attribute of the cookie */
	char	*value;		/* VALUE attribute */
	char	*domain;	/* Domains under which cookie may be used */
	char	*path;		/* Paths under which cookie may be used */
	time_t	expires;	/* Time after which cookie becomes expired */
	int	secure;		/* secure means should be used for transfer if set */
	int	discard;	/* Should be kept in memory _only_ */

	time_t	last_access;	/* last time cookie was sent */
	uint32_t handle;	/* ROSGD: the SWIs' name for it */

	Cookie	*previous;	/* Pointer to last in linked list */
	Cookie 	*next;		/* Pointer to next in linked list */
};

/* Linked list of domains that contain cookies.  It is VITAL that these remain
 * SORTED by domain name (strcmp order) otherwise you'll notice that cookies
 * aren't found when they should be.
 */
typedef struct _CookieDomain {
	char	*domain;		/* Domain name cookie applies to */
	Cookie	*cookie;		/* Linked list of cookies */
	int	cookies;		/* Number of cookies in list */
	struct _CookieDomain *previous;	/* Pointer to last in linked list */
	struct _CookieDomain *next;	/* Pointer to next in linked list */
} CookieDomain;


/* Global definitions */
static CookieDomain *cookie_domain_root;
Cookie *cookie_queue_root;
static int total_cookies = 0, unread_cookies = 0;
static int loading_cookie_file = 0;


typedef enum {
        cookiedomain_INVALID,
        cookiedomain_VALID,
        cookiedomain_ADD_DOT
} cookiedomain_result;


#ifdef TRACE
#ifdef USE_SYSLOG
#include "<syslog$dir>.C-veneer.h.syslog"
#endif
#include <stdarg.h>
#define DEBUG_FILENAME  "RAM:$.HTTPCookie"
static void cookie_debug(const char *format, ...)
{
	#ifdef TRACE
	if (getenv("NoTrace$Cookie")) return; else {
		va_list ap;
		#ifdef USE_SYSLOG
		static char buffer[1024];
		va_start(ap, format);
		vsprintf(buffer, format, ap);
		va_end(ap);
		(void) xsyslog_logmessage("HTTPCookie", buffer, 100);
		#else
		FILE *fp = fopen(DEBUG_FILENAME, "a");

		if (fp != 0) {
			va_start(ap, format);
			(void) vfprintf(fp, format, ap);
			va_end(ap);
			fclose(fp);
		}
		#endif
	}
	#else
	(void) format;
	#endif
}
#endif


/* Reverses a string.  The behaviour of this function is undefined
 * if "start" and "end" are not pointers into the same string on entry.
 */
static void rev_string(char *start, char *end)
{
	while (start < end) {
		char s = *start;
		char e = *end;
		*start++ = e;
		*end-- = s;
	}
}

/* Function to reverse the domain address a.b.c.d to d.c.b.a
 *
 *  StB: This version doesn't need to claim any RMA in order to perform
 *  this in situ domain reversal operation.  Furthermore, it avoids
 *  multiple copy and length finding operations.  Plus, it is simple
 *  to understand :-)
 *
 *  Basically, the string is first byte-reversed to get the domain
 *  components into the correct order, then each domain component
 *  is byte-reversed to re-order the letters back into their
 *  original order.
 *
 *  This routine's behaviour does not involve any undefined behaviour
 *  under any circumstances.  It never generates a pointer which lies
 *  outside the string bounds.
 */
static char *reverse_domain(char *const domain)
{
	char *lastdot = NULL, *dot = domain;

	if (*domain == '\0') return domain;
	rev_string(domain, strchr(domain, '\0') - 1);

	while (*dot) {
		if (*dot == '.') {
			if (lastdot && lastdot != dot) rev_string(lastdot, dot-1);
			lastdot = ++dot;
		}
		else {
			if (lastdot == NULL) lastdot = dot;
			++dot;
		}
	}

	rev_string(lastdot, dot-1);
	return domain;
}

/* This function delinks a cookie from a list of cookies and sorts out
 * all the double-linking fields.  If the item was the at the head of
 * the list, the head is modified to point to the next item (or set to
 * NULL if the list is now empty
 */
static void cookie_unlink_cookie(Cookie **head, Cookie *which)
{
	if (which->next) {
		which->next->previous = which->previous;
	}
	if (which->previous) {
		which->previous->next = which->next;
	}
	if (*head == which) {
		*head = which->next;
	}
}

/* As cookie_unlink_cookie, but works with domains instead of cookies */
static void cookie_unlink_cookie_domain(CookieDomain **head, CookieDomain *which)
{
	if (which->next) {
		which->next->previous = which->previous;
	}
	if (which->previous) {
		which->previous->next = which->next;
	}
	if (*head == which) {
		*head = which->next;
	}
}


/* Function to destroy a cookie and free up all the memory */
static void destroy_cookie (Cookie *cookie)
{
	if (cookie->domain) free(cookie->domain);
	if (cookie->path) free(cookie->path);
	if (cookie->name) free(cookie->name);
	if (cookie->value) free(cookie->value);
	free(cookie);
}


/* Function to remove a cookie from the pending queue */
static void remove_cookie_from_queue(Cookie *cookie)
{
	#ifdef TRACE
	cookie_debug("Entering remove_cookie_from_queue\n");
	cookie_debug("Last access is currently %s", ctime(&cookie->last_access));
	#endif

	unread_cookies --;
	cookie_unlink_cookie(&cookie_queue_root, cookie);

	#ifdef TRACE
	cookie_debug("Leaving remove_cookie_from_queue\n");
	#endif
}


/* Function to add a cookie to the pending queue */
static void add_cookie_to_queue(Cookie *cookie)
{
	/* Enforce an upper limit on the number of cookies that may be held in
	 * the pending queue
	 */
	if (cookie_queue_root != NULL) {
	        Cookie *list;
	        int count = 1;

		for (list=cookie_queue_root; list->next; list=list->next) ++count;
		if (count >= MAX_COOKIES_IN_QUEUE) {
		        if (loading_cookie_file) {
		                /* Periodic flush of the queue when loading our cookie file! */
				move_cookies_from_queue_to_list(0);
		        }
		        else {
                                remove_cookie_from_queue(list);
			        destroy_cookie(list);
		        }
		}
	}

	/* Set to head of list */
	cookie->previous = NULL;
	cookie->next = cookie_queue_root;
	if (cookie_queue_root != NULL) {
		cookie_queue_root->previous = cookie; /* Non empty list */
	}
	cookie_queue_root = cookie;
	/* Set bit 16 in URL status flag to indicate new cookie */
	/* Increase the number of unread cookies by one */
	unread_cookies += 1;
}


/* Function to return if a cookie domain change is allowed by that domain */
/* This function performs a domain-match comparison (RFC2109) */
static cookiedomain_result check_domain_valid(char *host, char *target)
{
	const int target_len = strlen(target);

	#ifdef TRACE
	cookie_debug("Checking %s is valid in domain %s\n", target, host);
	#endif

	if (strcmp(host, target) == 0) {
		return cookiedomain_VALID;
	}

	if (strcmp(host, NULL_HOST) == 0) {
		return cookiedomain_VALID;
	}

	/* New domain must start with a dot, ie end in dot as stored in reverse */
	if (target[target_len-1] != '.') {
	        #ifdef RELAX_DOMAIN_NEEDS_LEADING_DOT
	        if (Strncmp_ci(target, host, target_len) == 0) {
	                char next_char = host[target_len];
	                if (next_char != '.' && next_char != '\0') {
		                #ifdef TRACE
		                cookie_debug("Host is not subdomain of target so failing!\n");
		                #endif
		                return cookiedomain_INVALID;
	                }
	                if (strchr(target+1, '.') != NULL) {
		                return cookiedomain_ADD_DOT;
	                }
	        }
	        #endif
		#ifdef TRACE
		cookie_debug("Target does not match host so failing!\n");
		#endif
		return cookiedomain_INVALID;
	}
	/* Must be embedded dot in new domain */
	else if (strchr(target+1, '.') == target+target_len-1) {
		#ifdef TRACE
		cookie_debug("Does not have an embedded '.' so failing!\n");
		#endif
		return cookiedomain_INVALID;
	}
	/* Check for target (with dot removed) equalling host */
	else if (strlen(host) == (size_t) target_len-1 &&
	                Strncmp_ci(target, host, target_len-1) == 0 ) {
		#ifdef TRACE
		cookie_debug("Success!\n");
		#endif
		return cookiedomain_VALID;
	}
	/* Check new domain is subset of old */
	else if (strstr(host, target) != host) {
		#ifdef TRACE
		cookie_debug("Failed due to not substring!\n");
		#endif
		return cookiedomain_INVALID;
	}
	/* Passed all checks, return success response */
	else {
		#ifdef TRACE
		cookie_debug("Success!\n");
		#endif
		return cookiedomain_VALID;
	}
}

/* Function to return if a cookie path change is allowed by that domain */
static int check_path_valid(char *path, char *target)
{
	#ifdef TRACE
	cookie_debug("Checking path %s is valid in %s\n", target,path);
	#endif

	while (*target == '/') ++target;
	while (*path == '/') ++path;

	if (strcmp(path, target) == 0) return (TRUE);
	if (strcmp(path, NULL_PATH) == 0) return (TRUE);

	/* Only return true if new path is a subset of request path */
	if (strstr(path, target) != NULL) {
		#ifdef TRACE
		cookie_debug("It is valid\n");
		#endif
		return TRUE;
	}
	else {
		#ifdef TRACE
		cookie_debug("It is not valid\n");
		#endif
		return FALSE;
	}
}

/* Function to calculate a new expiry time for a cookie */
/* The delay is an integer given in seconds */
static void calculate_expiry_date(Cookie *cookie, time_t delay)
{
	/* Get number of secs elapsed since 1/1/70, then add delay */
	cookie->expires = time(NULL) + delay;
}

static int cookie_looks_like_day_name(const char *start, const char *term)
{
        if ((term - start) < 3) return 0;

        while (start < term) {
                if (!isalpha(*start) && !isspace(*start) && *start != '=') return 0;
                ++start;
        }

	if (Strncmp_ci(term - 3, "day", 3) == 0) return 1;
	return dates_looks_like_weekday(term - 3);
}

/* Function to read an expiry date string into memory */
static time_t read_new_expiry_date(char *date_string)
{
        time_t result;
        result = dates_string_to_date(date_string);
	#ifdef TRACE
	cookie_debug("Number of secs returned is %d (%s)\n", result, ctime(&result));
	#endif
        return result;
}


#ifdef TRACE
static void write_cookies_to_debug(void)
{
	CookieDomain *print_domain;

	cookie_debug("\nCurrent state of the cookie tree:\n");

	for (print_domain = cookie_domain_root; print_domain; print_domain = print_domain->next) {
		Cookie *print_cookie;

		cookie_debug("Domain \"%s\" ...\n", print_domain->domain);

		for (print_cookie = print_domain->cookie; print_cookie; print_cookie = print_cookie->next) {
			cookie_debug("... cookie %s=%s, domain %s and path %s\n    last access at %s",
				print_cookie->name,print_cookie->value,
				print_cookie->domain, print_cookie->path,
				ctime(&print_cookie->last_access));
			/* ctime returns pointer to static storage - cannot have two parameters as ctime() */
			cookie_debug("    expires at %s\n", ctime(&print_cookie->expires));
		}
	}

	cookie_debug("End of cookie tree\n\n");
}
#endif

static void write_cookies_format_1(FILE *fp)
{
	CookieDomain *cookie_domain;
	Cookie *cookie;
	time_t current_time = time(NULL);

	#ifdef TRACE
	cookie_debug("About to write file format 1\n");
	#endif

	/* Write initial header lines */
	fprintf(fp,"# %s Cookie File\n",Module_Help);
	fprintf(fp,"# Written on GMT %s",ctime(&current_time));
	fprintf(fp,"Format: 1\n");

	cookie_domain = cookie_domain_root;

	/* For each domain... */
	for (cookie_domain = cookie_domain_root; cookie_domain; cookie_domain = cookie_domain->next) {
		/* ... and each cookie in that domain... */
		for (cookie = cookie_domain->cookie; cookie; cookie = cookie->next) {
			#ifdef TRACE
			cookie_debug("Checking cookie %s=%s\n",cookie->name,cookie->value);
			#endif
			/* Check it has not been expired or it is marked 'discard' */
			#ifndef TRACE
			if (cookie->discard == FALSE)
		        #endif
			if (current_time<cookie->expires) {
				#ifdef TRACE
				cookie_debug("This cookie is suitable for saving.  Saving it.\n");
				if (cookie->discard == TRUE) {
				        cookie_debug(">> Normally discard this cookie for being per-session only\n");
				}
				#endif
				/* Write the obligatory information */
				fprintf(fp,"%s=\"%s\"; domain=\"%s\"; path=\"%s\"",
					cookie->name, cookie->value,
					reverse_domain(cookie->domain), cookie->path);
				/* Set the domain back to normal */
				reverse_domain(cookie->domain);
				/* "Secure" is best on the same line */
				if (cookie->secure == TRUE) fprintf(fp,"; Secure");
				/* Fill in the internal details on next line */
				fprintf(fp,"\nExpires=%s", ctime(&(cookie->expires)));
				fprintf(fp,"LastAccess=%s\n", ctime(&(cookie->last_access)));
			}
		}
	}
}

static void write_cookies_format_2(FILE *fp)
{
	CookieDomain *cookie_domain;
	Cookie *cookie;
	time_t current_time = time(NULL);

	#ifdef TRACE
	cookie_debug("About to write file format 2\n");
	#endif

	/* Write initial header lines */
	fprintf(fp,"# %s Cookie File\n",Module_Help);
	fprintf(fp,"# Written on GMT %s",ctime(&current_time));
	fprintf(fp,"Format: 2\n");

	/* For each domain... */
	for (cookie_domain = cookie_domain_root; cookie_domain; cookie_domain = cookie_domain->next) {
		/* ... and each cookie in that domain... */
		for (cookie = cookie_domain->cookie; cookie; cookie = cookie->next) {
			#ifdef TRACE
			cookie_debug("Checking cookie %s=%s\n",cookie->name,cookie->value);
			#endif

			/* Check it has not been expired or it is marked 'discard' */
			#ifndef TRACE
			if (cookie->discard == FALSE)
		        #endif
			if (current_time<cookie->expires && cookie->discard == FALSE) {
				#ifdef TRACE
				cookie_debug("This cookie is suitable for saving.  Saving it.\n");
				if (cookie->discard == TRUE) {
				        cookie_debug(">> Normally discard this cookie for being per-session only\n");
				}
				#endif
				/* Format for each cookie is as follows: */
				/* <name>\t<value>\t<domain>:<portlist>\t<path>\t<expires>\t<last access>\n */
				reverse_domain(cookie->domain);
				fprintf(fp,"%s\t%s\t%s:", cookie->name, cookie->value, cookie->domain);
				/* Set the domain back to normal */
				reverse_domain(cookie->domain);
				/* Write -1 to port list, as support now removed. */
				fprintf(fp,"-1");
				/* Write in the path */
				fprintf(fp,"\t%s\t",cookie->path);
				/* If secure write 'S' else write 's' */
				fputc(cookie->secure == TRUE ? 'S' : 's', fp);
				/* Finally write the expiry and last access times */
				fprintf(fp,"\t%x\t%x\n",(int)cookie->expires,(int)cookie->last_access);
				#ifdef TRACE
				cookie_debug("Expiry date being written to file: (hex) %08x (string) %s\n",
					cookie->expires, ctime(&cookie->expires));
				#endif
			}
		}
	}
}

/* Function called to write cookie file - chooses a file format and writes the file*/
static void write_cookies_to_file(void)
{
	FILE *fp;

	#ifdef TRACE
	cookie_debug("About to write cookies out to external database file\n");
	write_cookies_to_debug();
	#endif

	if ((fp = config_open_cookies_for_write()) == NULL) {
		#ifdef TRACE
		cookie_debug("Failed to open cookie file for writing\n");
		#endif
	}

	if (fp != NULL) {
	        const char *const env = getenv(COOKIE_FILE_FORMAT_ENV);
		if (env != NULL && !strcmp(env, "1")) {
			write_cookies_format_1(fp);
		}
		else {
			write_cookies_format_2(fp);
		}

		fclose(fp);
	}
}

static char *cookie_look_for_cookies(char *domain, char *path, Session *ses)
{
	size_t pointer = 0, max_size = 0;
	size_t num_cookies = 0;
	CookieDomain *cookie_domain;
	Cookie *cookie;
	const time_t current_time = time(NULL);
	char *buffer = NULL;

	/* SNB: This function is now far more efficient.  It avoids allocating huge buffers of RMA unless
	 * it actually finds that it needs one at all.  It does this by setting the buffer pointer to NULL
	 * initially and then relying on the buffer extender to allocate memory as required.  The extender
	 * now uses realloc, so it can take advantage of the property "realloc(NULL,size) == malloc(size)".
	 *
	 * Note that this function always returns NULL and has done ever since the proper header management
	 * code was added to the module.  Cookie headers are added directly to the header list when they
	 * are located.
	 */

	#ifdef TRACE
	cookie_debug("Looking for cookies for domain '%s' path '%s'\n", domain, path);
	#endif

	for (cookie_domain = cookie_domain_root; cookie_domain; cookie_domain = cookie_domain->next) {
		int match_len = strlen(cookie_domain->domain);
		int cmp = strncmp(domain, cookie_domain->domain, match_len);
		/* Check case where cookie domain matches domain with full stop added. */
		if ( cmp && strlen(domain) == (size_t) match_len-1 &&
		     cookie_domain->domain[match_len-1] == '.' &&
		     !strncmp(domain, cookie_domain->domain, match_len-1)
		   ) {
		        cmp = 0;
		}

		/* If cookie domain does not end with full stop, check next
		 * character in domain is a full stop
		 */
		if ( !cmp && cookie_domain->domain[match_len-1] != '.' &&
		     domain[match_len] != '.' && domain[match_len] != '\0' ) {
		        cmp = 1;
		}

		/* Check the domain for this cookie domain; they are sorted; quit loop if we know
		 * that we've definitely not got any more matching domains to save time
		 */
		if (cmp < 0) break;
		/* Does the domain match? No? Continue next case of loop */
		if (cmp > 0) continue;

		#ifdef TRACE
		cookie_debug("Domain '%s' matches\n", cookie_domain->domain);
		#endif

		for (cookie = cookie_domain->cookie; cookie; cookie = cookie->next) {
			char last_char, next_char;
			size_t size;
			/* These are stored in reverse-strcmp path order, so we can apply the
			 * same optimisations as for domains to avoid wasting time.
			 */
			match_len = strlen(cookie->path);
			cmp = strncmp(path, cookie->path, match_len);
			#ifdef TRACE
			cookie_debug("Tested '%s' result %d\n", cookie->path, cmp);
			#endif
			if (cmp > 0) break;
			/* Does the path match?  No?  Continue next case of loop */
			if (cmp < 0) continue;

			/* Must match to end of a directory name */
			next_char = path[match_len];
			last_char = match_len > 0 ? cookie->path[match_len-1] : '\0';
			if ( last_char != '/' && next_char != '/') continue;

			#ifdef TRACE
			cookie_debug("Path '%s' matches\n", cookie->path);
			#endif

			/* Only send if the cookie has not already expired */
			if (cookie->expires < current_time) continue;

			if (cookie->secure && !(ses->flags & flags_USING_HTTPS)) {
			        #ifdef TRACE
			        cookie_debug("Secure cookie `%s' requires secure comms channel\n",
			        	cookie->name);
			        #endif
			        if (!getenv("AcornHTTP$Secure")) continue;
		                #ifdef TRACE
		                cookie_debug("Allowing cookie anyway (AcornHTTP$Secure was set)\n");
		                #endif
			}

			size = strlen(cookie->name) + strlen(cookie->value) + sizeof("; = ");
			if (num_cookies > 0) size += sizeof("; ");

			if ((pointer + size) >= max_size) {
				/* Adding this entry would cause a buffer overflow.
				 * Attempt to extend the buffer (or allocate it on the
				 * first time through!)
				 */
				char *new_buffer;
				/* One cookie can be larger than a step */
				do {
					max_size += MAX_COOKIE_SIZE;
				} while ((pointer + size) >= max_size);
				new_buffer = module_realloc(buffer, max_size);
				if (new_buffer == NULL) {
					if (buffer) {
						/* Discard everything */
						free(buffer);
						buffer = NULL;
					}
					return buffer;
				}
				buffer = new_buffer;
			}

			/* Write the cookie into the output buffer */
                        if (num_cookies > 0) {
				pointer += sprintf(buffer + pointer, "; ");
                        }

			pointer += sprintf(buffer+pointer,"%s=%s",cookie->name,cookie->value);

			cookie->last_access = current_time;
			++num_cookies;
		}
	}

        if (buffer != NULL) {
		#ifdef TRACE
		cookie_debug("Adding cookie header: `%s'\n", buffer);
		#endif
		http_add_header(&ses->headers, "Cookie", buffer);
		free(buffer);
		buffer = NULL;
		pointer = 0;
		max_size = 0;
        }

	return NULL;
}

static char *cookie_process_path(const char *uri)
{
        const size_t length = strlen(uri);
        char *result;

        result = malloc(length + 2);
        if (result == NULL) return result;
        *result = '/';
        memcpy(result + 1, uri, length + 1);
        return result;
}

/* Function to return a buffer containing any cookies, if any, to a given domain/path */
char *send_cookies_to_domain (Session *s)
{
	char *domain, *path;

	domain = Strdup(s->endhost ? s->endhost : s->host);
	path = cookie_process_path(s->url);

	#if 0
        /* Again, RFC2109 forbids this */
	/* Strip the file off the end of the path */
	if (path) {
		char *find_end = strrchr(path, '/');
		if (find_end)
		    *(find_end+1) = '\0';
		}
	}
	#endif

	/* Reverse the domain name for comparisons */
	if (domain) {
		reverse_domain(domain);
	}

	#ifdef TRACE
	cookie_debug("Entering send_cookies_to_domain...domain %s path `%s'\n", domain, path);
	#endif

	/* Return value is ignored nowadays - used to be a buffer that needed to be returned to
	 * caller containing cookies for this session.  Now we pretend that there weren't any
	 * ever because they have already been added in directly.
	 */
	(void) cookie_look_for_cookies(domain, path, s);

	#ifdef TRACE
	cookie_debug("Leaving send_cookies_to_domain\n");
	#endif

	if (domain) free(domain);
	if (path) free(path);

	return NULL;
}
/* Function takes name of a domain and returns a pointer to it in the domain list */
static CookieDomain *find_domain(char *find)
{
	CookieDomain *domain;

	for (domain = cookie_domain_root; domain; domain = domain->next) {
		const int compar = strcmp(find, domain->domain);
		#ifdef TRACE
		cookie_debug("  find_domain:  strcmp(%s, %s)\n", find, domain->domain);
		#endif
		if (compar == 0) return domain;
		if (compar < 0) break;
	}

	return NULL;
}

/* Function to remove a cookie from the cookie tree stored in memory */
/* StB: 15/10/97: This function no longer aborts if passed null current_cookie or
 * the domain cannot be found.
 */
static void remove_cookie_from_list(Cookie *current_cookie, int remove_domain)
{
	CookieDomain *current_domain;
	#ifdef TRACE
	cookie_debug("Removing cookie (handle %p) from the list.\n", current_cookie);
	#endif

	if (!current_cookie) return;

	current_domain = find_domain(current_cookie->domain);
	#ifdef TRACE
	if (!current_domain) cookie_debug("Failed to find cookie's domain!!!\n");
	#endif
	if (current_domain == NULL) return;

	/* Reduce the number of cookies in that domain by one and remove the cookie */
	current_domain->cookies -= 1;
	cookie_unlink_cookie(&current_domain->cookie, current_cookie);
	total_cookies -= 1;

	if (current_domain->cookie == NULL && remove_domain == DOMAIN_TOO) {
		/* Domain has no cookies and we were asked to delete empty domains */
		cookie_unlink_cookie_domain(&cookie_domain_root, current_domain);
		free(current_domain->domain);
		free(current_domain);
	}
}

/* Function to remove least used cookie from linked list */
static void expire_cookies(CookieDomain *cookie_domain)
{
        #ifdef TRACE
        size_t count = 0;
        #endif
	Cookie *cookie, *least_used;

	least_used = cookie_domain->cookie;

	for (cookie = least_used; cookie; cookie = cookie->next) {
	        #ifdef TRACE
	        ++count;
	        #endif
		if (cookie->last_access < least_used->last_access) {
			least_used = cookie;
		}
	}

	#ifdef TRACE
	cookie_debug("expire_cookies: Domain %s, cookie %s\n",cookie_domain->domain, least_used->domain);
	#endif

	#ifdef TRACE
	cookie_debug("expire_cookies: Domain thinks it has %d cookie(s)\n", cookie_domain->cookies);
	cookie_debug("expire_cookies: Our count says it has %d cookie(s)\n", count);
	#endif

	remove_cookie_from_list(least_used, NOT_DOMAIN);
	destroy_cookie(least_used);
}

/* Whether a string holds a control character.  A tab in a cookie's name or
 * value would, once the cookie is saved in the cookie file (tab-separated),
 * read back as further fields: a cookie for any domain the server chose.
 */
static int has_control(const char *s)
{
	for (; s && *s; ++s) {
		if ((unsigned char) *s < ' ' || *s == 0x7f) return 1;
	}
	return 0;
}

/* Function to create a new cookie with default settings, and return a pointer to it */
static Cookie *create_new_cookie(char *host, char *path, char *name,char *value)
{
	/* This calloc is unnecessary - malloc will do provided you are initialising
	 * all the fields in the Cookie structure.  At the moment (15/10/97) this IS
	 * happening
	 */
	Cookie *new_cookie;

	if (has_control(name) || has_control(value)) return NULL;

	new_cookie = calloc(1, sizeof(Cookie));
	if (new_cookie == NULL) {
		#ifdef TRACE
		cookie_debug("Failed to create new Cookie object - calloc failed\n");
		#endif
		return NULL;
	}

	#ifdef TRACE
	cookie_debug("New cookie `%s'  `%s'  `%s'  `%s'\n", name, value, host, path);
	#endif

	/* Initialise all the fields.  I (StB) just set all the fields without checking
	 * for errors since if the memory allocation fails, you just get a NULL pointer
	 * back from Strdup, which is safe.  If memory allocation fails, we are probably
	 * going to be in serious trouble anyway, and it will be a rare condition, hence
	 * the pointers can all be checked at the end after everything has been safely
	 * initialised to avoid any nasty "surprises" */
	new_cookie->name = Strdup(name);
	new_cookie->value = Strdup(value);
	new_cookie->domain = Strdup(host);
	new_cookie->path = Strdup(path);
	calculate_expiry_date(new_cookie, DEFAULT_EXPIRY);
	new_cookie->secure = FALSE;
	new_cookie->discard = TRUE;
	new_cookie->previous = NULL;
	new_cookie->next = NULL;
	new_cookie->last_access = time(NULL);
	{
		static uint32_t next_handle = 0;
		if (++next_handle == 0) ++next_handle;
		new_cookie->handle = next_handle;
	}

	if (new_cookie->name == NULL || new_cookie->value == NULL ||
		new_cookie->domain == NULL || new_cookie->path == NULL) {
		#ifdef TRACE
		cookie_debug("Failed to create the new cookie (due to malloc failure)\n");
		#endif
		destroy_cookie(new_cookie);
		return NULL;
	}

	#ifdef TRACE
	cookie_debug("Just created a new cookie (address %p)\n", new_cookie);
	#endif
	return new_cookie;
}

/* This function removes the least used cookie from the most used domain */
static void cookie_clean(void)
{
	CookieDomain *find_domain, *best_domain = cookie_domain_root;

	for (find_domain = cookie_domain_root; find_domain; find_domain = find_domain->next) {
		if (find_domain->cookies > best_domain->cookies) {
			best_domain = find_domain;
		}
	}

	if (best_domain == NULL || best_domain->cookies == 0) {
		/* Obviously something has gone majorly wrong - attempt to compensate
		 * May help module stability
		 */
		total_cookies = 0;
		return;
	}

	/* Expire the oldest cookie */
	expire_cookies(best_domain);
}

static Cookie *cookie_find_duplicate(CookieDomain *domain, Cookie *match)
{
        Cookie *cookie;

	for (cookie = domain->cookie; cookie; cookie = cookie->next) {
		if (strcmp(cookie->name, match->name) == 0) {
		        if (strcmp(cookie->domain, match->domain) == 0 && strcmp(cookie->path, match->path) == 0) {
		                #ifdef TRACE
		                cookie_debug("duplicate finder: matching cookie\n");
		                cookie_debug("  (%s,%s,%s) == (%s,%s,%s)\n",
			                cookie->name, cookie->domain, cookie->path,
			                match->name, match->domain, match->path);
		                #endif
			        return cookie;
		        }
		}
	}

	return NULL;
}

/* Locate the cookie domain object that will hold cookies for the given "domain".  These
 * are sorted in strcmp order, hence the insertion point can be tracked easily whilst
 * looking for an existing domain object.  If the domain does not exist, it is created
 * and inserted into the domain list and initialised to be empty.  If the domain did already
 * exists, no other action is taken.  A pointer to the cookie domain object is returned
 * by this function, or NULL if it couldn't be created (implying it wasn't found either).
 */
static CookieDomain *cookie_locate_domain_with_create(CookieDomain **head, const char *domain)
{
	CookieDomain *insertion_point = NULL;
	CookieDomain *find_domain;

	for (find_domain = *head; find_domain; find_domain = find_domain->next) {
		const int compar = strcmp(find_domain->domain, domain);

		if (compar < 0) {
			insertion_point = find_domain;
			continue;
		}
		if (compar == 0) {
			insertion_point = find_domain;
		}
		else {
			find_domain = NULL;
		}
		break;
	}

	if (find_domain == NULL) {
		/* We didn't find the domain we were looking for.  Create a new domain */
		#ifdef TRACE
		cookie_debug("locate_domain_with_create: did not find the domain `%s'\n", domain);
		#endif

		find_domain = malloc(sizeof(CookieDomain));

		if (find_domain == NULL) return find_domain;
		find_domain->domain = Strdup(domain);
		if (find_domain->domain == NULL) {
			free(find_domain);
			return NULL;
		}
		find_domain->cookie = NULL;
		find_domain->cookies = 0;

		#ifdef TRACE
		cookie_debug("Inserting new domain (%p) after %p\n", find_domain, insertion_point);
		#endif

		if (insertion_point == NULL) {
			/* Insert at the head of the list */
			find_domain->previous = NULL;
			find_domain->next = *head;
			*head = find_domain;
		}
		else {
			/* Insert after insertion point */
			find_domain->next = insertion_point->next;
			if (insertion_point->next != NULL) {
				insertion_point->next->previous = find_domain;
			}
			find_domain->previous = insertion_point;
			insertion_point->next = find_domain;
		}
	}

	return find_domain;
}

/* This function links a cookie object into the list of cookies for the given
 * cookie domain object.  This list is stored in path reverse strcmp order so as
 * to ensure that when the cookies are extracted for addition to an HTTP request,
 * they automagically come out in the correct order as per RFC2069.  It destroys
 * any duplicates that it finds in favour of the new cookie.
 */
static void cookie_link_to_domain(CookieDomain *head, Cookie *cookie)
{
	Cookie *insertion_point;
	const char *const path = cookie->path;

	while ((insertion_point = cookie_find_duplicate(head, cookie)) != NULL) {
		#ifdef TRACE
		cookie_debug("Deleting duplicate (%p) domain=%s, name=%s, path=%s\n",
			insertion_point, insertion_point->domain, insertion_point->name, insertion_point->path);
		#endif
		remove_cookie_from_list(insertion_point, NOT_DOMAIN);
		destroy_cookie(insertion_point);
	}

	#ifdef TRACE
	{
	        size_t count = 0;
	        Cookie *c;
	        for (c = head->cookie; c; c=c->next) ++count;
	        cookie_debug("Cookie domain (%s) thinks it has %d cookie(s)\n", head->domain, head->cookies);
	        cookie_debug("Cookie domain (%s) really has %d cookie(s)\n", head->domain, count);
	}
	#endif

	head->cookies ++;
	if (head->cookies >= MAX_COOKIES_PER_DOMAIN) {
		expire_cookies(head);
	}

	if (head->cookie == NULL || strcmp(head->cookie->path, path) <= 0) {
		/* Simple case - either is the only cookie or it needs to go first */
		#ifdef TRACE
		cookie_debug("Inserting cookie at head of domain list\n");
		#endif
		cookie->next = head->cookie;
		head->cookie = cookie;
		cookie->previous = NULL;
		if (cookie->next) {
			cookie->next->previous = cookie;
		}
		return;
	}

	#ifdef TRACE
	cookie_debug("Not at the head of the domain list ... searching for insertion point ...\n");
	#endif
	for (insertion_point = head->cookie; insertion_point->next; insertion_point = insertion_point->next) {
		if (strcmp(insertion_point->next->path, path) <= 0) break;
	}

	#ifdef TRACE
	cookie_debug("Inserting after cookie `%s=%s' (path '%s')\n", insertion_point->name, insertion_point->value, insertion_point->path);
	#endif

	cookie->previous = insertion_point;
	cookie->next = insertion_point->next;
	insertion_point->next = cookie;
	if (cookie->next) {
		cookie->next->previous = cookie;
	}
}

/* Function to insert a cookie into the cookie database.
 *
 * Creates a new domain in linked list of domains if necessary.  This function uses
 * several auxiliary functions now instead of having it all in the same function(!)
 * It also takes care of duplicate suppression, and actually guarantees that the
 * cookie data structures are kept in a consistent state at function exit.
 *
 */
static void insert_cookie_into_list (Cookie *cookie)
{
	CookieDomain *find_domain;

	/* Make sure the function does nothing if a NULL pointer is passed in */
	if (cookie == NULL) {
		#ifdef TRACE
		cookie_debug("insert_cookie_into_list was passed a NULL pointer\n");
		#endif
		return;
	}

	#ifdef TRACE
	cookie_debug("About to insert a cookie into the tree\n");
	write_cookies_to_debug();
	#endif

	/* Let's first check if we've reached the cookie limit and remove an old cookie if we have */
	if (total_cookies >= MAX_COOKIES) {
		#ifdef TRACE
		cookie_debug("Expiring oldest cookie - too many in memory currently\n");
		#endif
		cookie_clean();
	}

	/* Find the cookie domain - create it if necessary */
	find_domain = cookie_locate_domain_with_create(&cookie_domain_root, cookie->domain);
	if (find_domain == NULL) {
		#ifdef TRACE
		cookie_debug("insert_cookie_into_list failed to locate/create the CookieDomain for %s\n",
			cookie->domain);
		#endif
		return;
	}

	#ifdef TRACE
	cookie_debug("Linking new cookie into its domain\n");
	#endif
	total_cookies += 1;
	cookie_link_to_domain(find_domain, cookie);

	#ifdef TRACE
	cookie_debug("After insertion of new cookie - dump of cookie tree\n");
	write_cookies_to_debug();
	#endif
}

/* Function to create a new cookie, insert it into the list, then return a pointer to it */
static Cookie *add_new_cookie(char *host, char *path, char *string, char *value)
{
	Cookie *cookie;

	if ((cookie = create_new_cookie(host,path,string,value)) != NULL) {
		#ifdef TRACE
		cookie_debug("Adding new cookie to the queue\n");
		#endif
		add_cookie_to_queue(cookie);
	}

	return cookie;
}

static Cookie *cookie_enum_find_next_from_domain_root(Cookie *cookie)
{
	CookieDomain *find_cookie_domain = cookie_domain_root;
	Cookie *find_cookie = NULL;

	if (cookie != NULL) {
		/* Move on to next cookie */
		find_cookie = cookie->next;
		if (find_cookie == NULL) {
			/* We have exhausted the cookies for this domain.  We need to find the next domain
			 * and then pretend as if this was the first call, but starting from further along
			 * the domain list.
			 */
			CookieDomain *find_domain;

			/* It is efficient to walk back along the domain cookies until we find the head
			 * and then scan down the domain list checking to see if we get a match with the first
			 * cookie in the list.  To completely re-iterate other everything is not efficient.
			 */
			while (cookie->previous) {
				cookie = cookie->previous;
			}
			for (find_domain = cookie_domain_root; find_domain; find_domain = find_domain->next) {
				if (find_domain->cookie == cookie) {
					find_cookie_domain = find_domain->next;
					cookie = NULL;
					break;
				}
			}
		}
	}

	if (cookie == NULL) {
		/* This was the initial call OR we exhausted the previous domain */
		/* find_cookie_domain has already been initialised ready for this search */
		find_cookie = NULL;
		for (; find_cookie_domain; find_cookie_domain = find_cookie_domain->next) {
			find_cookie = find_cookie_domain->cookie;
			if (find_cookie != NULL) break;
		}
	}

	return find_cookie;
}


/* Far simpler - this is a simple list of cookies that we can walk down */
static Cookie *cookie_enum_find_next_from_queue_root(Cookie *cookie)
{
	Cookie *find_cookie = cookie_queue_root;

	if (cookie == NULL) {
		return cookie_queue_root;
	}

	while (find_cookie && find_cookie != cookie) {
		find_cookie = find_cookie->next;
	}

	if (find_cookie == cookie) find_cookie = find_cookie->next;

	return find_cookie;
}

/* Function to pass to browser details of each cookie			*/
/* On entry:								*/
/* R0 = flags (bit 0 set means cookies in list, unset means in queue)	*/
/* R1 = unique cookie handle, 0 for initial call			*/
/* On exit:								*/
/* R0 = flags (bit 0 set means secure channel should be used)		*/
/* R1 = unique cookie handle, 0 for no more cookies			*/
/* R2 = total number of cookies created					*/
/* R3 = number of cookies not read					*/
/* R4 = pointer to domain name string					*/
/* R5 = pointer to NAME string						*/
/* R6 = pointer to VALUE string						*/
/* R7 = pointer to path string						*/
/* ROSGD: a handle's cookie, in the lists or the queue */
static Cookie *cookie_from_handle(uint32_t handle)
{
	CookieDomain *d;
	Cookie *c;
	if (handle == 0) return NULL;
	for (d = cookie_domain_root; d; d = d->next)
		for (c = d->cookie; c; c = c->next)
			if (c->handle == handle) return c;
	for (c = cookie_queue_root; c; c = c->next)
		if (c->handle == handle) return c;
	return NULL;
}

/* ROSGD: the strings EnumerateCookies returns, in the RMA until the next call */
static uint32_t enum_strings;

_kernel_oserror *enumerate_cookies(uint32_t r[8])
{
	int flags		= (int) r[0];
	Cookie *cookie	= cookie_from_handle(r[1]);
	Cookie *find_cookie;

	if (enum_strings) ros_rma_free(ros_ptr(enum_strings));
	enum_strings = 0;

	if (r[1] != 0 && cookie == NULL) {
		find_cookie = NULL;             /* a handle no longer known: the end */
	}
	else if (flags & enumerate_cookies_DOMAIN_LIST) {
		find_cookie = cookie_enum_find_next_from_domain_root(cookie);
	}
	else {
		find_cookie = cookie_enum_find_next_from_queue_root(cookie);
	}

	/* Set common return values including the next cookie handle to be passed back */
	r[0] = 0; /* flags */
	r[1] = find_cookie ? find_cookie->handle : 0;
	r[2] = (uint32_t) total_cookies;
	r[3] = (uint32_t) unread_cookies;

	/* If there is a new cookie to be passed back... */
	if (find_cookie) {
		char *domain = Strdup(find_cookie->domain);
		size_t ld, ln, lv, lp;
		char *b;
		if (find_cookie->secure == TRUE) {
			r[0] |= enumerate_cookies_SECURE_COOKIE;
		}
		if (domain) reverse_domain(domain);
		ld = domain ? strlen(domain) + 1 : 1;
		ln = strlen(find_cookie->name) + 1;
		lv = strlen(find_cookie->value) + 1;
		lp = strlen(find_cookie->path) + 1;
		b = ros_rma_alloc(ld + ln + lv + lp);
		if (b) {
			enum_strings = ros_addr(b);
			memcpy(b, domain ? domain : "", ld);
			memcpy(b + ld, find_cookie->name, ln);
			memcpy(b + ld + ln, find_cookie->value, lv);
			memcpy(b + ld + ln + lv, find_cookie->path, lp);
			r[4] = ros_addr(b);
			r[5] = ros_addr(b + ld);
			r[6] = ros_addr(b + ld + ln);
			r[7] = ros_addr(b + ld + ln + lv);
		}
		else {
			r[4] = r[5] = r[6] = r[7] = 0;
		}
		free(domain);
	}
	else {
		r[4] = r[5] = r[6] = r[7] = 0;
	}

	return NULL;
}

/* Function to recieve from the browser whether a cookie is to be accepted or rejected	*/
/* On entry:										*/
/* R0 - flags, bit zero set for accept or unset for reject				*/
/* R1 - session, forget what this is for						*/
/* R2 - cookie handle									*/
/* No exit defined									*/
_kernel_oserror *consume_cookie(uint32_t r[8])
{
	const int flags = (int) r[0];
	Cookie *find_cookie;

	if (r[2] == 0) {
		/* Bad parameter - discard silently */
		return NULL;
	}

	for (find_cookie = cookie_queue_root; find_cookie; find_cookie = find_cookie->next) {
		if (find_cookie->handle == r[2]) break;
	}

	/* If the cookie exists in the queue... */
	if (find_cookie) {
		remove_cookie_from_queue(find_cookie);
		if (flags & consume_cookies_ACCEPT) {
			/* Insert it into the data structure in memory */
			insert_cookie_into_list (find_cookie);
			write_cookies_to_file();
		}
		else {
			/* Otherwise destroy it */
			destroy_cookie(find_cookie);
		}
	}

	return NULL;
}

/* Function to add a cookie via a SWI call, eg from Javascript				*/
/* On entry:										*/
/* R0 - flags, bit 0 set for secure							*/
/* R1 - name										*/
/* R2 - value										*/
/* R3 - expires										*/
/* R4 - path										*/
/* R5 - domain										*/
/* No exit defined									*/
_kernel_oserror *add_cookie(uint32_t r[8])
{
	int flags = (int) r[0];
	char *name = r[1] ? ros_ptr(r[1]) : NULL;
	char *value = r[2] ? ros_ptr(r[2]) : NULL;
	char *expires = r[3] ? ros_ptr(r[3]) : NULL;
	char *path = r[4] ? ros_ptr(r[4]) : NULL;
	char *domain = r[5] ? ros_ptr(r[5]) : NULL;
	Cookie *new_cookie;

	if (name == NULL || value == NULL || path == NULL || domain == NULL) {
		return make_error(HTTP_BAD_PARAMETER, 0);
	}
	if ((new_cookie = create_new_cookie(domain,path,name,value)) != NULL) {
		if (flags & enumerate_cookies_SECURE_COOKIE) new_cookie->secure = TRUE;
		new_cookie->expires = expires ? read_new_expiry_date (expires) : new_cookie->expires;
		new_cookie->last_access = time(NULL);
		insert_cookie_into_list(new_cookie);
		write_cookies_to_file();
	}

	return NULL;
}

/* ROSGD: the module going: the jar freed */
void cookie_final(void)
{
	while (cookie_queue_root) {
		Cookie *c = cookie_queue_root;
		remove_cookie_from_queue(c);
		destroy_cookie(c);
	}
	while (cookie_domain_root) {
		CookieDomain *d = cookie_domain_root;
		while (d->cookie) {
			Cookie *c = d->cookie;
			d->cookie = c->next;
			destroy_cookie(c);
		}
		cookie_domain_root = d->next;
		free(d->domain);
		free(d);
	}
	total_cookies = unread_cookies = 0;
	if (enum_strings) ros_rma_free(ros_ptr(enum_strings));
	enum_strings = 0;
}

/* StB: copied from ArcWeb's cookie parser.  It is far more thorough that the
 * code in http 0.48 and earlier.  It deals with quoting, whitespace etc.
 *
 * "pcp" is a pointer to a char* which points at the start of the string to
 * parse on entry, and on exit has been updated to point to the start of the
 * attribute name, "val" is a pointer to char* into which is written a pointer
 * to the value of this parameter or NULL if there wasn't one, "pep" is a pointer
 * to char* into which is written the pointer to pass to cookie_get_next_arg to
 * read the next argument and is only valid if the return value of the function
 * is non-zero. "term" is filled in with the terminator character that was found
 * (needed to determine when a new cookie is being read)
 *
 * Function returns 1 if it managed to read something, 0 if it didn't.
 *
 */
static int cookie_get_next_arg(char **pcp, char **val, char **pep, char*term)
{
	char *cp = *pcp, *sc, *ep, *tp, *cm;

	*val = 0;
	*pep = 0;
	if (!*pcp) return 0;
	while (isspace(*cp) && *cp) ++cp; /* strip leading spaces */
	if (!*cp) return 0; /* no more args */
	cm = strchr(cp, ','); /* look for cookie separator */
	sc = strchr(cp, ';'); /* Look for attribute separateor */
	if (cm) {
	        /* Unfortunately, some servers send dates containing commas and don't bother
	         * to quote them.  So we look at the text before the comma to see if it looks
	         * like a day name.  Thanks server vendors.
	         */
		if (sc && cm>sc) cm = 0;
		else if (cookie_looks_like_day_name(cp, cm)) {
			/* Comma appears before ; - need to quickly check unquoted dates */
			cm = strchr(cm+1, ',');
		}
		else sc = cm;
	}
	if (sc) *term=*sc; else *term='\0';
	if (sc) *sc++ = 0;
	*pep = sc;
	ep = strchr(cp, '\0') - 1;
	while (isspace(*ep) && *ep && ep > cp) --ep;
	*++ep = 0; /* trailing spaces snipped (from value) */
	if (ep == cp || strlen(cp) < 4) return 0; /* no more args - corrupt? */
	sc = strchr(cp, '=');
	if (!sc) {
		if (*cp == '\"' && ep[-1] == '\"') {
			/* strip quoted single literal */
			++cp;
			*--ep = 0;
			*pcp = cp;
		}
		return 1;
	}
	*sc = 0; /* delimit it */
	tp = sc - 1;
	while (isspace(*tp) && *tp && tp > cp) --tp;
	/* Following line used to (pre-0.80) incorrectly discarding single-letter cookie names
	 * which is, to say the least, unfortunate for my.yahoo.com users :-/
	 */
	if (isspace(*tp)) return 0; /* nothing? confused :-( */
	if (*tp == '\"' && *cp == '\"' && tp != cp) {
		++cp;
		tp[-1] = 0;
	}
	*pcp = cp;
	tp = sc + 1;
	while (*tp && isspace(*tp)) tp++;
	if (*tp == '\"' && ep[-1] == '\"') {
		/* value was quoted - strip quotes */
		++tp;
		*--ep = 0;
	}
	*val = tp; /* tell caller where the value is stored */
	#if 0
	cookie_debug("Found an arg: `%s' ==> `%s'\n", *pcp, tp);
	#endif
	return 1;
}

/* This function provides a single place were a cookie can be destroyed during its
 * creation by act_on_cookie below.  Saves on useless cope duplication everywhere
 */
static void act_on_cookie_giveup(Cookie **pcookie)
{
	Cookie *const cookie = *pcookie;

	if (cookie != NULL) {
	        #ifdef TRACE
	        cookie_debug("act_on_cookie_giveup: %p\n", cookie);
	        #endif
		remove_cookie_from_queue(cookie);
		destroy_cookie(cookie);
		*pcookie = NULL;
	}
}

/* Function to deal with cookie header once one is found - also called when loading
 * a version 1 cookie file
 */
static void act_on_cookie(char *string, Session *s, char *host, char *path)
{
	static Cookie *current_cookie = NULL;
	static char last_term = '\0';

	char *value, *next;
	char next_term = '\0';

	if (s == NULL) {
	        current_cookie = NULL;
	        last_term = '\0';
	        return;
	}

	#ifdef TRACE
	cookie_debug("act_on_cookie: Now entering function...\n");
	cookie_debug("%s\n", string);
	cookie_debug("act_on_cookie:   Checking host `%s' and path `%s'\n", host, path);
	#endif
	current_cookie = NULL;

	for (; cookie_get_next_arg(&string, &value, &next, &next_term); string = next, last_term = next_term) {

		#ifdef TRACE
		cookie_debug("Found an arg: '%s' => `%s'\n", string, value ? value : "<NONE>");
		cookie_debug("  next = %p, next_term = %d, last_term = %d\n", next, next_term, last_term);
		#endif

		/* An attribute with no value ("Set-Cookie: a=b; domain") has nothing to
		 * set.  Its value is NULL, which the code below must not be given.
		 */
		if (value == NULL && current_cookie &&
		    (Strcmp_ci(string, "domain") == 0 || Strcmp_ci(string, "max-age") == 0 ||
		     Strcmp_ci(string, "path") == 0 || Strcmp_ci(string, "expires") == 0 ||
		     Strcmp_ci(string, "lastaccess") == 0)) {
			continue;
		}

		/* Now fill the name/value pair into the appropriate structure */
		/* First deal with domain field */
		if (Strcmp_ci(string,"domain") == 0 && current_cookie) {
			/* Set new domain */
		        size_t extra = 0;
			reverse_domain(value);
			switch (check_domain_valid(host, value)) {
		                case cookiedomain_ADD_DOT:
		                        extra = 1;
		                	/*FALLTHROUGH*/
			        case cookiedomain_VALID:
					/* Delete default domain */
					if (current_cookie->domain) {
						free(current_cookie->domain);
					}
					/* Write in new domain */
					current_cookie->domain = Strdup_ext(value, extra);
					if (current_cookie->domain == NULL) {
						act_on_cookie_giveup(&current_cookie);
					}
					else if (extra != 0) {
					        strcat(current_cookie->domain, ".");
					}
					break;
				default:
					act_on_cookie_giveup(&current_cookie);
					break;
			}
		}
		else if (Strcmp_ci(string,"max-age") == 0 && current_cookie) {
			/* Set new expiry time */
			calculate_expiry_date(current_cookie, atoi(value));
			current_cookie->discard = FALSE;
		}
		else if (Strcmp_ci(string,"path") == 0 && current_cookie) {
			/* Set new path */
		        #ifdef TRACE
		        cookie_debug("Path override to `%s'\n", value);
		        #endif
			if (check_path_valid(path,value)) {
				/* Delete default path */
				if (current_cookie->path != NULL) {
					free(current_cookie->path);
				}
				/* Fill in new path */
				current_cookie->path = Strdup(value);
				if (current_cookie->path == NULL) {
					act_on_cookie_giveup(&current_cookie);
				}
			}
			else {
				act_on_cookie_giveup(&current_cookie);
			}
		}
		else if (Strcmp_ci(string,"expires") == 0 && current_cookie) {
			current_cookie->expires = read_new_expiry_date(value);
			current_cookie->discard = FALSE;

			#ifdef TRACE
			cookie_debug("Set new expiry date to %s\n", ctime(&(current_cookie->expires)));
			#endif
		}
		else if (Strcmp_ci(string,"lastaccess") == 0 && current_cookie) {
			current_cookie->last_access = read_new_expiry_date (value);

			#ifdef TRACE
			cookie_debug("Set new last access to %s\n", ctime(&(current_cookie->last_access)));
			#endif
		}
		else if (last_term == '\0' || last_term == ',' || last_term == '\n') {
			/* Not a parameter? It must be a new name! */
			#ifdef TRACE
			cookie_debug("About to add new cookie with domain %s, path %s, %s=%s\n", host,path,string,value);
			#endif
			current_cookie = add_new_cookie(host, path, string, value);
		}
	}

	last_term = next_term;

	#ifdef TRACE
	cookie_debug("act_on_cookie: Now leaving function.\n");
	#endif
}

/* Function to transfer all cookies from temp queue into main list */
void move_cookies_from_queue_to_list(int check_variable)
{
	if (cookie_queue_root == NULL) return;

	if (check_variable) {
		/* If env is set and equal to 'on' then just accept all cookies unconditionally.
		 * As of version 0.50, this code no longer cmp's the contents of zero page with "off"
	 	 * when the environment variable does not exist.
	 	 */
		const char *const env = getenv(ACCEPT_ALL_ENV);
		if (env == NULL || strcmp(env, "off") == 0) return;
	}

	while (cookie_queue_root) {
		Cookie *const cookie = cookie_queue_root;
		#ifdef TRACE
		cookie_debug("Moving %s=%s from queue into list\n",cookie->name,cookie->value);
		#endif
		remove_cookie_from_queue(cookie);
		insert_cookie_into_list(cookie);
	}

	if (!loading_cookie_file) {
		write_cookies_to_file();
	}
}

/* StB added this function to centralise the data file reading activities.  This one works.
 * It returns a pointer to the line of data if it managed to read a line, NULL on EOF or
 * other error.  It strips comments, leading & trailing whitespace and blank lines.  Depending
 * on the fourth parameter, # needs to be the first non-whitespace character on the line if this
 * flag is TRUE, otherwise it can appear anywhere on the line.
 */
static char *readlines_raw_do(char *const buffer, int size, FILE *f, int only_first_char_comments)
{
	char *result = fgets(buffer, size, f);
	if (result) {
		char *buf = strchr(buffer, '\n');
		if (buf) *buf = 0; /* Strip the end-of-line character */

		while (*result && isspace(*result)) ++result; /* Strip leading whitespace */

		if (only_first_char_comments) {
			buf = (result[0] == '#') ? result : NULL;
		}
		else {
			buf = strchr(result, '#');
		}

		if (buf) {
			*buf = 0;
		}

		/* Strip trailing whitespace */
		for (buf = strchr(result, '\0'); buf > result;) {
			--buf;
			if (!isspace(*buf)) {
				buf[1] = '\0';
				break;
			}
		}
		if (!*result) {
			/* Line was blank - get another one (possibly after removing comments and stuff) */
			return readlines_raw_do(buffer, size, f, only_first_char_comments);
		}
	}

	return result;
}


static size_t cookie_tokenise(char *buffer, char *fields[], size_t max_fields)
{
	size_t i;

	/* For safety's sake */
	for (i = 0; i<max_fields; ++i) {
		fields[i] = NULL;
	}

	/* Tokenise string - return number of valid tokens found */
	for (i=0; i<max_fields; ++i) {
		fields[i] = buffer;
		while (*buffer != '\t' && *buffer != '\0') ++buffer;
		if (*buffer == '\0') return i+1;
		*buffer++ = '\0';
	}

	return i;
}

/* For only_first_char_comment - so we can change it easily */
#define comment_POLICY 1
#define GETLINE() (readlines_raw_do(_buffer, MAX_COOKIE_SIZE, fp, comment_POLICY))

typedef enum {
	token_NAME, token_VALUE, token_DOMAIN, token_PATH,
	token_SECURE, token_EXPIRES, token_LAST_ACCESS,

	token_MAX_TOKEN_COUNT
} cookie_tokens;

static void read_cookie_data_from_file(FILE *fp, char *_buffer)
{
	char *buffer = GETLINE();

	if (!buffer) return;

	/* First non-comment line *must* be the format type */
	if (strncmp(buffer,"Format:",7) != 0) {
		#ifdef TRACE
		cookie_debug("Malformed 'Format' line: Line found is %s\n", buffer);
		#endif
		return;
	}

	if (atoi(buffer+8) == 1) {
		while ((buffer = GETLINE()) != NULL) {
			act_on_cookie(buffer, NULL, NULL_HOST, NULL_PATH); /* Parse it */
		}
	}
	else if (atoi(buffer+8) == 2) {
		/* Deal with format 2 type files */
		#ifdef TRACE
		cookie_debug("Correct format (2) in place\n");
		#endif
		while ((buffer = GETLINE()) != NULL) {
			Cookie *cookie;
			char *tokens[token_MAX_TOKEN_COUNT];
			char *portlist;
			size_t token_count = cookie_tokenise(buffer, tokens, token_MAX_TOKEN_COUNT);

			if (token_count < token_MAX_TOKEN_COUNT) {
				#ifdef TRACE
				cookie_debug("Cookie tokeniser only found %d fields - wanted %d\n",
					token_count, token_MAX_TOKEN_COUNT);
				#endif
				continue;
			}

			portlist = strchr(tokens[token_DOMAIN], ':');
			if (portlist != NULL) {
				*portlist++ = '\0';
			}

			/* Don't forget that we hold the domain in reverse internally! */
			reverse_domain(tokens[token_DOMAIN]);
			#ifdef TRACE
			cookie_debug("Internal domain `%s'\n", tokens[token_DOMAIN]);
			#endif

			/* Make the cookie */
			cookie = create_new_cookie(tokens[token_DOMAIN], tokens[token_PATH],
				tokens[token_NAME], tokens[token_VALUE]);

			/* Check that the cookie was successfully created */
			if (!cookie) continue;

			cookie->discard = FALSE;

			if ((tokens[token_SECURE])[0] == 'S') cookie->secure = TRUE; /* Default false */
			cookie->expires = (int) strtol(tokens[token_EXPIRES], NULL, 16);
			#ifdef TRACE
			cookie_debug("Setting expiry date on new cookie to %s", ctime(&cookie->expires));
			#endif
			cookie->last_access = (int) strtol(tokens[token_LAST_ACCESS], NULL, 16);
			add_cookie_to_queue(cookie);
		}
	}
}

/* Function to read a cookie file into memory
 * This function performs the basic housekeeping (particularly of the buffer
 * which is now allocated dynamically instead of taking 4K of the SVC stack!
 *
 * The functions which are called by this loader are now very resilient and
 * should be able to filter out any rubbish without any trouble at all.
 */
void read_cookie_file(void)
{
	char *buffer;
	FILE *fp;

	#ifdef TRACE
	cookie_debug("Entering read_cookie_file...\n");
	#endif

	cookie_domain_root = NULL;
	cookie_queue_root = NULL;
	total_cookies = 0;
	unread_cookies = 0;
	loading_cookie_file = 1;
	act_on_cookie(0, 0, 0, 0); /* Trapped specially to init static data */

	/* Open the file to be read, and get the format type of file */
	if ((fp = config_open_cookies_for_read()) == NULL) {
		#ifdef TRACE
		cookie_debug("Unable to open cookies from Choices\n");
		#endif
		if ((fp = fopen(COOKIE_FILE_NAME, "r")) == NULL) {
			#ifdef TRACE
			cookie_debug("Unable to open cookies from " COOKIE_FILE_NAME "\n");
			#endif
			loading_cookie_file = 0;
			return;
		}
	}

	buffer = malloc(MAX_COOKIE_SIZE);
	if (buffer != NULL) {
		read_cookie_data_from_file(fp, buffer);
		free(buffer);
	}

	fclose(fp);
	move_cookies_from_queue_to_list (0);
	loading_cookie_file = 0;
	#ifdef TRACE
	cookie_debug("Leaving read_cookie_file.\n");
	#endif
}


void cookie_set_cookie(char *start_field, Session *s)
{
	char *host, *path;

	#ifdef TRACE
	cookie_debug("Found a new cookie - processing it\n");
	#endif

	/* Set default host and path names */
	if (s->endhost) {
	        host = Strdup(s->endhost);
	}
	else {
                host = Strdup(s->host);
	}
	path = cookie_process_path(s->uri);
	/* Internally all the domain names are stored in reverse */
	reverse_domain(host);

	/* Strip the file off the end of the path */
        {
	        /* RFC2109.  Section 4.3.1 */
		char *find_end = strrchr(path, '/');
		if (find_end != NULL) {
			*find_end = '\0';
		}
        }

	/* Strip spurious trailing data from the header */
	{
	        char *find_end = strchr(start_field, '\0');
	        if (find_end != start_field) find_end--;
	        while (find_end != start_field && (isspace(*find_end) || strchr(";, \t", *find_end))) find_end--;
	        *(find_end+1) = '\0';
	}

	act_on_cookie(start_field, s, host, path);

	if (host) {
		free(host);
	}
	if (path) {
		free(path);
	}
}


#else /* COOKIE */
/* All these functions deny the existence of any cookies - easier to
 * have them here as empty functions rather than compile the entire
 * file out otherwise you start getting warnings from the compiler
 * about lack of objects with external linkage in the AOF file.
 */
void read_cookie_file(void) { }
char *send_cookies_to_domain(Session *ses) { (void) ses; return 0; }
#endif
