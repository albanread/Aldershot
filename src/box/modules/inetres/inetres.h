/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* inetres.h -- the !Internet commands (InetRes), shared between their files.
 *
 * RISC OS keeps these as C programs in !Internet.bin, FreeBSD's tools
 * ported: Ping, TraceRoute, ARP, IfConfig, Route, InetStat, SysCtl and the
 * rest. ROSGD has no ARM programs to run, and Linux is the stack, so they
 * are commands of a native module answering from Linux (inetres.c). */
#ifndef INETRES_H
#define INETRES_H

#include <stddef.h>
#include <stdint.h>

#include "rosgd/error.h"
#include "rosgd/module.h"

extern struct ros_module inetres_module;

/* A command's words, as the C library's argv: argv[0] is the command. */
#define INET_MAXARGS 32
struct inet_args {
    int argc;
    char *argv[INET_MAXARGS];
    char buf[1024];
};

/* getopt over a command's words: the option letter, '?' for an unknown one
 * (reported), -1 at the first operand.  optarg for options taking one. */
struct inet_opt {
    int ind;                        /* the next word */
    const char *arg;                /* the current option's argument */
    const char *next;               /* inside a word of several letters */
};
int inet_getopt(struct inet_opt *o, const struct inet_args *a, const char *spec);

/* Text to the VDU stream, "\n" as OS_NewLine: *SPOOL and redirection see
 * it, as a program's output. */
os_error *inet_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* A tool's failure, as FreeBSD's err() and errx() print it ("ping: ..."),
 * and the program's exit: Sys$ReturnCode. With -e (IfConfig, Route, SysCtl,
 * ARP, IPVars) the text goes to Inet$Error instead of the screen, which
 * !Internet's Startup tests with CheckError. */
struct inet_tool {
    const char *name;               /* "ping" */
    int e_flag;
};
os_error *inet_fail(const struct inet_tool *t, int code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void inet_exit(int code);
void inet_set_var(const char *name, const char *value);  /* a string variable */           /* Sys$ReturnCode, as the C library's exit */

/* Whether Escape has been pressed, acknowledged if so. */
int inet_escape(void);
/* The error UpCall_Sleep gives when Escape ends a task window's sleep
 * (ros_sleep_fd): taken as Escape pressed */
#define INET_ERR_ESCAPE 0x11u

/* RISC OS interface names over Linux's. lo0 is lo. A name that Linux does
 * not know and that ends in a digit N (ej0, eg0, ec0: EtherUSB,
 * EtherGENET, ...) is the Nth Ethernet interface Linux has. Linux's own
 * names work as they are. The Linux name goes in out. The result is 0, or
 * -1 if there is no such interface. */
int inet_linux_ifname(const char *riscos, char out[16]);
/* And the name RISC OS shows for a Linux one: lo is lo0. */
void inet_riscos_ifname(const char *linux_name, char out[16]);

/* A RISC OS file, through FileSwitch (files.c): its handle, and a buffer in
 * the RMA the SWIs see.  Reads and writes return the bytes moved, or -1. */
struct inet_file {
    uint32_t handle;
    uint8_t *buf;
};
os_error *inet_fopen(struct inet_file *f, const char *name, int write);
long inet_fread(struct inet_file *f, void *buf, size_t n);
long inet_fwrite(struct inet_file *f, const void *buf, size_t n);
long inet_ftell(struct inet_file *f);
int inet_fseek(struct inet_file *f, long pos);
os_error *inet_fclose(struct inet_file *f);
long inet_fsize(const char *name);          /* -1 if not a file */

/* A line from the input stream (OS_ReadLine): its length, -1 on Escape */
int inet_readline(char *buf, size_t max);

/* The commands (one file each). */
os_error *inet_ping(const struct inet_args *a);
os_error *inet_traceroute(const struct inet_args *a);
os_error *inet_arp(const struct inet_args *a);
os_error *inet_ifconfig(const struct inet_args *a);
os_error *inet_route(const struct inet_args *a);
os_error *inet_inetstat(const struct inet_args *a);
os_error *inet_sysctl(const struct inet_args *a);
os_error *inet_ipvars(const struct inet_args *a);
os_error *inet_gethost(const struct inet_args *a);
os_error *inet_ifrconfig(const struct inet_args *a);
os_error *inet_showstat(const struct inet_args *a);
os_error *inet_md5(const struct inet_args *a);         /* and every digest: argv[0] names it */
os_error *inet_tftp(const struct inet_args *a);
os_error *inet_checkmem(const struct inet_args *a);   /* the Startup utilities: utils.c */
os_error *inet_readcmosip(const struct inet_args *a);
os_error *inet_triggercbs(const struct inet_args *a);

#endif
