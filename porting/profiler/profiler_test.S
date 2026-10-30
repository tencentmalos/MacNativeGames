// SPDX-License-Identifier: MIT
.text
.global mng_test_integer
.type mng_test_integer, %function
mng_test_integer:
    adrp x16, mng_test_integer_target
    add x16, x16, :lo12:mng_test_integer_target
    mov x17, #17
    b runtime_hle_profile_call
.global mng_test_float
.type mng_test_float, %function
mng_test_float:
    adrp x16, mng_test_float_target
    add x16, x16, :lo12:mng_test_float_target
    mov x17, #17
    b runtime_hle_profile_call
.section .note.GNU-stack,"",@progbits
