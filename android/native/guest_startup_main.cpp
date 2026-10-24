// SPDX-License-Identifier: MIT
#include <csignal>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace mng { [[noreturn]] void startup_probe(const std::string&, const std::string&); }
namespace {
void timeout(int) {
    // Async-signal-safe: no allocation, locking or C++ unwinding.
    static constexpr char report[] = "{\"status\":\"fail\",\"phase\":\"sdk_startup\",\"error\":\"startup probe deadline exceeded\"}\n";
    write(STDOUT_FILENO, report, sizeof(report)-1);
    _exit(4);
}
}
int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: mng_startup_probe package-directory botw160|totk100\n");
        return 2;
    }
    struct sigaction action{};
    action.sa_handler=timeout;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM,&action,nullptr)!=0) return 2;
    alarm(15);
    mng::startup_probe(argv[1],argv[2]);
}
