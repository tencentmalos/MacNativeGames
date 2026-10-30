// SPDX-License-Identifier: GPL-2.0-or-later
// Foundation owns device state; the native game consumes the shared Switch layout.
#include "input.h"
extern "C" {
[[noreturn]] void runtime_fail(const char*);
void runtime_gamepad_clear() {mng::input.clear();}
void runtime_rumble_set(unsigned,float,float,float,float) {runtime_fail("Android rumble transport is not connected");}
void runtime_motion_start() {}
int runtime_motion_read(void*,int) {return 0;}
}
