// SPDX-License-Identifier: MIT
#include "render_record.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <unistd.h>
using namespace mng::port;
static void require(bool ok){if(!ok)throw std::runtime_error("record test failed");}
static std::string read(const std::string& p){std::ifstream in(p);return {std::istreambuf_iterator<char>(in),{}};}
int main(){
    char tmp[]="/tmp/mng-record-XXXXXX";require(mkdtemp(tmp));std::filesystem::path dir(tmp);
    try {
        uint64_t now=1;int starts=0,ends=0;bool available=true;
        RenderRecord r(tmp,"0123456789abcdef0123456789abcdef","test\"\n",{[&](bool start){if(start)++starts;else ++ends;return available;},[&]{return now;}});
        require(!r.action(1,1,1,8,"idle"));require(starts==0);
        require(!r.arm(1,9));require(r.arm(1,2));require(!r.arm(2));
        r.present(10);require(r.active());require(starts==1);
        require(r.action(3,0x1234,7,16,"Draw","x=1"));r.present(11);require(r.active());
        r.action(4,0x1234,8,16,"Copy");r.present(12);require(!r.active()&&ends==1);
        auto original=read(r.path());require(original.find("\"complete\":true")!=std::string::npos);
        require(original.find("\"completed_frames\":2")!=std::string::npos);
        require(original.find("test\\\"\\u000a")!=std::string::npos);
        require(r.arm(1));r.present(20);require(!r.active());require(read(r.path())==original);
        require(r.arm(2,1,8192,1));r.present(30);require(r.action(5,1,2,8,"one"));
        require(!r.action(5,1,2,8,"too_many"));require(read(r.path()).find("event_limit")!=std::string::npos);
        require(r.arm(3,1,8192,10,20));r.present(40);now=30;r.action(6,1,2,8,"late");
        require(read(r.path()).find("record_timeout")!=std::string::npos);
        require(r.arm(4));r.present(50);r.present(52);require(read(r.path()).find("nonconsecutive_present")!=std::string::npos);
        available=false;require(r.arm(5));r.present(60);require(!r.active());require(read(r.path()).find("capture_backend_unavailable")!=std::string::npos);
        available=true;require(r.arm(6,1,8192));r.present(70);
        require(!r.action(7,1,2,8,"large",std::string(8192,'x').c_str()));require(read(r.path()).find("trace_size_limit")!=std::string::npos);
        require(r.arm(7));r.present(80);r.abort("cancelled");require(read(r.path()).find("cancelled")!=std::string::npos);
        std::filesystem::remove_all(dir);std::cout<<"render record: PASS\n";return 0;
    }catch(...){std::filesystem::remove_all(dir);throw;}
}
