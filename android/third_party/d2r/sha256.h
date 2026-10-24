// SPDX-License-Identifier: MIT
// Copyright 2026 jhaizhou
// SHA-256 declarations adapted from d2r-mobile 758f5d4.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef uint32_t CC_LONG;
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; uint32_t used; } CC_SHA256_CTX;
int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md);
#ifdef __cplusplus
}
#endif
