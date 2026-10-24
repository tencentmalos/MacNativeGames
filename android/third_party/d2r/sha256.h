// SPDX-License-Identifier: MIT
// Copyright 2026 jhaizhou
// SHA-256 declarations adapted from d2r-mobile 758f5d4.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef uint32_t mng_port_sha_size;
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; uint32_t used; } mng_port_sha256_CTX;
int mng_port_sha256_Init(mng_port_sha256_CTX *c);
int mng_port_sha256_Update(mng_port_sha256_CTX *c, const void *data, mng_port_sha_size len);
int mng_port_sha256_Final(unsigned char *md, mng_port_sha256_CTX *c);
unsigned char *mng_port_sha256(const void *data, mng_port_sha_size len, unsigned char *md);
#ifdef __cplusplus
}
#endif
