// SPDX-License-Identifier: MIT
// Included inside the shared renderer namespace. Controls are changed only
// while holding its mutex, including during same-process A/B measurement.
namespace mng_translate {
uint64_t pipeline_early_hits=0,pipeline_state_builds=0;
bool &preparation_plan(){static bool on=[] {const char *v=getenv("MNG_PREPARATION_PLAN");return !v || strcmp(v,"0");}();return on;}
bool &pipeline_early(){static bool on=[] {const char *v=getenv("MNG_PIPELINE_EARLY");return !v || strcmp(v,"0");}();return on;}
bool &descriptor_plan(){static bool on=[] {const char *v=getenv("MNG_DESCRIPTOR_PLAN");return !v || strcmp(v,"0");}();return on;}
bool &image_fast(){static bool on=[] {const char *v=getenv("MNG_IMAGE_READ_FAST");return !v || strcmp(v,"0");}();return on;}
}
