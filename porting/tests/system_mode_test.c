// SPDX-License-Identifier: MIT
#include "../../games/totk/runtime/system_mode.h"
#include <stdio.h>
#include <stdlib.h>

static void require(int value) { if (!value) abort(); }
int main(void) {
    RuntimeSystemMode mode;
    require(runtime_parse_system_mode(NULL, NULL, 1, &mode));
    require(mode.operation == 0 && mode.performance == 0 && mode.width == 1280 && mode.height == 720);
    require(runtime_parse_system_mode(NULL, NULL, 0, &mode));
    require(mode.operation == 1 && mode.performance == 1 && mode.width == 1920 && mode.height == 1080);
    require(runtime_parse_system_mode("handheld", "720", 0, &mode));
    require(mode.operation == 0 && mode.performance == 0 && mode.height == 720);
    require(!runtime_parse_system_mode("handheld", "1080", 1, &mode));
    require(mode.height == 720); // Invalid requests do not partially change a profile.
    require(runtime_parse_system_mode("docked", "1080", 1, &mode));
    require(mode.operation == 1 && mode.performance == 1 && mode.height == 1080);
    require(!runtime_parse_system_mode("docked", "720", 0, &mode));
    require(!runtime_parse_system_mode("invalid", NULL, 1, &mode));
    require(!runtime_parse_system_mode(NULL, "900", 1, &mode));
    require(runtime_parse_system_mode(NULL, "720", 0, &mode) && mode.operation == 0);
    require(runtime_parse_system_mode(NULL, "1080", 1, &mode) && mode.operation == 1);
    puts("PASS: handheld/normal/720p and docked/boost/1080p; defaults and inconsistent request rejection");
}
