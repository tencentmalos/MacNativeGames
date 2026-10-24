/* SPDX-License-Identifier: MIT
 * Call an AAPCS64 guest function on its own guarded stack. The platform x18 is
 * never changed. Host callee-saved registers and the original SP survive return.
 */
.text
.globl mng_call_guest
.hidden mng_call_guest
.type mng_call_guest,%function
.p2align 2
mng_call_guest:
    sub sp, sp, #160
    stp x19, x20, [sp]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x19, sp
    mov sp, x1
    mov x29, xzr
    mov x16, x0
    mov x0, x2
    blr x16
    mov sp, x19
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret
.size mng_call_guest,.-mng_call_guest
.section .note.GNU-stack,"",@progbits
