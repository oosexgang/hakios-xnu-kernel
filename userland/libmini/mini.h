/*
 * libmini: just enough C library for the static test programs of the
 * Darwin bring-up. No dyld, no libSystem: the programs talk to the kernel
 * directly (see start.S).
 */
#ifndef MINI_H
#define MINI_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
typedef long long off_t;
typedef int pid_t;

extern int errno;
extern char **environ;

/* system call numbers (xnu bsd/kern/syscalls.master) */
enum {
	SYS_exit = 1, SYS_read = 3, SYS_write = 4, SYS_open = 5, SYS_close = 6, SYS_wait4 = 7, SYS_link = 9,
	SYS_unlink = 10, SYS_chdir = 12, SYS_chmod = 15, SYS_chown = 16, SYS_getpid = 20, SYS_getuid = 24,
	SYS_chflags = 34, SYS_kill = 37, SYS_getppid = 39, SYS_dup = 41, SYS_ioctl = 54, SYS_reboot = 55,
	SYS_symlink = 57, SYS_readlink = 58, SYS_execve = 59, SYS_msync = 65, SYS_munmap = 73, SYS_dup2 = 90,
	SYS_select = 93, SYS_fsync = 95, SYS_socket = 97, SYS_bind = 104, SYS_gettimeofday = 116,
	SYS_rename = 128, SYS_mkdir = 136, SYS_rmdir = 137, SYS_utimes = 138, SYS_pread = 153, SYS_pwrite = 154,
	SYS_mount = 167, SYS_mmap = 197, SYS_lseek = 199, SYS_ftruncate = 201, SYS_getxattr = 234,
	SYS_setxattr = 236, SYS_listxattr = 240, SYS_sysctlbyname = 274, SYS_stat64 = 338, SYS_fstat64 = 339,
	SYS_lstat64 = 340, SYS_getdirentries64 = 344, SYS_lchown = 364,
};

/*
 * Not variadic on purpose: the Apple arm64 ABI passes variadic arguments on
 * the stack, and the kernel wants them in registers.
 */
long mini_syscall(long number, long a, long b, long c, long d, long e, long f);
pid_t fork(void);
int pipe(int fds[2]);

#define O_RDONLY        0
#define O_WRONLY        1
#define O_RDWR          2
#define O_APPEND        0x8
#define O_CREAT         0x200
#define O_TRUNC         0x400
#define O_EXCL          0x800
#define O_DIRECTORY     0x100000

#define AF_UNIX         1
#define SOCK_STREAM     1

/* local socket address */
struct sockaddr_un {
	uint8_t         sun_len;
	uint8_t         sun_family;
	char            sun_path[104];
};

#define SEEK_SET        0
#define SEEK_END        2

#define PROT_READ       1
#define PROT_WRITE      2
#define MAP_SHARED      1
#define MAP_PRIVATE     2
#define MAP_ANON        0x1000
#define MAP_FAILED      ((void *)-1)

#define MNT_RDONLY      1
#define MNT_UNION       0x00000020      /* what is not in the mounted directory is found in the one below */

#define S_IFMT          0170000
#define S_IFDIR         0040000
#define S_IFCHR         0020000
#define S_IFBLK         0060000
#define S_IFREG         0100000
#define S_IFLNK         0120000
#define S_IFSOCK        0140000
#define S_IFIFO         0010000

#define XATTR_NOFOLLOW  0x0001

#define DT_DIR          4
#define DT_REG          8
#define DT_LNK          10

struct timespec { long tv_sec; long tv_nsec; };
struct timeval { long tv_sec; int tv_usec; };

/* struct stat as stat64() fills it in */
struct stat {
	int32_t         st_dev;
	uint16_t        st_mode;
	uint16_t        st_nlink;
	uint64_t        st_ino;
	uint32_t        st_uid;
	uint32_t        st_gid;
	int32_t         st_rdev;
	struct timespec st_atimespec;
	struct timespec st_mtimespec;
	struct timespec st_ctimespec;
	struct timespec st_birthtimespec;
	off_t           st_size;
	int64_t         st_blocks;
	int32_t         st_blksize;
	uint32_t        st_flags;
	uint32_t        st_gen;
	int32_t         st_lspare;
	int64_t         st_qspare[2];
};

/* directory entry as getdirentries64() returns it */
struct dirent {
	uint64_t        d_ino;
	uint64_t        d_seekoff;
	uint16_t        d_reclen;
	uint16_t        d_namlen;
	uint8_t         d_type;
	char            d_name[1024];
};

#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)

void    _exit(int status) __attribute__((noreturn));
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     open(const char *path, int flags, int mode);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);
int     dup2(int from, int to);
int     chdir(const char *path);
pid_t   getpid(void);
pid_t   getppid(void);
int     getuid(void);
pid_t   wait4(pid_t pid, int *status, int options, void *rusage);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     stat(const char *path, struct stat *st);
int     lstat(const char *path, struct stat *st);
int     fstat(int fd, struct stat *st);
ssize_t readlink(const char *path, char *buf, size_t size);
ssize_t getdirentries(int fd, void *buf, size_t size, off_t *position);
void   *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int     munmap(void *addr, size_t length);
int     gettimeofday(struct timeval *tv, void *tz);
int     usleep(unsigned microseconds);
int     sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);
int     reboot(int howto);
int     mount(const char *type, const char *dir, int flags, void *data);
int     mkdir(const char *path, int mode);
int     rmdir(const char *path);
int     unlink(const char *path);
int     rename(const char *from, const char *to);
int     link(const char *from, const char *to);
int     symlink(const char *target, const char *path);
int     chmod(const char *path, int mode);
int     chown(const char *path, int uid, int gid);
int     lchown(const char *path, int uid, int gid);
int     chflags(const char *path, int flags);
int     utimes(const char *path, const struct timeval times[2]);
ssize_t listxattr(const char *path, char *names, size_t size, int options);
ssize_t getxattr(const char *path, const char *name, void *value, size_t size, uint32_t position, int options);
int     setxattr(const char *path, const char *name, const void *value, size_t size, uint32_t position, int options);
int     ftruncate(int fd, off_t length);
int     fsync(int fd);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
int     socket(int domain, int type, int protocol);
int     bind(int fd, const void *address, unsigned length);

size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy(char *dst, const char *src);
char   *strchr(const char *s, int c);
void   *memcpy(void *dst, const void *src, size_t n);
void   *memset(void *dst, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
const char *strerror(int error);

int     printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int     dprintf(int fd, const char *format, ...) __attribute__((format(printf, 2, 3)));
int     vdprintf(int fd, const char *format, va_list ap);
int     puts(const char *s);

int     main(int argc, char **argv, char **envp);

#endif /* MINI_H */
