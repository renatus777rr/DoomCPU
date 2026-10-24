#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>

static int doomcpu_putc(char ch, FILE *stream)
{
    (void)stream;
    register int a0 asm("a0") = (unsigned char)ch;
    register int a7 asm("a7") = 2;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (unsigned char)ch;
}

static FILE doomcpu_stream =
    FDEV_SETUP_STREAM(doomcpu_putc, NULL, NULL, _FDEV_SETUP_WRITE);

FILE *const stdin = &doomcpu_stream;
FILE *const stdout = &doomcpu_stream;
FILE *const stderr = &doomcpu_stream;

static long doomcpu_call3(unsigned number,
                          uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    register uint32_t a0 asm("a0") = arg0;
    register uint32_t a1 asm("a1") = arg1;
    register uint32_t a2 asm("a2") = arg2;
    register uint32_t a7 asm("a7") = number;

    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a7)
                 : "memory");

    int32_t result = (int32_t)a0;
    if (result < 0 && result >= -4095) {
        errno = -result;
        return -1;
    }
    return result;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    return (int)doomcpu_call3(11, (uint32_t)(uintptr_t)path,
                              (uint32_t)flags, (uint32_t)mode);
}

int close(int fd)
{
    return (int)doomcpu_call3(12, (uint32_t)fd, 0, 0);
}

ssize_t read(int fd, void *buffer, size_t count)
{
    if (count > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    return (ssize_t)doomcpu_call3(13, (uint32_t)fd,
                                  (uint32_t)(uintptr_t)buffer,
                                  (uint32_t)count);
}

ssize_t write(int fd, const void *buffer, size_t count)
{
    if (count > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    return (ssize_t)doomcpu_call3(14, (uint32_t)fd,
                                  (uint32_t)(uintptr_t)buffer,
                                  (uint32_t)count);
}

off_t lseek(int fd, off_t offset, int whence)
{
    return (off_t)doomcpu_call3(15, (uint32_t)fd,
                                (uint32_t)offset, (uint32_t)whence);
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    return (int)doomcpu_call3(16, (uint32_t)(uintptr_t)tv, 0, 0);
}

int clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
    return (int)doomcpu_call3(16, (uint32_t)(uintptr_t)ts, 1, 0);
}

int mkdir(const char *path, mode_t mode)
{
    return (int)doomcpu_call3(17, (uint32_t)(uintptr_t)path,
                              (uint32_t)mode, 0);
}

int unlink(const char *path)
{
    return (int)doomcpu_call3(18, (uint32_t)(uintptr_t)path, 0, 0);
}

int rename(const char *oldpath, const char *newpath)
{
    return (int)doomcpu_call3(19, (uint32_t)(uintptr_t)oldpath,
                              (uint32_t)(uintptr_t)newpath, 0);
}

int isatty(int fd)
{
    return fd >= 0 && fd <= 2;
}

void _exit(int status)
{
    register int a0 asm("a0") = status;
    register int a7 asm("a7") = 10;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    for (;;) {}
}
