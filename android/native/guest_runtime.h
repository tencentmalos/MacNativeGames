// SPDX-License-Identifier: MIT
#pragma once
#include <string>

namespace mng {
// Runs main PLT -> SDK -> timer gate and original SDK SleepThread on four
// isolated guest stacks. Constructors and game entry are not invoked.
std::string probe_modules(const std::string& directory, const std::string& title);
// Validate, relocate and check mapping permissions without executing guest code.
std::string validate_modules(const std::string& directory, const std::string& title);
}
