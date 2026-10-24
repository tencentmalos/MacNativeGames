/* SPDX-License-Identifier: MIT
 * Diagnostic-only AAPCS64 entry with eight arguments and a private guest stack.
 * The SDK startup entry never returns during a successful game session.
 */
.text
.globl mng_call_guest_startup
.hidden mng_call_guest_startup
.type mng_call_guest_startup,%function
.p2align 2
mng_call_guest_startup:
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
    mov x17, x2
    ldp x0, x1, [x17]
    ldp x2, x3, [x17, #16]
    ldp x4, x5, [x17, #32]
    ldp x6, x7, [x17, #48]
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
.size mng_call_guest_startup,.-mng_call_guest_startup
// The first SDK loader notification has no work in the native Mac loader.
// A leaf guest callback avoids entering host C++ on the guest stack.
.globl mng_guest_loader_ready
.hidden mng_guest_loader_ready
.type mng_guest_loader_ready,%function
.p2align 2
mng_guest_loader_ready:
    ret
.size mng_guest_loader_ready,.-mng_guest_loader_ready
.section .note.GNU-stack,"",@progbits
