#include <stdint.h>
#include "doomgeneric.h"

static inline void doomcpu_ecall3(unsigned service,
                                  uintptr_t arg0,
                                  uintptr_t arg1,
                                  uintptr_t arg2)
{
    register uintptr_t a0 asm("a0") = arg0;
    register uintptr_t a1 asm("a1") = arg1;
    register uintptr_t a2 asm("a2") = arg2;
    register uintptr_t a7 asm("a7") = service;

    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a7)
                 : "memory");
}

void DG_Init(void)
{
    /* Initialize host display; a null framebuffer means "no frame yet". */
    doomcpu_ecall3(20, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_DrawFrame(void)
{
    doomcpu_ecall3(20, (uintptr_t)DG_ScreenBuffer,
                   DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_SleepMs(uint32_t ms)
{
    doomcpu_ecall3(22, ms, 0, 0);
}

uint32_t DG_GetTicksMs(void)
{
    register uintptr_t a0 asm("a0") = 0;
    register uintptr_t a7 asm("a7") = 23;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (uint32_t)a0;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    register uintptr_t a0 asm("a0") = (uintptr_t)pressed;
    register uintptr_t a1 asm("a1") = (uintptr_t)key;
    register uintptr_t a7 asm("a7") = 21;

    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a7)
                 : "memory");
    return (int)a0;
}

void DG_SetWindowTitle(const char *title)
{
    doomcpu_ecall3(24, (uintptr_t)title, 0, 0);
}
