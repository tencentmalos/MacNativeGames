// SPDX-License-Identifier: MIT
#include "nvn_vulkan.h"
#include <cstdio>
#include <cstring>
#include <vector>
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--list-procs")) {
        std::vector<const char *> names(mng_nvn_proc_names(nullptr, 0));
        mng_nvn_proc_names(names.data(), names.size());
        for (auto *name : names)
            puts(name);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "Pass a private writable shader-cache directory\n");
        return 2;
    }
    char report[2048]{};
    int result = mng_nvn_self_test(argv[1], report, sizeof(report));
    puts(report);
    return result;
}
