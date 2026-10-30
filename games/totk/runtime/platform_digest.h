/* SPDX-License-Identifier: MIT */
#pragma once
#ifdef __ANDROID__
#include "sha256.h"
#define CC_SHA256 mng_port_sha256
#define CC_SHA256_CTX mng_port_sha256_CTX
#define CC_SHA256_Init mng_port_sha256_Init
#define CC_SHA256_Update mng_port_sha256_Update
#define CC_SHA256_Final mng_port_sha256_Final
#define CC_LONG mng_port_sha_size
#else
#include <CommonCrypto/CommonDigest.h>
#endif
