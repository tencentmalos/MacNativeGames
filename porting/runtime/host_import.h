// SPDX-License-Identifier: MIT
#pragma once
#if defined(__APPLE__)
#define MNG_OPTIONAL_IMPORT __attribute__((weak_import))
#else
#define MNG_OPTIONAL_IMPORT __attribute__((weak))
#endif
