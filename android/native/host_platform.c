// SPDX-License-Identifier: MIT
// Android host integration for the shared MacNativeGames native services.
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void runtime_boot_progress(const char *s,uint64_t done,uint64_t total) {
    fprintf(stderr,"[android-boot] %s %llu/%llu\n",s,(unsigned long long)done,(unsigned long long)total);
}
void runtime_startup_checkpoint(uintptr_t caller) { (void)caller; }
// Optional output overrides are disabled; retain the game's own output settings.
int runtime_output_res(unsigned *w,unsigned *h) { (void)w;(void)h;return 0; }
int runtime_render_res(unsigned *w,unsigned *h) { (void)w;(void)h;return 0; }
uint64_t runtime_output_res_extra_bytes(void) {return 0;}
void dm_guest_write_prepare(void *p,size_t n) { (void)p;(void)n; }
// These modules are not linked into the startup executable. Returning NULL
// leaves their SDK functions intact; unsupported calls stop in the IPC layer.


void *runtime_codec_override(const char *s) { (void)s;return NULL; }
void *runtime_movie_override(const char *s) { (void)s;return NULL; }
void *runtime_swkbd_override(const char *s) { (void)s;return NULL; }


void runtime_autoload_file(const char *p) { (void)p; }
void runtime_boot_status(const char *stage,const char *detail) {fprintf(stderr,"[android-boot] %s: %s\n",stage,detail);}
