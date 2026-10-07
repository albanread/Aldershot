/* kstat.h -- the stat structure the Unix bridge writes: the Apple Silicon
 * box's runtime is LP64 AArch64 Linux, and its fstatat fills Linux's
 * arm64 struct stat (asm-generic's) straight into the program's memory.
 * ROSGD overlay (design 26): arm64's layout, spelled in widths that do not
 * change with ILP32 (128 bytes). */
struct kstat {
	dev_t st_dev;
	ino_t st_ino;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid;
	gid_t st_gid;
	dev_t st_rdev;
	unsigned long long __pad;
	off_t st_size;
	blksize_t st_blksize;
	int __pad2;
	blkcnt_t st_blocks;
	long long st_atime_sec;
	long long st_atime_nsec;
	long long st_mtime_sec;
	long long st_mtime_nsec;
	long long st_ctime_sec;
	long long st_ctime_nsec;
	unsigned __unused[2];
};
