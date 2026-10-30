// SPDX-License-Identifier: MIT
#include "../../games/totk/runtime/runtime.h"
#include "../../games/totk/runtime/autoload.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static _Atomic unsigned presses;
uint64_t runtime_now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
void runtime_input_key_synthetic(unsigned key,int down){if(key!=38)abort();if(down)++presses;}
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"%s\n",message);exit(1);}
static void require(int value){if(!value)abort();}
int main(int argc,char **argv){
    require(argc==2);char status[256];
    runtime_autoload_snapshot(status,sizeof status);require(strstr(status,"state=off")!=NULL);
    // Android enables this after loading the library, unlike desktop env.
    setenv("TOTK_AUTOLOAD","1",1);setenv("TOTK_AUTOLOAD_PERIOD_MS","300",1);setenv("TOTK_AUTOLOAD_LIMIT_S","1",1);
    runtime_autoload_configure();runtime_autoload_configure();
    runtime_autoload_save_file_opened("save:/option.sav");usleep(100000);require(!presses);
    if(!strcmp(argv[1],"nosave"))return 0;
    runtime_autoload_save_file_opened("save:/0/caption.sav");usleep(400000);require(presses>0);
    if(!strcmp(argv[1],"user"))runtime_autoload_user_input();
    else if(!strcmp(argv[1],"load"))runtime_autoload_save_file_opened("save:/0/game_data.sav");
    else {require(!strcmp(argv[1],"timeout"));usleep(900000);}
    usleep(200000);unsigned count=presses;
    runtime_autoload_save_file_opened("save:/1/caption.sav");usleep(400000);require(presses==count);
    runtime_autoload_snapshot(status,sizeof status);require(strstr(status,"state=done")!=NULL);
    require(strstr(status,!strcmp(argv[1],"load")?"stop_reason=1":!strcmp(argv[1],"user")?"stop_reason=2":"stop_reason=3")!=NULL);
    puts(status);return 0;
}
