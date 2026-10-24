#include <stdio.h>
#include <stdlib.h>

static int doomcpu_putc(char c, FILE *stream)
{
    (void)stream;

    register int a0 asm("a0") = (unsigned char)c;
    register int a7 asm("a7") = 2;

    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a7)
                 : "memory");

    return (unsigned char)c;
}

static FILE doomcpu_stream =
    FDEV_SETUP_STREAM(doomcpu_putc, NULL, NULL, _FDEV_SETUP_WRITE);

FILE *const stdin = &doomcpu_stream;
FILE *const stdout = &doomcpu_stream;
FILE *const stderr = &doomcpu_stream;

void _exit(int status)
{
    register int a0 asm("a0") = status;
    register int a7 asm("a7") = 10;

    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a7)
                 : "memory");

    for (;;) {}
}

int main(void)
{
    int *values = malloc(3 * sizeof(int));

    if (!values) {
        printf("malloc failed\n");
        return 1;
    }

    values[0] = 12;
    values[1] = 13;
    values[2] = 17;

    printf("DoomCPU libc test: %d\n",
           values[0] + values[1] + values[2]);

    free(values);
    return 0;
}
