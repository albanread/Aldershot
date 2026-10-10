/* config.h for libsmb2 in BOX (deps/get-libsmb2.sh): musl on Linux, which
 * the HAL (HybrisOS) also runs, and the hosted build on macOS.  Written by
 * hand in place of CMake's ConfigureChecks: no Kerberos, no GSSAPI, the
 * library's own MD4, MD5, SHA and AES. */
#define HAVE_ARPA_INET_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_POLL_H 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_ADDRINFO 1
#define HAVE_STRUCT_IOVEC 1
#define HAVE_LINGER 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_TIME_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ERRNO_H 1
#ifdef __APPLE__
#define HAVE_SOCKADDR_LEN 1
#define HAVE_ARC4RANDOM_BUF 1
#else
#define HAVE_SYS_RANDOM_H 1
#define HAVE_GETRANDOM 1
#endif
#define STDC_HEADERS 1
/* lib/CMakeLists.txt passes these on the command line. */
#define _U_ __attribute__((unused))
