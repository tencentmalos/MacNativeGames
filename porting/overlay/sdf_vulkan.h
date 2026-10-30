// SPDX-License-Identifier: MIT
#pragma once
#include <volk.h>
struct ImTextureData;
namespace mng::overlay {
VkPipeline createSdfPipeline(VkDevice device, VkRenderPass pass);
// Preserves ImGui's bitmap source; only renderer uploads use the SDF bytes.
bool updateSdfTexture(ImTextureData* texture);
}
