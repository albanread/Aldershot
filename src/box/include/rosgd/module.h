/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* module.h -- modules, as the runtime hosts them.
 *
 * A ROSGD module is a descriptor plus code. The descriptor is the list a
 * RISC OS module header holds (title, help, initialise and finalise, a SWI
 * chunk), which ROSCC's gen_module.py already treats as data. A compiled
 * module's descriptor comes from the module header in its ROM image bytes,
 * and a native module declares its own.
 *
 * Workspace is the private word, as ever: an arena address holding a
 * pointer into the RMA, so compiled and native code can share it.
 *
 * A native module keeps *all* its state there, and nothing in C statics.
 * RISC OS state is shared between tasks: a buffer or a record one task
 * creates, another reads, by address. Today the tasks are threads of one
 * process. If each task ran in its own process, where only the arena is
 * common, state anywhere else would split into one copy per process.
 */
#ifndef ROSGD_MODULE_H
#define ROSGD_MODULE_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

struct ros_module;

/* A native SWI implementation, entered with the register block: the
 * generated thunk that unpacks it for a typed function. */
typedef void ros_swi_thunk(struct ros_cpu *s);

/* A *command's information word, as in a module's command table: the
 * fewest and most parameters, which to GSTrans (bit n, parameter n), and
 * the flags. */
#define ROS_CMD_INFO(min, max, gstrans, flags) \
    ((uint32_t)(min) | (uint32_t)(gstrans) << 8 | (uint32_t)(max) << 16 | (uint32_t)(flags))
#define ROS_CMD_FS         0x80000000u  /* a filing system's command */
#define ROS_CMD_CONFIGURE  0x40000000u  /* a *Configure keyword */
#define ROS_CMD_HELP_CODE  0x20000000u  /* help is code, not text */
#define ROS_CMD_INTL_HELP  0x10000000u  /* help and syntax are message tokens */

/* A native module's *command: its table entry, and its code, entered as
 * a compiled command is, with the tail (arena) and the parameter count.
 * Syntax and help as in a module: \x1B\x00 stands for the command's name. */
struct ros_command {
    const char *name;
    uint32_t info;
    const char *syntax;
    const char *help;
    os_error *(*run)(struct ros_module *m, uint32_t tail, uint32_t argc);
};

/* A *Configure keyword's code (ROS_CMD_CONFIGURE) is entered as the kernel
 * enters it: tail 0 to list its syntax, 1 to print its status, else the
 * parameters.  The kernel's generic errors, which a compiled keyword asks
 * for with R0 = 0 to 3: option not recognised, numeric parameter needed,
 * parameter too big, too many parameters (runtime/oscli.c). */
os_error *ros_configure_error(uint32_t which);

struct ros_module {
    const char *title;
    const char *help;

    /* Native entries.  NULL where the module is compiled, or has none. */
    os_error *(*init)(struct ros_module *m, const char *tail);
    os_error *(*final)(struct ros_module *m, int fatal);
    /* A service call: R1 the service, the rest its parameters; set R1 to 0
     * to claim it (Service_Serviced). */
    void (*service)(struct ros_module *m, struct ros_cpu *s);
    /* A native module's error for a SWI in its chunk that it lacks. */
    os_error *(*bad_swi)(struct ros_module *m, uint32_t offset);
    /* A native module's start entry: the module entered as the
     * application (OS_Module 2), tail the command tail (arena), the
     * preferred incarnation's private word in m->private_word.  It runs
     * as a compiled start entry runs, as the program: its SWIs are the
     * outermost, so a Wimp_Poll in it switches tasks, the thread parked
     * inside the SWI until the Wimp comes back to it.  It ends with
     * OS_Exit; one that returns has exited. */
    void (*start)(struct ros_module *m, uint32_t tail);

    /* Compiled entries: code addresses in the ROM image, entered through
     * the dispatcher with RISC OS's registers. Initialise has R10 -> the
     * command tail and R12 -> the private word. SWIs have R11 = the offset
     * in the chunk and R12 -> the private word. */
    uint32_t init_addr;
    uint32_t final_addr;
    uint32_t service_addr;
    uint32_t swi_addr;

    /* The SWI chunk: base number, and how many SWIs it holds. */
    uint32_t swi_chunk;
    uint32_t swi_count;
    /* A native module's SWIs: one thunk per SWI in the chunk. */
    ros_swi_thunk *const *swi_thunks;
    const char *const *swi_names;   /* after the prefix: "Sum", "Classify" */
    const char *swi_prefix;

    /* *Commands: a compiled module's table in its image (base + the
     * header's offset), or a native module's, ending with a NULL name. */
    uint32_t base;                  /* a compiled module's header */
    uint32_t command_table;
    const struct ros_command *commands;

    /* Runtime state. */
    uint32_t private_word;          /* the preferred incarnation's private word */
    uint32_t incarnations;          /* RMA nodes: link, private word, postfix */
    uint32_t base_incarnation;      /* the first, "Base" */
    int cleared;                    /* OS_Module 9 has been past it */
    void *loaded;                   /* loaded from a file (OS_Module 0, 1): its
                                       image's RMA block, freed when it goes */
    void *pristine;                 /* an x32 module's image as placed, before
                                       it ran (host memory). Its statics sit
                                       beside its code, so a fresh start
                                       (*RMReInit, or a start after *RMKill)
                                       puts them back, as a C module's are made
                                       afresh on RISC OS */
    uint32_t pristine_size;
    struct ros_module *next;

    /* ARM shadows. A title has at most one module in the chain and at most
     * one shadow: an ARM module of a native module's title, not in the
     * chain, hanging off its twin. ARM callers reach the shadow, and native
     * callers the twin (ros_module_route). */
    struct ros_module *shadow;      /* on a chain module: its ARM shadow, or NULL */
    struct ros_module *twin;        /* on a shadow: the chain module it shadows */
    uint8_t arm;                    /* ARM code: an ARM image loaded from a file,
                                       the built-in SharedCLibrary */
    uint8_t shadow_policy;          /* ROS_SHADOW_*: a ROM module's from
                                       boot/rom_contents.c's column; 0, the
                                       default, a ROM title's or
                                       ROSGD$Shadowable's (module.c) */
    uint8_t prefer_arm;             /* the escape hatch: native callers sent
                                       to the shadow too (*ARMPrefer) */
    uint32_t image_end;             /* an ARM module's image: [base, image_end),
                                       which says whose a vector claim or a
                                       ResourceFS block is */
};

/* A native module's shadow policy: never shadowed (an ARM load of its title
 * is absorbed), shadowable, or shadowed by the built-in ARM module
 * (SharedCLibrary, made on the first ARM reference). The ROM's modules have
 * theirs from boot/rom_contents.c's column (0 there is never). 0 in a
 * descriptor takes a ROM module's of the same title, else
 * ROSGD$Shadowable's comma-separated list (shadowable if listed). */
#define ROS_SHADOW_NEVER   1
#define ROS_SHADOW_ALLOW   2
#define ROS_SHADOW_BUILTIN 3

/* A compiled module's descriptor, read from the module header at the start
 * of its ROM image: the thirteen header words. Only a 32-bit module is
 * accepted. */
os_error *ros_module_from_rom(struct ros_module *m, uint32_t base);

/* Add a module to the chain and initialise it: its base incarnation. */
os_error *ros_module_add(struct ros_module *m, const char *tail);

/* One of the image's own modules: it stays known when killed, so OS_Module
 * 3 can start it again, and OS_Module 19 lists it (runtime/rom.c). */
void ros_module_register_rom(struct ros_module *m);
/* A module taken out of the ROM table again (the self-test's, once it is
 * done with it) */
void ros_module_unregister_rom(struct ros_module *m);

/* A native module's header built at addr, in the ROM before it is made
 * read-only, rather than in the RMA: its size. */
uint32_t ros_module_header_at(struct ros_module *m, uint32_t addr);
struct ros_module *ros_module_rom(unsigned i);

/* The version in a module's help string, BCD: "1.23" is &00012300. */
uint32_t ros_module_version(const struct ros_module *m);

/* OS_Module 6 and 7, for native code: RMA blocks claimed as the kernel
 * claims them. */
os_error *xos_module_claim(uint32_t size, void **block);
os_error *xos_module_free(void *block);

/* The module whose SWI chunk holds number, or NULL. */
struct ros_module *ros_module_for_swi(uint32_t number);
/* The same for a caller of a kind (switrace.h's ROS_KIND_*): an ARM
 * caller's is a chain module's shadow where it has one */
struct ros_module *ros_module_for_swi_kind(uint32_t number, int kind);
/* The module a caller of a kind reaches for chain module m: its shadow for
 * an ARM caller (the built-in SharedCLibrary made now if it is not yet),
 * else m itself */
struct ros_module *ros_module_route(struct ros_module *m, int kind);
/* The same without making the built-in SharedCLibrary: for walks of the
 * chain (*commands, *Help) that must not make it as they pass */
struct ros_module *ros_module_route_existing(struct ros_module *m, int kind);
/* The built-in shadow of SharedCLibrary, made (runtime/armrun/box.c): the
 * module, initialised, or NULL */
struct ros_module *ros_armrun_builtin_clib(struct ros_module *twin);
/* A shadow for twin, made from an ARM module's header at base (in the RMA,
 * its code range registered, block its RMA block): initialised, linked as
 * the twin's shadow (module.c) */
os_error *ros_module_make_shadow(struct ros_module *twin, uint32_t base, uint32_t size,
                                 void *block, struct ros_module **out);
/* A squeezed module (modsqz: its initialise offset's bit 31 set): *image
 * is an RMA block of *size bytes, and is unsqueezed as RISC OS's kernel
 * does into a new RMA block, which replaces it (the old one freed). A
 * module not squeezed is left as it is. OS_Module 0, 1, 10 and 11 do this
 * (#162) */
os_error *ros_module_unsqueeze(uint8_t **image, uint32_t *size);
/* The shadow whose image holds addr, which is a shadow linked to its twin
 * or one initialising on this thread, or NULL. The address says whose a
 * vector claim or a ResourceFS block is. */
struct ros_module *ros_module_shadow_at(uint32_t addr);
/* The ARM module whose image holds addr, which may be in the chain, a
 * shadow, or one initialising, or NULL. It is for a fault's report */
struct ros_module *ros_module_arm_at(uint32_t addr);
/* The shadow initialising on this thread, or NULL. While it is, ARM code
 * cannot change a system variable that already exists. */
struct ros_module *ros_module_shadow_initialising(void);

/* *ARMPrefer: native callers of title are sent to its shadow too (on), or
 * not (off), for testing. The title is remembered, so a native module of it
 * that joins the chain later is preferred. Refused for the built-in
 * SharedCLibrary. */
os_error *ros_module_armprefer(const char *title, int on);
/* The i'th preferred title (0 the first), or NULL past the last */
const char *ros_module_armpreferred(unsigned i);

struct ros_module *ros_module_first(void);

/* OS_ServiceCall: R1 the service, the rest its parameters, offered to
 * each module in turn until one claims it by setting R1 to 0. */
void ros_service_call(struct ros_cpu *s);

#endif

/* An application entered from a command (OS_Module 2) exits here. */
void ros_module_app_exit(void);
/* Native code run as such an application (a C application's loader):
 * returns when it exits. */
void ros_module_run_as_application(void (*run)(void *arg), void *arg);
/* An application starting on this thread, which will come back: OS_CLI's
 * alias expansions in progress under it keep their lines (oscli.c) */
void ros_oscli_application_starts(void);
/* An application starting on this thread: if an Obey file of the thread has
 * lines still to run, its exit comes back to the file (filecmds.c) */
void ros_obey_catch_exit(void);
/* An Obey file of this thread has lines still to run (filecmds.c) */
int ros_obey_lines_left(void);
