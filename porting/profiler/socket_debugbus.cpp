// SPDX-License-Identifier: MIT
#include "socket_debugbus.h"
#include "debugbus.h"
#include "../runtime/host_thread.h"
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace mng::diagnostics {
struct SocketDebugBus::Impl {
    int socket{-1};std::string path;std::atomic<bool> stop{};std::thread worker;
    explicit Impl(std::string value):path(std::move(value)) {
        sockaddr_un address{};address.sun_family=AF_UNIX;
        if(path.empty() || path.size()>=sizeof address.sun_path)throw std::runtime_error("debugbus socket path limit");
        memcpy(address.sun_path,path.c_str(),path.size()+1);
        socket=::socket(AF_UNIX,SOCK_STREAM,0);
        if(socket<0)throw std::runtime_error("debugbus socket creation failed");
        if(bind(socket,reinterpret_cast<sockaddr*>(&address),sizeof address)){
            close(socket);throw std::runtime_error("debugbus bind failed; existing paths are never replaced");
        }
        if(chmod(path.c_str(),0600) || listen(socket,8)){
            close(socket);unlink(path.c_str());throw std::runtime_error("debugbus permissions/listen failed");
        }
        try{worker=std::thread([this]{run();});}catch(...){close(socket);unlink(path.c_str());throw;}
    }
    ~Impl(){stop=true;worker.join();close(socket);unlink(path.c_str());}
    void run(){
        mng_name_thread("mng-debugbus");
        while(!stop){pollfd ready{socket,POLLIN,0};if(poll(&ready,1,100)<=0)continue;
            int client=accept(socket,nullptr,nullptr);if(client<0)continue;
            uid_t uid;gid_t gid;
            if(getpeereid(client,&uid,&gid) || uid!=geteuid()){close(client);continue;}
            timeval timeout{0,250000};setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
            setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
            int no_sigpipe=1;setsockopt(client,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof no_sigpipe);
            std::string request;bool complete=false;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
            while(request.size()<=256){
                const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
                if(left<=0 || stop)break;
                pollfd input{client,POLLIN,0};if(poll(&input,1,int(left))<=0)break;
                char ch;auto n=recv(client,&ch,1,MSG_DONTWAIT);if(n!=1)break;
                if(ch=='\n'){complete=true;break;}if(!ch)break;request+=ch;
            }
            std::string reply;
            try{reply=complete?command(request):"error=incomplete_or_excessive_request\n";}
            catch(const std::exception&){reply="error=debugbus_command_failed\n";}
            size_t sent=0;
            const auto send_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
            while(sent<reply.size()){
                const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(send_deadline-std::chrono::steady_clock::now()).count();
                if(left<=0 || stop)break;
                pollfd output{client,POLLOUT,0};if(poll(&output,1,int(left))<=0)break;
                auto n=send(client,reply.data()+sent,reply.size()-sent,MSG_DONTWAIT);if(n<=0)break;sent+=size_t(n);
            }
            close(client);
        }
    }
};
SocketDebugBus::SocketDebugBus(const std::string& path):impl_(std::make_unique<Impl>(path)){}
SocketDebugBus::~SocketDebugBus()=default;
}
