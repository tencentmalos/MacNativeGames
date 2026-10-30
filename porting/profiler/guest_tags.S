// SPDX-License-Identifier: MIT
// Register-only AAPCS64 functions, independently checked in the title catalog.
// Guest bodies stay on their Guest stack; only begin/end cross the existing HLE
// host-stack/TLS adapter. x18 is never used. No guessed stack argument ABI.
.text
.globl _mng_guest_tag_thunks
.p2align 4
_mng_guest_tag_thunks:
.set guest_tag_index,0
.rept 32
    mov x17,#guest_tag_index
    b guest_tag_call
    nop
    nop
    .set guest_tag_index,guest_tag_index+1
.endr

guest_tag_call:
    adrp x16,_mng_guest_tags_active@PAGE
    add x16,x16,_mng_guest_tags_active@PAGEOFF
    ldar w16,[x16]
    cbnz w16,1f
    adrp x16,_mng_guest_tags_originals@PAGE
    add x16,x16,_mng_guest_tags_originals@PAGEOFF
    ldr x16,[x16,x17,lsl #3]
    br x16
1:  stp x29,x30,[sp,#-240]!
    mov x29,sp
    stp x0,x1,[sp,#16]
    stp x2,x3,[sp,#32]
    stp x4,x5,[sp,#48]
    stp x6,x7,[sp,#64]
    stp x8,x17,[sp,#80]
    stp q0,q1,[sp,#96]
    stp q2,q3,[sp,#128]
    stp q4,q5,[sp,#160]
    stp q6,q7,[sp,#192]
    mov x0,x17
    mov x1,x30
    add x2,sp,#240
    add x3,sp,#16
    adrp x16,_mng_guest_tags_begin_bridge@PAGE
    ldr x16,[x16,_mng_guest_tags_begin_bridge@PAGEOFF]
    blr x16
    str x0,[sp,#224]
    ldp q6,q7,[sp,#192]
    ldp q4,q5,[sp,#160]
    ldp q2,q3,[sp,#128]
    ldp q0,q1,[sp,#96]
    ldp x8,x17,[sp,#80]
    ldp x6,x7,[sp,#64]
    ldp x4,x5,[sp,#48]
    ldp x2,x3,[sp,#32]
    ldp x0,x1,[sp,#16]
    adrp x16,_mng_guest_tags_originals@PAGE
    add x16,x16,_mng_guest_tags_originals@PAGEOFF
    ldr x16,[x16,x17,lsl #3]
    blr x16
    stp x0,x1,[sp,#16]
    stp x2,x3,[sp,#32]
    stp x4,x5,[sp,#48]
    stp x6,x7,[sp,#64]
    str x8,[sp,#80]
    stp q0,q1,[sp,#96]
    stp q2,q3,[sp,#128]
    stp q4,q5,[sp,#160]
    stp q6,q7,[sp,#192]
    ldr x0,[sp,#224]
    adrp x16,_mng_guest_tags_end_bridge@PAGE
    ldr x16,[x16,_mng_guest_tags_end_bridge@PAGEOFF]
    blr x16
    ldp q6,q7,[sp,#192]
    ldp q4,q5,[sp,#160]
    ldp q2,q3,[sp,#128]
    ldp q0,q1,[sp,#96]
    ldr x8,[sp,#80]
    ldp x6,x7,[sp,#64]
    ldp x4,x5,[sp,#48]
    ldp x2,x3,[sp,#32]
    ldp x0,x1,[sp,#16]
    ldp x29,x30,[sp],#240
    ret
