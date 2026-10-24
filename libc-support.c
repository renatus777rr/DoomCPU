#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>

static unsigned char output_buffer[128];

static FILE output_stream = {
    ._flags = __SNBF,
    ._bf = { ._base = output_buffer, ._size = sizeof output_buffer },
    ._write = NULL,
    ._read = NULL,
    ._seek = NULL,
    ._close = NULL,
    ._cookie = NULL
};

FILE *stdout = &output_stream;

void _exit(int status)
{
    register int a0 asm("a0") = status;
    register int a7 asm("a7") = 10;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    for (;;) {}
}

int _write(int fd, const void *buf, size_t count)
{
    (void)fd;
    const unsigned char *p = buf;

    for (size_t i = 0; i < count; i++) {
        register int a0 asm("a0") = p[i];
        register int a7 asm("a7") = 2;
        asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    }

    return (int)count;
}
