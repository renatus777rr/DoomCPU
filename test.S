.section .text
.globl _start

_start:
    li a0, 20
    li a1, 22
    add a0, a0, a1

    # DoomCPU host service: print integer
    li a7, 1
    ecall

    # Exit
    li a0, 0
    li a7, 10
    ecall

1:
    j 1b
