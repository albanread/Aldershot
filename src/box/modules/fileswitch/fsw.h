/* Copyright 1996 Acorn Computers Ltd
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
 * This file is a reimplementation in C of RISC OS Open's source
 * (Sources/FileSys/FileSwitch: hdr.LowFSI, hdr.HighFSI, s.FSErrors,
 * s.FileSwHdr).
 */

/* fsw.h -- FileSwitch's insides, shared by its files: fileswitch.c (the
 * vectors, names, streams) and fsutils.c (the listings, Access, Copy, Wipe,
 * Count, and running files).  This is not an interface.  The module's
 * interface is its SWIs.
 */
#ifndef ROSGD_FSW_H
#define ROSGD_FSW_H

#include <stddef.h>
#include <stdint.h>

#include "fileswitch.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"

/* FileSwitch's errors (hdr/NewErrors, FileSwitch's Messages) */
#define E_ALREADY_EXISTS  0xC4u
#define E_BAD_COMMAND     0xFEu
#define E_BAD_COPY        0xB1u
#define E_BAD_FILE_NAME   0xCCu
#define E_BAD_FILE_TYPE   0x41Du
#define E_BAD_FIND_MODE   0x405u
#define E_BAD_FSCONTROL   0x401u
#define E_BAD_OSARGS      0x403u
#define E_BAD_OSFILE      0x402u
#define E_BAD_OSGBPB      0x404u
#define E_BAD_PARAMETERS  0x1EAu
#define E_BAD_RENAME      0xB0u
#define E_BUFF_OVERFLOW   0x1E4u
#define E_BAD_OPTION      0x601u
#define E_TYPES_DONT_MATCH 0xAFu
#define E_CHANNEL         0xDEu
#define E_END_OF_FILE     0xDFu
#define E_FILE_NOT_FOUND  0xD6u
#define E_IS_A_DIRECTORY  0xA8u
#define E_IS_A_FILE       0x41Cu
#define E_NOTHING_TO_COPY 0x415u
#define E_NOTHING_TO_DELETE 0x416u
#define E_NOT_READING     0x413u
#define E_NOT_UPDATE      0xC1u
#define E_NO_SELECTED_FS  0x40Bu
#define E_NO_STACK        0x414u      /* NotEnoughStackForFSEntry */
#define E_OUTSIDE_FILE    0xB7u
#define E_RECURSIVE_PATH  0x425u
#define E_TOO_MANY_OPEN   0xC0u
#define E_UNKNOWN_ACTION  0x409u
#define E_UNKNOWN_FS      0xF8u
#define E_UNSUPPORTED     0x40Eu
#define E_WILDCARDS       0xFDu

/* The filing systems: HostFS, ResourceFS and LanMan at 0-2, then those
 * modules add (OS_FSControl 12, modfs.c), a slot each; 0 where none */
#define FSW_MAXFS 24
extern const struct fs *fsw_filing_systems[FSW_MAXFS];

/* A place: a filing system, a disc, and the path from $ ("" for $) */
struct fsw_loc {
    int fsi;
    char disc[32];
    char path[1024];
};

/* Each filing system's directories.  Lib and URD may be unset, and then
 * mean $, as FileSwitch has them. */
struct fsw_dirs {
    int ready;
    int lib_set, urd_set;
    struct fsw_loc csd, lib, urd, psd;
};

extern struct fsw_dirs fsw_dirs[FSW_MAXFS];
extern int fsw_current;                 /* the current filing system, or -1 */
extern int fsw_temp;                    /* the temporary one, or -1 */

enum { R_READ, R_WRITE };

const struct fs *fsw_fs_of(const struct fsw_loc *l);
struct fsw_dirs *fsw_dirs_of(int fsi);
os_error *fsw_select_fs(int fsi);

/* Names: resolve one to a place (mode R_READ matches wildcards, R_WRITE
 * refuses them); through a list of places when it is relative. */
os_error *fsw_resolve(const char *name, int mode, struct fsw_loc *out, int depth);
os_error *fsw_resolve_via(const char *name, const char *list, int mode, struct fsw_loc *out);
os_error *fsw_path_list(int kind, uint32_t arg, char *out, size_t max);
os_error *fsw_stat_loc(const struct fsw_loc *l, struct fs_info *info);
os_error *fsw_dir_arg(uint32_t addr, struct fsw_loc *l, const char *dflt);
void fsw_canonical(const struct fsw_loc *l, char *out, size_t max);
const char *fsw_leaf_of(const struct fsw_loc *l);
int fsw_has_wild(const char *s);
int fsw_wild_match(const char *pat, const char *s);

/* Changes to a filing system, each told first through UpCall_ModifyingFile
 * (OS_UpCall 3), R9 one of these (hdr/UpCall), as FileSwitch's
 * DoUpCallModifyingFile (s/LowLevel); fsw_setinfo takes OS_File 1-4's
 * reasons and says WriteInfo, as FileSwitch passes them all */
#define UPFS_SAVE          0x000u
#define UPFS_WRITE_INFO    0x001u
#define UPFS_DELETE        0x006u
#define UPFS_CREATE        0x007u
#define UPFS_CREATE_DIR    0x008u
#define UPFS_OPEN_UPDATE   0x102u
#define UPFS_CLOSE         0x103u
#define UPFS_ENSURE_SIZE   0x200u
#define UPFS_RENAME        0x208u
uint32_t fsw_info_word(int fsi);
void fsw_modifying(uint32_t reason, const struct fsw_loc *l, uint32_t r2, uint32_t r3,
                   uint32_t r4, uint32_t r5);
os_error *fsw_create(const struct fsw_loc *l, uint32_t load, uint32_t exec, uint32_t start,
                     uint32_t end);
os_error *fsw_mkdir(const struct fsw_loc *l);
os_error *fsw_remove(const struct fsw_loc *l, const struct fs_info *i);
os_error *fsw_setinfo(const struct fsw_loc *l, int reason, uint32_t load, uint32_t exec,
                      uint32_t attr);

/* Types and dates */
uint64_t fsw_now_cs(void);
uint32_t fsw_stamp_load(uint32_t type, uint64_t cs);
int fsw_is_typed(uint32_t load);
uint32_t fsw_file_type_of(const char *leaf, const struct fs_info *i);
void fsw_type_name(uint32_t type, char out[9]);

/* The caller's strings, variables, and output */
os_error *fsw_arg_name(uint32_t addr, char *out, size_t max);
os_error *fsw_arg_path(uint32_t addr, char *out, size_t max);
int fsw_read_var(const char *name, char *out, size_t max);
os_error *fsw_write_s(const char *s);
os_error *fsw_printf_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

os_error *fsw_err_bad_name(const char *name);
os_error *fsw_err_not_found(const char *name);
os_error *fsw_err_is_dir(const char *name);
os_error *fsw_close_all(void);

/* modfs.c: every call of a filing system, on a place.  If the place is
 * inside an image, the call goes through the image.  Otherwise it goes to
 * the place's own filing system: the native ones' struct fs, or a module's
 * entries.  fsw_stat_loc above opens an image a path goes into.
 * fsw_readdir reads an image file's contents. */
int fsw_readdir(const struct fsw_loc *l, uint32_t index, struct fs_entry *e, os_error **err);
int fsw_end_when_none(const struct fsw_loc *l);
int fsw_read_only(const struct fsw_loc *l);
os_error *fsw_fs_create(const struct fsw_loc *l, uint32_t load, uint32_t exec, uint32_t length);
os_error *fsw_fs_mkdir(const struct fsw_loc *l);
os_error *fsw_fs_remove(const struct fsw_loc *l);
os_error *fsw_fs_setinfo(const struct fsw_loc *l, int reason, uint32_t load, uint32_t exec,
                         uint32_t attr);
os_error *fsw_fs_rename(const struct fsw_loc *from, const struct fsw_loc *to);
os_error *fsw_fs_free(const struct fsw_loc *l, uint64_t *fr, uint64_t *big, uint64_t *size);
os_error *fsw_fs_save(const struct fsw_loc *l, uint32_t load, uint32_t exec, uint32_t start,
                      uint32_t end);
os_error *fsw_fs_load(const struct fsw_loc *l, uint32_t addr, uint32_t length);

/* Open files: a native filing system's handle, or a module's, buffered as
 * FileSwitch buffers them.  swh is the FileSwitch handle the open is for
 * (FSEntry_Open's R3), 0 for FileSwitch's own. */
struct fsw_file;
os_error *fsw_file_open(const struct fsw_loc *l, int write, uint32_t swh, struct fsw_file **f);
os_error *fsw_file_read(struct fsw_file *f, uint32_t pos, void *buf, uint32_t n, uint32_t *got);
os_error *fsw_file_write(struct fsw_file *f, uint32_t pos, const void *buf, uint32_t n);
os_error *fsw_file_set_extent(struct fsw_file *f, uint32_t extent);
uint32_t fsw_file_extent(struct fsw_file *f);
os_error *fsw_file_flush(struct fsw_file *f);
os_error *fsw_file_close(struct fsw_file *f, int modified);
uint32_t fsw_file_fh(const struct fsw_file *f);
uint32_t fsw_file_bufsize(const struct fsw_file *f);
int fsw_file_image(const struct fsw_file *f);

/* Filing systems and image filing systems from modules (OS_FSControl 12,
 * 16, 35, 36, 15, 20) */
os_error *fsw_add_fs(uint32_t base, uint32_t offset, uint32_t r12);
os_error *fsw_remove_fs(int fsi);
os_error *fsw_add_image_fs(uint32_t base, uint32_t offset, uint32_t r12);
os_error *fsw_remove_image_fs(uint32_t type);
int fsw_module_of(int fsi, uint32_t *base, uint32_t *r12);
os_error *fsw_bootup_fs(int fsi);
void fsw_shutdown_fs(void);
void fsw_forget_modules(void);

/* Images (modfs.c), whose files are streams (fileswitch.c) */
int fsw_image_of_stream(uint32_t h);
void fsw_image_closing(int img);
unsigned fsw_images_close_at(const struct fsw_loc *l);
os_error *fsw_stream_open(const struct fsw_loc *l, int update, uint32_t *h);
os_error *fsw_stream_close(uint32_t h);
uint32_t fsw_stream_bufsize(uint32_t h);
int fsw_stream_files_inside(int img);       /* streams open inside it, not images */
void fsw_close_all_on(int fsi, os_error **first);
void fsw_fs_blocks_changed(void);

/* fsutils.c: OS_FSControl's utilities */
os_error *fsw_catex(uint32_t reason, uint32_t tail);       /* 5-8 */
os_error *fsw_info(uint32_t reason, uint32_t name);        /* 9, 32 */
os_error *fsw_access(uint32_t name, uint32_t access);      /* 24 */
os_error *fsw_copy(struct ros_cpu *s);                     /* 26 */
os_error *fsw_wipe(struct ros_cpu *s);                     /* 27 */
os_error *fsw_count(struct ros_cpu *s);                    /* 28 */
os_error *fsw_run(uint32_t line);                          /* 4 */
extern const struct ros_command fsw_util_commands[];

#endif
