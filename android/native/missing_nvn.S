/* SPDX-License-Identifier: MIT
 * Fixed diagnostic stubs for unresolved NVN functions. These stop only when
 * called, identifying the requested API without executing a null pointer.
 * runtime_hle_wrap_named supplies the guest-to-host stack switch.
 */
.text
.globl mng_missing_nvn_thunks
.hidden mng_missing_nvn_thunks
.p2align 4
mng_missing_nvn_thunks:
.set missing_i,0
.rept 1024
    mov x0,#missing_i
    b mng_missing_nvn_called
    brk #0xb5
    brk #0xb5
.set missing_i,missing_i+1
.endr
.section .note.GNU-stack,"",@progbits
