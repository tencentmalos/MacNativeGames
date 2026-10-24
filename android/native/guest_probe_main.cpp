// SPDX-License-Identifier: MIT
#include "guest_runtime.h"
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=3){std::cerr<<"usage: mng_runtime_probe package-directory botw160|totk100\n";return 2;}
    const auto report=mng::probe_modules(argv[1],argv[2]);std::cout<<report<<'\n';
    return report.find("\"status\":\"pass\"")==std::string::npos;
}
