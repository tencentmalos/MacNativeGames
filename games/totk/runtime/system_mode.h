// SPDX-License-Identifier: MIT
#pragma once
#include <string.h>

// One immutable launch profile for OE, HID and the host output. Values match
// Citron's nn::oe OperationMode and PerformanceMode definitions. The game's
// dynamic resolution remains independent of this nominal output size.
typedef struct RuntimeSystemMode {
    const char *name;
    int operation, performance, width, height;
} RuntimeSystemMode;

static inline int runtime_parse_system_mode(const char *name, const char *resolution,
                                          int default_handheld, RuntimeSystemMode *out) {
    int handheld = default_handheld;
    if (name) {
        if (!strcmp(name, "handheld")) handheld = 1;
        else if (!strcmp(name, "docked")) handheld = 0;
        else return 0;
    }
    if (resolution) {
        int selected;
        if (!strcmp(resolution, "720")) selected = 1;
        else if (!strcmp(resolution, "1080")) selected = 0;
        else return 0;
        if (name && selected != handheld) return 0;
        handheld = selected;
    }
    RuntimeSystemMode result;
    result.name = handheld ? "handheld" : "docked";
    result.operation = result.performance = handheld ? 0 : 1;
    result.width = handheld ? 1280 : 1920;
    result.height = handheld ? 720 : 1080;
    *out = result;
    return 1;
}

#ifdef __cplusplus
extern "C" {
#endif
const RuntimeSystemMode *runtime_system_mode(void);
int runtime_system_mode_snapshot(char *out, unsigned bytes);
#ifdef __cplusplus
}
#endif
