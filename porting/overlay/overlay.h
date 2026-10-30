// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <memory>
namespace mng::overlay {
struct FrameSample {
    uint64_t frame{}, gpu_frame{};
    double frame_ms{}, cpu_process_ms{}, present_ms{};
    std::optional<double> gpu_ms;
    uint64_t draws{}, dispatches{}, pipelines{}, shaders{}, upload_bytes{};
    double pipeline_ms{}, shader_ms{}, upload_ms{};
};
struct Config {
    std::string directory, title, driver, font_path;
    bool chinese{true};
    float density{1.f};
};
// One per host renderer. Only input methods are called from producer threads.
class Overlay {
public:
    Overlay();
    ~Overlay();
    void initialize(const Config& config, unsigned width, unsigned height, bool sdf_capable=false);
    void sdfSubmitted(bool ready);
    void draw(); // host ImGui context and Vulkan backend must already exist
    void presented(const FrameSample& sample);
    void pointer(int phase, float normalized_x, float normalized_y);
    bool key(int android_code, bool down);
    void cancelInput();
    void toggleControls();
    bool capturesInput() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
Overlay& shared();
// Platform supplies real BatteryManager readings; invalid values remain absent.
void battery(int level, int64_t current_ua, int64_t charge_uah, int voltage_mv,
             int temperature_tenths, bool charging);
}
