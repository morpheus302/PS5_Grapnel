/* (ps5) win32_posix.c's file calls through host/posix/ps5_files.c, which lifts the console's limit of 249 files open by path
   (PS5_PayloadSDK platform/docs/PROBE.md "Open files"; the game opens a host descriptor per guest file, nn_fs.c). Included
   by win32_posix.c after every system header, so that only its own calls are renamed: CreateFileA, ReadFile, WriteFile,
   SetFilePointerEx, GetFileSizeEx, SetEndOfFile, FlushFileBuffers, CloseHandle and CopyFileA reach the descriptor through
   ps5platform/fdv.h's enter and leave, as fdv.h asks of every call that takes one; CreateDirectoryA's mkdir makes
   folders under /data 0777. */
#ifndef HOOKSHOT_POSIX_PS5_FDV_HOOKS_H
#define HOOKSHOT_POSIX_PS5_FDV_HOOKS_H
#ifdef __PROSPERO__
#include <sys/stat.h>
#include <sys/types.h>

int hos_ps5_fd_open(const char *path, int flags, ...);
int hos_ps5_fd_close(int fd);
ssize_t hos_ps5_fd_read(int fd, void *buffer, size_t n);
ssize_t hos_ps5_fd_write(int fd, const void *buffer, size_t n);
ssize_t hos_ps5_fd_pread(int fd, void *buffer, size_t n, off_t at);
ssize_t hos_ps5_fd_pwrite(int fd, const void *buffer, size_t n, off_t at);
off_t hos_ps5_fd_lseek(int fd, off_t at, int whence);
int hos_ps5_fd_fstat(int fd, struct stat *st);
int hos_ps5_fd_ftruncate(int fd, off_t size);
int hos_ps5_fd_fdatasync(int fd);
int hos_ps5_mkdir(const char *path, mode_t mode);

#define open(...) hos_ps5_fd_open(__VA_ARGS__)
#define close(fd) hos_ps5_fd_close(fd)
#define read(fd, b, n) hos_ps5_fd_read(fd, b, n)
#define write(fd, b, n) hos_ps5_fd_write(fd, b, n)
#define pread(fd, b, n, at) hos_ps5_fd_pread(fd, b, n, at)
#define pwrite(fd, b, n, at) hos_ps5_fd_pwrite(fd, b, n, at)
#define lseek(fd, at, whence) hos_ps5_fd_lseek(fd, at, whence)
#define fstat(fd, st) hos_ps5_fd_fstat(fd, st)
#define ftruncate(fd, size) hos_ps5_fd_ftruncate(fd, size)
#define fdatasync(fd) hos_ps5_fd_fdatasync(fd)
#define mkdir(path, mode) hos_ps5_mkdir(path, mode)
#endif
#endif
