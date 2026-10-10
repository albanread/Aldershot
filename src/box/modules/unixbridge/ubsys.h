/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* ubsys.h -- the Unix bridge's call numbers: x86-64 Linux's, from linux-6.18.54's
 * arch/x86/entry/syscalls/syscall_64.tbl, by tools/unix_syscalls.py: do not edit.
 * UB_X32_* are x32's own entries (the x32 bit is added by the caller). */
#ifndef ROSGD_UBSYS_H
#define ROSGD_UBSYS_H

#define UB_read 0
#define UB_write 1
#define UB_open 2
#define UB_close 3
#define UB_stat 4
#define UB_fstat 5
#define UB_lstat 6
#define UB_poll 7
#define UB_lseek 8
#define UB_mmap 9
#define UB_mprotect 10
#define UB_munmap 11
#define UB_brk 12
#define UB_rt_sigaction 13
#define UB_rt_sigprocmask 14
#define UB_rt_sigreturn 15
#define UB_ioctl 16
#define UB_pread64 17
#define UB_pwrite64 18
#define UB_readv 19
#define UB_writev 20
#define UB_access 21
#define UB_pipe 22
#define UB_select 23
#define UB_sched_yield 24
#define UB_mremap 25
#define UB_msync 26
#define UB_mincore 27
#define UB_madvise 28
#define UB_shmget 29
#define UB_shmat 30
#define UB_shmctl 31
#define UB_dup 32
#define UB_dup2 33
#define UB_pause 34
#define UB_nanosleep 35
#define UB_getitimer 36
#define UB_alarm 37
#define UB_setitimer 38
#define UB_getpid 39
#define UB_sendfile 40
#define UB_socket 41
#define UB_connect 42
#define UB_accept 43
#define UB_sendto 44
#define UB_recvfrom 45
#define UB_sendmsg 46
#define UB_recvmsg 47
#define UB_shutdown 48
#define UB_bind 49
#define UB_listen 50
#define UB_getsockname 51
#define UB_getpeername 52
#define UB_socketpair 53
#define UB_setsockopt 54
#define UB_getsockopt 55
#define UB_clone 56
#define UB_fork 57
#define UB_vfork 58
#define UB_execve 59
#define UB_exit 60
#define UB_wait4 61
#define UB_kill 62
#define UB_uname 63
#define UB_semget 64
#define UB_semop 65
#define UB_semctl 66
#define UB_shmdt 67
#define UB_msgget 68
#define UB_msgsnd 69
#define UB_msgrcv 70
#define UB_msgctl 71
#define UB_fcntl 72
#define UB_flock 73
#define UB_fsync 74
#define UB_fdatasync 75
#define UB_truncate 76
#define UB_ftruncate 77
#define UB_getdents 78
#define UB_getcwd 79
#define UB_chdir 80
#define UB_fchdir 81
#define UB_rename 82
#define UB_mkdir 83
#define UB_rmdir 84
#define UB_creat 85
#define UB_link 86
#define UB_unlink 87
#define UB_symlink 88
#define UB_readlink 89
#define UB_chmod 90
#define UB_fchmod 91
#define UB_chown 92
#define UB_fchown 93
#define UB_lchown 94
#define UB_umask 95
#define UB_gettimeofday 96
#define UB_getrlimit 97
#define UB_getrusage 98
#define UB_sysinfo 99
#define UB_times 100
#define UB_ptrace 101
#define UB_getuid 102
#define UB_syslog 103
#define UB_getgid 104
#define UB_setuid 105
#define UB_setgid 106
#define UB_geteuid 107
#define UB_getegid 108
#define UB_setpgid 109
#define UB_getppid 110
#define UB_getpgrp 111
#define UB_setsid 112
#define UB_setreuid 113
#define UB_setregid 114
#define UB_getgroups 115
#define UB_setgroups 116
#define UB_setresuid 117
#define UB_getresuid 118
#define UB_setresgid 119
#define UB_getresgid 120
#define UB_getpgid 121
#define UB_setfsuid 122
#define UB_setfsgid 123
#define UB_getsid 124
#define UB_capget 125
#define UB_capset 126
#define UB_rt_sigpending 127
#define UB_rt_sigtimedwait 128
#define UB_rt_sigqueueinfo 129
#define UB_rt_sigsuspend 130
#define UB_sigaltstack 131
#define UB_utime 132
#define UB_mknod 133
#define UB_uselib 134
#define UB_personality 135
#define UB_ustat 136
#define UB_statfs 137
#define UB_fstatfs 138
#define UB_sysfs 139
#define UB_getpriority 140
#define UB_setpriority 141
#define UB_sched_setparam 142
#define UB_sched_getparam 143
#define UB_sched_setscheduler 144
#define UB_sched_getscheduler 145
#define UB_sched_get_priority_max 146
#define UB_sched_get_priority_min 147
#define UB_sched_rr_get_interval 148
#define UB_mlock 149
#define UB_munlock 150
#define UB_mlockall 151
#define UB_munlockall 152
#define UB_vhangup 153
#define UB_modify_ldt 154
#define UB_pivot_root 155
#define UB__sysctl 156
#define UB_prctl 157
#define UB_arch_prctl 158
#define UB_adjtimex 159
#define UB_setrlimit 160
#define UB_chroot 161
#define UB_sync 162
#define UB_acct 163
#define UB_settimeofday 164
#define UB_mount 165
#define UB_umount2 166
#define UB_swapon 167
#define UB_swapoff 168
#define UB_reboot 169
#define UB_sethostname 170
#define UB_setdomainname 171
#define UB_iopl 172
#define UB_ioperm 173
#define UB_create_module 174
#define UB_init_module 175
#define UB_delete_module 176
#define UB_get_kernel_syms 177
#define UB_query_module 178
#define UB_quotactl 179
#define UB_nfsservctl 180
#define UB_getpmsg 181
#define UB_putpmsg 182
#define UB_afs_syscall 183
#define UB_tuxcall 184
#define UB_security 185
#define UB_gettid 186
#define UB_readahead 187
#define UB_setxattr 188
#define UB_lsetxattr 189
#define UB_fsetxattr 190
#define UB_getxattr 191
#define UB_lgetxattr 192
#define UB_fgetxattr 193
#define UB_listxattr 194
#define UB_llistxattr 195
#define UB_flistxattr 196
#define UB_removexattr 197
#define UB_lremovexattr 198
#define UB_fremovexattr 199
#define UB_tkill 200
#define UB_time 201
#define UB_futex 202
#define UB_sched_setaffinity 203
#define UB_sched_getaffinity 204
#define UB_set_thread_area 205
#define UB_io_setup 206
#define UB_io_destroy 207
#define UB_io_getevents 208
#define UB_io_submit 209
#define UB_io_cancel 210
#define UB_get_thread_area 211
#define UB_lookup_dcookie 212
#define UB_epoll_create 213
#define UB_epoll_ctl_old 214
#define UB_epoll_wait_old 215
#define UB_remap_file_pages 216
#define UB_getdents64 217
#define UB_set_tid_address 218
#define UB_restart_syscall 219
#define UB_semtimedop 220
#define UB_fadvise64 221
#define UB_timer_create 222
#define UB_timer_settime 223
#define UB_timer_gettime 224
#define UB_timer_getoverrun 225
#define UB_timer_delete 226
#define UB_clock_settime 227
#define UB_clock_gettime 228
#define UB_clock_getres 229
#define UB_clock_nanosleep 230
#define UB_exit_group 231
#define UB_epoll_wait 232
#define UB_epoll_ctl 233
#define UB_tgkill 234
#define UB_utimes 235
#define UB_vserver 236
#define UB_mbind 237
#define UB_set_mempolicy 238
#define UB_get_mempolicy 239
#define UB_mq_open 240
#define UB_mq_unlink 241
#define UB_mq_timedsend 242
#define UB_mq_timedreceive 243
#define UB_mq_notify 244
#define UB_mq_getsetattr 245
#define UB_kexec_load 246
#define UB_waitid 247
#define UB_add_key 248
#define UB_request_key 249
#define UB_keyctl 250
#define UB_ioprio_set 251
#define UB_ioprio_get 252
#define UB_inotify_init 253
#define UB_inotify_add_watch 254
#define UB_inotify_rm_watch 255
#define UB_migrate_pages 256
#define UB_openat 257
#define UB_mkdirat 258
#define UB_mknodat 259
#define UB_fchownat 260
#define UB_futimesat 261
#define UB_newfstatat 262
#define UB_unlinkat 263
#define UB_renameat 264
#define UB_linkat 265
#define UB_symlinkat 266
#define UB_readlinkat 267
#define UB_fchmodat 268
#define UB_faccessat 269
#define UB_pselect6 270
#define UB_ppoll 271
#define UB_unshare 272
#define UB_set_robust_list 273
#define UB_get_robust_list 274
#define UB_splice 275
#define UB_tee 276
#define UB_sync_file_range 277
#define UB_vmsplice 278
#define UB_move_pages 279
#define UB_utimensat 280
#define UB_epoll_pwait 281
#define UB_signalfd 282
#define UB_timerfd_create 283
#define UB_eventfd 284
#define UB_fallocate 285
#define UB_timerfd_settime 286
#define UB_timerfd_gettime 287
#define UB_accept4 288
#define UB_signalfd4 289
#define UB_eventfd2 290
#define UB_epoll_create1 291
#define UB_dup3 292
#define UB_pipe2 293
#define UB_inotify_init1 294
#define UB_preadv 295
#define UB_pwritev 296
#define UB_rt_tgsigqueueinfo 297
#define UB_perf_event_open 298
#define UB_recvmmsg 299
#define UB_fanotify_init 300
#define UB_fanotify_mark 301
#define UB_prlimit64 302
#define UB_name_to_handle_at 303
#define UB_open_by_handle_at 304
#define UB_clock_adjtime 305
#define UB_syncfs 306
#define UB_sendmmsg 307
#define UB_setns 308
#define UB_getcpu 309
#define UB_process_vm_readv 310
#define UB_process_vm_writev 311
#define UB_kcmp 312
#define UB_finit_module 313
#define UB_sched_setattr 314
#define UB_sched_getattr 315
#define UB_renameat2 316
#define UB_seccomp 317
#define UB_getrandom 318
#define UB_memfd_create 319
#define UB_kexec_file_load 320
#define UB_bpf 321
#define UB_execveat 322
#define UB_userfaultfd 323
#define UB_membarrier 324
#define UB_mlock2 325
#define UB_copy_file_range 326
#define UB_preadv2 327
#define UB_pwritev2 328
#define UB_pkey_mprotect 329
#define UB_pkey_alloc 330
#define UB_pkey_free 331
#define UB_statx 332
#define UB_io_pgetevents 333
#define UB_rseq 334
#define UB_uretprobe 335
#define UB_uprobe 336
#define UB_pidfd_send_signal 424
#define UB_io_uring_setup 425
#define UB_io_uring_enter 426
#define UB_io_uring_register 427
#define UB_open_tree 428
#define UB_move_mount 429
#define UB_fsopen 430
#define UB_fsconfig 431
#define UB_fsmount 432
#define UB_fspick 433
#define UB_pidfd_open 434
#define UB_clone3 435
#define UB_close_range 436
#define UB_openat2 437
#define UB_pidfd_getfd 438
#define UB_faccessat2 439
#define UB_process_madvise 440
#define UB_epoll_pwait2 441
#define UB_mount_setattr 442
#define UB_quotactl_fd 443
#define UB_landlock_create_ruleset 444
#define UB_landlock_add_rule 445
#define UB_landlock_restrict_self 446
#define UB_memfd_secret 447
#define UB_process_mrelease 448
#define UB_futex_waitv 449
#define UB_set_mempolicy_home_node 450
#define UB_cachestat 451
#define UB_fchmodat2 452
#define UB_map_shadow_stack 453
#define UB_futex_wake 454
#define UB_futex_wait 455
#define UB_futex_requeue 456
#define UB_statmount 457
#define UB_listmount 458
#define UB_lsm_get_self_attr 459
#define UB_lsm_set_self_attr 460
#define UB_lsm_list_modules 461
#define UB_mseal 462
#define UB_setxattrat 463
#define UB_getxattrat 464
#define UB_listxattrat 465
#define UB_removexattrat 466
#define UB_open_tree_attr 467
#define UB_file_getattr 468
#define UB_file_setattr 469

#define UB_X32_rt_sigaction 512
#define UB_X32_rt_sigreturn 513
#define UB_X32_ioctl 514
#define UB_X32_readv 515
#define UB_X32_writev 516
#define UB_X32_recvfrom 517
#define UB_X32_sendmsg 518
#define UB_X32_recvmsg 519
#define UB_X32_execve 520
#define UB_X32_ptrace 521
#define UB_X32_rt_sigpending 522
#define UB_X32_rt_sigtimedwait 523
#define UB_X32_rt_sigqueueinfo 524
#define UB_X32_sigaltstack 525
#define UB_X32_timer_create 526
#define UB_X32_mq_notify 527
#define UB_X32_kexec_load 528
#define UB_X32_waitid 529
#define UB_X32_set_robust_list 530
#define UB_X32_get_robust_list 531
#define UB_X32_vmsplice 532
#define UB_X32_move_pages 533
#define UB_X32_preadv 534
#define UB_X32_pwritev 535
#define UB_X32_rt_tgsigqueueinfo 536
#define UB_X32_recvmmsg 537
#define UB_X32_sendmmsg 538
#define UB_X32_process_vm_readv 539
#define UB_X32_process_vm_writev 540
#define UB_X32_setsockopt 541
#define UB_X32_getsockopt 542
#define UB_X32_io_setup 543
#define UB_X32_io_submit 544
#define UB_X32_execveat 545
#define UB_X32_preadv2 546
#define UB_X32_pwritev2 547

#define UB_NR_LIMIT 548
#define UB_NAMES { \
    [0] = "read", \
    [1] = "write", \
    [2] = "open", \
    [3] = "close", \
    [4] = "stat", \
    [5] = "fstat", \
    [6] = "lstat", \
    [7] = "poll", \
    [8] = "lseek", \
    [9] = "mmap", \
    [10] = "mprotect", \
    [11] = "munmap", \
    [12] = "brk", \
    [13] = "rt_sigaction", \
    [14] = "rt_sigprocmask", \
    [15] = "rt_sigreturn", \
    [16] = "ioctl", \
    [17] = "pread64", \
    [18] = "pwrite64", \
    [19] = "readv", \
    [20] = "writev", \
    [21] = "access", \
    [22] = "pipe", \
    [23] = "select", \
    [24] = "sched_yield", \
    [25] = "mremap", \
    [26] = "msync", \
    [27] = "mincore", \
    [28] = "madvise", \
    [29] = "shmget", \
    [30] = "shmat", \
    [31] = "shmctl", \
    [32] = "dup", \
    [33] = "dup2", \
    [34] = "pause", \
    [35] = "nanosleep", \
    [36] = "getitimer", \
    [37] = "alarm", \
    [38] = "setitimer", \
    [39] = "getpid", \
    [40] = "sendfile", \
    [41] = "socket", \
    [42] = "connect", \
    [43] = "accept", \
    [44] = "sendto", \
    [45] = "recvfrom", \
    [46] = "sendmsg", \
    [47] = "recvmsg", \
    [48] = "shutdown", \
    [49] = "bind", \
    [50] = "listen", \
    [51] = "getsockname", \
    [52] = "getpeername", \
    [53] = "socketpair", \
    [54] = "setsockopt", \
    [55] = "getsockopt", \
    [56] = "clone", \
    [57] = "fork", \
    [58] = "vfork", \
    [59] = "execve", \
    [60] = "exit", \
    [61] = "wait4", \
    [62] = "kill", \
    [63] = "uname", \
    [64] = "semget", \
    [65] = "semop", \
    [66] = "semctl", \
    [67] = "shmdt", \
    [68] = "msgget", \
    [69] = "msgsnd", \
    [70] = "msgrcv", \
    [71] = "msgctl", \
    [72] = "fcntl", \
    [73] = "flock", \
    [74] = "fsync", \
    [75] = "fdatasync", \
    [76] = "truncate", \
    [77] = "ftruncate", \
    [78] = "getdents", \
    [79] = "getcwd", \
    [80] = "chdir", \
    [81] = "fchdir", \
    [82] = "rename", \
    [83] = "mkdir", \
    [84] = "rmdir", \
    [85] = "creat", \
    [86] = "link", \
    [87] = "unlink", \
    [88] = "symlink", \
    [89] = "readlink", \
    [90] = "chmod", \
    [91] = "fchmod", \
    [92] = "chown", \
    [93] = "fchown", \
    [94] = "lchown", \
    [95] = "umask", \
    [96] = "gettimeofday", \
    [97] = "getrlimit", \
    [98] = "getrusage", \
    [99] = "sysinfo", \
    [100] = "times", \
    [101] = "ptrace", \
    [102] = "getuid", \
    [103] = "syslog", \
    [104] = "getgid", \
    [105] = "setuid", \
    [106] = "setgid", \
    [107] = "geteuid", \
    [108] = "getegid", \
    [109] = "setpgid", \
    [110] = "getppid", \
    [111] = "getpgrp", \
    [112] = "setsid", \
    [113] = "setreuid", \
    [114] = "setregid", \
    [115] = "getgroups", \
    [116] = "setgroups", \
    [117] = "setresuid", \
    [118] = "getresuid", \
    [119] = "setresgid", \
    [120] = "getresgid", \
    [121] = "getpgid", \
    [122] = "setfsuid", \
    [123] = "setfsgid", \
    [124] = "getsid", \
    [125] = "capget", \
    [126] = "capset", \
    [127] = "rt_sigpending", \
    [128] = "rt_sigtimedwait", \
    [129] = "rt_sigqueueinfo", \
    [130] = "rt_sigsuspend", \
    [131] = "sigaltstack", \
    [132] = "utime", \
    [133] = "mknod", \
    [134] = "uselib", \
    [135] = "personality", \
    [136] = "ustat", \
    [137] = "statfs", \
    [138] = "fstatfs", \
    [139] = "sysfs", \
    [140] = "getpriority", \
    [141] = "setpriority", \
    [142] = "sched_setparam", \
    [143] = "sched_getparam", \
    [144] = "sched_setscheduler", \
    [145] = "sched_getscheduler", \
    [146] = "sched_get_priority_max", \
    [147] = "sched_get_priority_min", \
    [148] = "sched_rr_get_interval", \
    [149] = "mlock", \
    [150] = "munlock", \
    [151] = "mlockall", \
    [152] = "munlockall", \
    [153] = "vhangup", \
    [154] = "modify_ldt", \
    [155] = "pivot_root", \
    [156] = "_sysctl", \
    [157] = "prctl", \
    [158] = "arch_prctl", \
    [159] = "adjtimex", \
    [160] = "setrlimit", \
    [161] = "chroot", \
    [162] = "sync", \
    [163] = "acct", \
    [164] = "settimeofday", \
    [165] = "mount", \
    [166] = "umount2", \
    [167] = "swapon", \
    [168] = "swapoff", \
    [169] = "reboot", \
    [170] = "sethostname", \
    [171] = "setdomainname", \
    [172] = "iopl", \
    [173] = "ioperm", \
    [174] = "create_module", \
    [175] = "init_module", \
    [176] = "delete_module", \
    [177] = "get_kernel_syms", \
    [178] = "query_module", \
    [179] = "quotactl", \
    [180] = "nfsservctl", \
    [181] = "getpmsg", \
    [182] = "putpmsg", \
    [183] = "afs_syscall", \
    [184] = "tuxcall", \
    [185] = "security", \
    [186] = "gettid", \
    [187] = "readahead", \
    [188] = "setxattr", \
    [189] = "lsetxattr", \
    [190] = "fsetxattr", \
    [191] = "getxattr", \
    [192] = "lgetxattr", \
    [193] = "fgetxattr", \
    [194] = "listxattr", \
    [195] = "llistxattr", \
    [196] = "flistxattr", \
    [197] = "removexattr", \
    [198] = "lremovexattr", \
    [199] = "fremovexattr", \
    [200] = "tkill", \
    [201] = "time", \
    [202] = "futex", \
    [203] = "sched_setaffinity", \
    [204] = "sched_getaffinity", \
    [205] = "set_thread_area", \
    [206] = "io_setup", \
    [207] = "io_destroy", \
    [208] = "io_getevents", \
    [209] = "io_submit", \
    [210] = "io_cancel", \
    [211] = "get_thread_area", \
    [212] = "lookup_dcookie", \
    [213] = "epoll_create", \
    [214] = "epoll_ctl_old", \
    [215] = "epoll_wait_old", \
    [216] = "remap_file_pages", \
    [217] = "getdents64", \
    [218] = "set_tid_address", \
    [219] = "restart_syscall", \
    [220] = "semtimedop", \
    [221] = "fadvise64", \
    [222] = "timer_create", \
    [223] = "timer_settime", \
    [224] = "timer_gettime", \
    [225] = "timer_getoverrun", \
    [226] = "timer_delete", \
    [227] = "clock_settime", \
    [228] = "clock_gettime", \
    [229] = "clock_getres", \
    [230] = "clock_nanosleep", \
    [231] = "exit_group", \
    [232] = "epoll_wait", \
    [233] = "epoll_ctl", \
    [234] = "tgkill", \
    [235] = "utimes", \
    [236] = "vserver", \
    [237] = "mbind", \
    [238] = "set_mempolicy", \
    [239] = "get_mempolicy", \
    [240] = "mq_open", \
    [241] = "mq_unlink", \
    [242] = "mq_timedsend", \
    [243] = "mq_timedreceive", \
    [244] = "mq_notify", \
    [245] = "mq_getsetattr", \
    [246] = "kexec_load", \
    [247] = "waitid", \
    [248] = "add_key", \
    [249] = "request_key", \
    [250] = "keyctl", \
    [251] = "ioprio_set", \
    [252] = "ioprio_get", \
    [253] = "inotify_init", \
    [254] = "inotify_add_watch", \
    [255] = "inotify_rm_watch", \
    [256] = "migrate_pages", \
    [257] = "openat", \
    [258] = "mkdirat", \
    [259] = "mknodat", \
    [260] = "fchownat", \
    [261] = "futimesat", \
    [262] = "newfstatat", \
    [263] = "unlinkat", \
    [264] = "renameat", \
    [265] = "linkat", \
    [266] = "symlinkat", \
    [267] = "readlinkat", \
    [268] = "fchmodat", \
    [269] = "faccessat", \
    [270] = "pselect6", \
    [271] = "ppoll", \
    [272] = "unshare", \
    [273] = "set_robust_list", \
    [274] = "get_robust_list", \
    [275] = "splice", \
    [276] = "tee", \
    [277] = "sync_file_range", \
    [278] = "vmsplice", \
    [279] = "move_pages", \
    [280] = "utimensat", \
    [281] = "epoll_pwait", \
    [282] = "signalfd", \
    [283] = "timerfd_create", \
    [284] = "eventfd", \
    [285] = "fallocate", \
    [286] = "timerfd_settime", \
    [287] = "timerfd_gettime", \
    [288] = "accept4", \
    [289] = "signalfd4", \
    [290] = "eventfd2", \
    [291] = "epoll_create1", \
    [292] = "dup3", \
    [293] = "pipe2", \
    [294] = "inotify_init1", \
    [295] = "preadv", \
    [296] = "pwritev", \
    [297] = "rt_tgsigqueueinfo", \
    [298] = "perf_event_open", \
    [299] = "recvmmsg", \
    [300] = "fanotify_init", \
    [301] = "fanotify_mark", \
    [302] = "prlimit64", \
    [303] = "name_to_handle_at", \
    [304] = "open_by_handle_at", \
    [305] = "clock_adjtime", \
    [306] = "syncfs", \
    [307] = "sendmmsg", \
    [308] = "setns", \
    [309] = "getcpu", \
    [310] = "process_vm_readv", \
    [311] = "process_vm_writev", \
    [312] = "kcmp", \
    [313] = "finit_module", \
    [314] = "sched_setattr", \
    [315] = "sched_getattr", \
    [316] = "renameat2", \
    [317] = "seccomp", \
    [318] = "getrandom", \
    [319] = "memfd_create", \
    [320] = "kexec_file_load", \
    [321] = "bpf", \
    [322] = "execveat", \
    [323] = "userfaultfd", \
    [324] = "membarrier", \
    [325] = "mlock2", \
    [326] = "copy_file_range", \
    [327] = "preadv2", \
    [328] = "pwritev2", \
    [329] = "pkey_mprotect", \
    [330] = "pkey_alloc", \
    [331] = "pkey_free", \
    [332] = "statx", \
    [333] = "io_pgetevents", \
    [334] = "rseq", \
    [335] = "uretprobe", \
    [336] = "uprobe", \
    [424] = "pidfd_send_signal", \
    [425] = "io_uring_setup", \
    [426] = "io_uring_enter", \
    [427] = "io_uring_register", \
    [428] = "open_tree", \
    [429] = "move_mount", \
    [430] = "fsopen", \
    [431] = "fsconfig", \
    [432] = "fsmount", \
    [433] = "fspick", \
    [434] = "pidfd_open", \
    [435] = "clone3", \
    [436] = "close_range", \
    [437] = "openat2", \
    [438] = "pidfd_getfd", \
    [439] = "faccessat2", \
    [440] = "process_madvise", \
    [441] = "epoll_pwait2", \
    [442] = "mount_setattr", \
    [443] = "quotactl_fd", \
    [444] = "landlock_create_ruleset", \
    [445] = "landlock_add_rule", \
    [446] = "landlock_restrict_self", \
    [447] = "memfd_secret", \
    [448] = "process_mrelease", \
    [449] = "futex_waitv", \
    [450] = "set_mempolicy_home_node", \
    [451] = "cachestat", \
    [452] = "fchmodat2", \
    [453] = "map_shadow_stack", \
    [454] = "futex_wake", \
    [455] = "futex_wait", \
    [456] = "futex_requeue", \
    [457] = "statmount", \
    [458] = "listmount", \
    [459] = "lsm_get_self_attr", \
    [460] = "lsm_set_self_attr", \
    [461] = "lsm_list_modules", \
    [462] = "mseal", \
    [463] = "setxattrat", \
    [464] = "getxattrat", \
    [465] = "listxattrat", \
    [466] = "removexattrat", \
    [467] = "open_tree_attr", \
    [468] = "file_getattr", \
    [469] = "file_setattr", \
    [512] = "rt_sigaction", \
    [513] = "rt_sigreturn", \
    [514] = "ioctl", \
    [515] = "readv", \
    [516] = "writev", \
    [517] = "recvfrom", \
    [518] = "sendmsg", \
    [519] = "recvmsg", \
    [520] = "execve", \
    [521] = "ptrace", \
    [522] = "rt_sigpending", \
    [523] = "rt_sigtimedwait", \
    [524] = "rt_sigqueueinfo", \
    [525] = "sigaltstack", \
    [526] = "timer_create", \
    [527] = "mq_notify", \
    [528] = "kexec_load", \
    [529] = "waitid", \
    [530] = "set_robust_list", \
    [531] = "get_robust_list", \
    [532] = "vmsplice", \
    [533] = "move_pages", \
    [534] = "preadv", \
    [535] = "pwritev", \
    [536] = "rt_tgsigqueueinfo", \
    [537] = "recvmmsg", \
    [538] = "sendmmsg", \
    [539] = "process_vm_readv", \
    [540] = "process_vm_writev", \
    [541] = "setsockopt", \
    [542] = "getsockopt", \
    [543] = "io_setup", \
    [544] = "io_submit", \
    [545] = "execveat", \
    [546] = "preadv2", \
    [547] = "pwritev2", \
}

#endif
