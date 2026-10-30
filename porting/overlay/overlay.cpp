// SPDX-License-Identifier: MIT
#include "overlay.h"
#include <spatial/imgui/overlay/OverlayShell.hpp>
#include <spatial/imgui/overlay/OverlayComponents.hpp>
#include <spatial/imgui/overlay/OverlayTheme.hpp>
#include <spatial/imgui/overlay/PerfHud.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <map>
#include <cmath>
namespace mng::overlay {
namespace ov=spatial::imgui::overlay;
namespace si=spatial::imgui;
namespace perf=spatial::perf;
using Clock=std::chrono::steady_clock;
struct Overlay::Impl : ov::IntentSink {
    Config config;
    ov::OverlayShell shell;
    ov::OverlayTheme theme;
    ov::PresentationEnvironment environment;
    ov::PerfHud hud;
    perf::DeviceMetricsSampler sampler;
    perf::FrameRateCounter fps;
    perf::PerfHudSettings settings=perf::defaultPerfHudSettings();
    ov::TextEditSession text_session;
    ov::ImeDialogState ime;
    FrameSample sample;
    ov::MetricSeries frame_graph;
    std::atomic<bool> ready{},capture{},toggle{},cancel{},back{};
    std::atomic<uint64_t> sequence{};
    uint64_t seen{};
    Clock::time_point last_draw{};
    bool timings=true,compilation=false,frame_plot=true;
    std::string cpu_source="--";
    void submit(const ov::OverlayIntent& intent) override {shell.controller().submitIntent(intent);}
    void emit(const ov::OverlayCommand& command) override {shell.controller().emitCommand(command);}
    std::string tr(const char* zh,const char* en) const {return config.chinese?zh:en;}
    std::string translate(const ov::LocalizedText& t) const {
        if(!config.chinese)return t.fallback;
        static const std::map<std::string,std::string> translations={
            {"Performance HUD","性能状态层"},{"Layout","布局"},{"Simple","仅 FPS"},{"Horizontal","横向"},{"Vertical","纵向"},
            {"Preset","预设"},{"FPS only","仅 FPS"},{"Essential","基本"},{"Battery","电池"},{"Full","完整"},{"Custom","自定义"},
            {"Rows","显示项目"},{"Graphs","曲线"},{"Appearance","外观"},{"Background opacity","背景不透明度"},
            {"Colour intensity","颜色强度"},{"Color intensity","颜色强度"},{"Text outline","文字描边"},
            {"Controls","控制"},{"Control panel","控制面板"},{"Control Panel","控制面板"},{"Details","详细信息"},{"Detail","详细信息"},
            {"Close","关闭"},{"Back","返回"},{"Apply","应用"},{"Cancel","取消"},{"Status","状态"},
            {"RAM","内存"},{"BAT","电池"},{"PWR","功耗"},{"LEFT","续航"},{"TIME","时间"},
            {"CPU TEMP","CPU 温度"},{"GPU TEMP","GPU 温度"},{"BAT TEMP","电池温度"},
            {"FPS graph","FPS 曲线"},{"CPU graph","CPU 曲线"},{"GPU graph","GPU 曲线"},{"The row is hidden","此项目已隐藏"}
        };
        auto it=translations.find(t.fallback);return it==translations.end()?t.fallback:it->second;
    }
    void save() {
        nlohmann::json json={{"version",1},{"shell",ov::serializeOverlayState(shell.persistentState())},
            {"perf",perf::serializePerfHudSettings(settings)},{"timings",timings},{"compilation",compilation},{"frame_plot",frame_plot}};
        const auto path=config.directory+"/overlay.json",temporary=path+".part";
        std::ofstream file(temporary);file<<json.dump(2);file.close();
        if(file) {std::error_code error;std::filesystem::rename(temporary,path,error);if(error)std::fprintf(stderr,"[overlay] save failed: %s\n",error.message().c_str());}
    }
    void load() {
        ov::OverlayPersistentState persistent;
        persistent.setStatusModeFor(ov::PresentationKind::Flat,ov::StatusMode::Summary);
        persistent.setStatusOrientationFor(ov::PresentationKind::Flat,ov::StatusOrientation::Horizontal);
        try {
            std::ifstream file(config.directory+"/overlay.json");
            if(file) {auto j=nlohmann::json::parse(file);if(j.value("version",0)==1) {
                (void)ov::deserializeOverlayState(j.value("shell",std::string{}),persistent);
                (void)perf::deserializePerfHudSettings(j.value("perf",std::string{}),settings);
                timings=j.value("timings",true);compilation=j.value("compilation",false);frame_plot=j.value("frame_plot",true);
            }}
        } catch(const std::exception& e){std::fprintf(stderr,"[overlay] settings ignored: %s\n",e.what());}
        shell.restorePersistentState(persistent);
    }
    ov::OverlaySnapshot snapshot() {
        ov::OverlaySnapshot out;
        hud.appendSummary(out.status,settings);
        auto add=[&](const char* id,const char* label,std::string value) {
            ov::SummaryItem item;item.id=ov::StableId(id);item.label=ov::LocalizedText(label);item.value=std::move(value);
            out.status.summary_items.push_back(std::move(item));
        };
        if(timings) {
            add("host.frame",config.chinese?"帧间隔":"Frame",fmt::format("{:.1f} ms",sample.frame_ms));
            if(frame_plot) {ov::SummarySparkline line;line.series=frame_graph;line.minimum_axis_maximum=34;out.status.summary_items.back().sparkline=std::move(line);}
            add("host.gpu",config.chinese?"GPU 帧":"GPU frame",sample.gpu_ms?fmt::format("{:.1f} ms",*sample.gpu_ms):"--");
        }
        if(compilation) add("host.build",config.chinese?"管线构建":"Pipeline build",fmt::format("{} / {:.1f} ms",sample.pipelines,sample.pipeline_ms));
        ov::DetailSection section;section.id=ov::StableId("host.render");section.title=ov::LocalizedText(tr("渲染与提交","Rendering and presentation"));
        auto property=[&](std::string label,std::string value) {ov::DetailProperty p;p.label=ov::LocalizedText(label);p.value=std::move(value);section.properties.push_back(std::move(p));};
        property(tr("游戏","Game"),config.title);property(tr("GPU 驱动","GPU driver"),config.driver);
        property(tr("已呈现帧","Presented frame"),std::to_string(sample.frame));
        property(tr("帧间隔（含等待）","Frame interval (including waits)"),fmt::format("{:.2f} ms",sample.frame_ms));
        property(tr("进程 CPU 线程合计","Process CPU time, summed threads"),fmt::format("{:.2f} ms",sample.cpu_process_ms));
        property(tr("CPU 利用率来源","CPU utilization source"),cpu_source);
        property(tr("GPU 利用率来源","GPU utilization source"),tr("设备全局 sysfs；不是本游戏独占","Device-global sysfs; not game-exclusive"));
        property(tr("GPU 时间戳区间合计","GPU timestamp interval sum"),sample.gpu_ms?fmt::format("{:.2f} ms · frame {}",*sample.gpu_ms,sample.gpu_frame):tr("尚无已回收查询","No retired query available"));
        property(tr("GPU 计时范围","GPU timing scope"),tr("NVN 提交，延迟 8 帧；不含 HUD/最终合成","NVN submissions, delayed 8 frames; excludes HUD/final composite"));
        property(tr("呈现调用（含等待）","Host presentation (including waits)"),fmt::format("{:.2f} ms",sample.present_ms));
        property("Draw / Dispatch",fmt::format("{} / {}",sample.draws,sample.dispatches));
        property(tr("管线构建","Pipeline build"),fmt::format("{} / {:.2f} ms",sample.pipelines,sample.pipeline_ms));
        property(tr("Shader 加载","Shader loading"),fmt::format("{} / {:.2f} ms",sample.shaders,sample.shader_ms));
        property(tr("纹理上传","Texture upload"),fmt::format("{:.2f} MiB / {:.2f} ms",double(sample.upload_bytes)/1048576,sample.upload_ms));
        section.graphs.push_back(frame_graph);out.status.detail_sections.push_back(std::move(section));
        auto& c=shell.controller();out.controls.pages.push_back(ov::makePerfHudControlPage(settings,c.statusMode(),c.statusOrientation()));
        ov::ControlPage page;page.id=ov::StableId("host.metrics");page.title=ov::LocalizedText(tr("渲染统计","Renderer metrics"));
        auto control=[&](const char* id,std::string label,bool value) {ov::ControlDescriptor d;d.id=ov::ControlId(id);d.action=ov::ActionId(id);d.kind=ov::ControlKind::Toggle;d.label=ov::LocalizedText(label);d.bool_value=value;page.controls.push_back(std::move(d));};
        control("host.timings",tr("显示帧间隔 / GPU 时间","Show frame interval / GPU time"),timings);
        control("host.frame_plot",tr("显示帧间隔曲线","Show frame interval graph"),frame_plot);
        control("host.compilation",tr("显示管线构建耗时","Show pipeline build time"),compilation);
        ov::ControlDescriptor hide;hide.id=ov::ControlId("host.hide");hide.action=ov::ActionId("host.hide");hide.kind=ov::ControlKind::Action;hide.label=ov::LocalizedText(tr("隐藏状态层（F10 / 手柄 Mode 重开）","Hide status (F10 / gamepad Mode reopens)"));page.controls.push_back(hide);
        out.controls.pages.push_back(std::move(page));return out;
    }
};
Overlay::Overlay():impl(std::make_unique<Impl>()){}
Overlay::~Overlay()=default;
Overlay& shared(){static Overlay instance;return instance;}
void Overlay::initialize(const Config& config,unsigned width,unsigned height,bool sdf_capable) {
    auto& s=*impl;s.config=config;ov::OverlayInitInfo init;init.requested_font_mode=si::FontRenderMode::Sdf;init.renderer_capabilities.renderer_managed_textures=true;init.renderer_capabilities.partial_texture_updates=true;init.renderer_capabilities.sdf_font=sdf_capable;s.shell.initialize(init);
    s.environment.output.extent={float(width),float(height)};s.environment.views={s.environment.output};s.environment.scale.pixels_per_dp=config.density;
    s.shell.setPresentation(s.environment);
    auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags|=ImGuiBackendFlags_HasGamepad;
    if(sdf_capable){io.Fonts->TexGlyphPadding=6;io.Fonts->Flags|=ImFontAtlasFlags_NoBakedLines;ImGui::GetStyle().AntiAliasedLinesUseTex=false;}
    auto fonts=s.shell.fonts();ImFont* font=nullptr;
    if(!config.font_path.empty())font=io.Fonts->AddFontFromFileTTF(config.font_path.c_str(),16.f*config.density);
    if(!font)font=io.Fonts->AddFontDefault();
    for(auto& role:fonts.roles){role.handle=font;role.resolved_weight=ov::TextWeight::Regular;}
    fonts.cjk_resolved=!config.font_path.empty();fonts.cjk_font_source=config.font_path;s.shell.setResolvedFonts(fonts);
    ov::applyOverlayTheme(s.theme,config.density);s.load();
    s.frame_graph.id=ov::StableId("host.frame");s.frame_graph.label=ov::LocalizedText(config.chinese?"帧间隔":"Frame interval");s.frame_graph.unit=ov::LocalizedText("ms");s.frame_graph.reference_value=33.333f;s.frame_graph.reserveCapacity(120);
    s.sampler.start();s.ready=true;std::fprintf(stderr,"[overlay] Foundation shell initialized %ux%u density=%.2f font=%s\n",width,height,config.density,config.font_path.c_str());
}
void Overlay::sdfSubmitted(bool ready) {
    impl->shell.setSdfPipelineEvidence({ready,ready,ready});
    static bool reported=false;if(ready&&!reported){reported=true;std::fprintf(stderr,"[overlay] %s atlas=generated texture=bound shader=foundation-fwidth partial_updates=1\n",si::describeFontMode(impl->shell.fontMode()).c_str());}
}
void Overlay::presented(const FrameSample& sample) {
    if(!impl->ready)return;impl->sample=sample;impl->fps.onFrame(Clock::now());if(sample.frame>1)impl->frame_graph.push(float(sample.frame_ms));
}
void Overlay::draw() {
    auto& s=*impl;if(!s.ready)return;auto& io=ImGui::GetIO();
    const auto now=Clock::now();io.DeltaTime=s.last_draw.time_since_epoch().count()?std::max(.001f,std::chrono::duration<float>(now-s.last_draw).count()):1.f/60;s.last_draw=now;
    io.DisplaySize={s.environment.output.extent.x,s.environment.output.extent.y};
    auto& controller=s.shell.controller();
    if(s.cancel.exchange(false)){s.shell.notifyPresentationInvalidated(si::PointerCancelReason::SessionLost);io.ClearInputKeys();io.AddMouseButtonEvent(0,false);}
    if(s.toggle.exchange(false)){ov::OverlayIntent intent;intent.kind=controller.controlPanelOpen()?ov::OverlayIntentKind::CloseControlPanel:ov::OverlayIntentKind::OpenControlPanel;intent.page_id=ov::StableId(ov::perf_hud::kControlPage);controller.submitIntent(intent);}
    if(s.back.exchange(false)){ov::OverlayIntent intent;intent.kind=ov::OverlayIntentKind::Back;controller.submitIntent(intent);}
    controller.processIntents();
    if(s.sampler.sequence()!=s.seen){s.seen=s.sampler.sequence();if(auto device=s.sampler.latest()){
        s.cpu_source=device->cpu.usage_source==perf::CpuUsageSource::System?"system /proc/stat":device->cpu.usage_source==perf::CpuUsageSource::AppProcesses?"app processes / online CPUs":"unavailable (frequency only)";
        if(device->cpu.usage_source==perf::CpuUsageSource::Frequency)device->cpu.usage_percent.reset();
        s.hud.update(*device,s.fps.fps());
    }}else s.hud.setFps(s.fps.fps());
    auto snapshot=s.snapshot();
    // Measure the exact localized strings drawn this frame.
    for(auto& item:snapshot.status.summary_items)item.label=ov::LocalizedText(s.translate(item.label));
    ov::OverlayMetrics metrics;ov::measureOverlayStatus(snapshot,s.shell.fonts(),s.theme,metrics,s.environment);s.shell.setMetrics(metrics);
    auto frame=s.shell.render(io.DeltaTime);
#ifndef NDEBUG
    if(s.sequence.load()<=32)for(const auto& p:frame.input.routed)if(p.pressed||p.released||p.cancelled)std::fprintf(stderr,"[overlay-input] route %.1f %.1f pressed=%d released=%d cancel=%d layer=%llu\n",p.position.x,p.position.y,p.pressed,p.released,p.cancelled,(unsigned long long)p.layer);
#endif
    for(const auto& p:frame.input.routed){io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);io.AddMousePosEvent(p.position.x,p.position.y);if(p.pressed)io.AddMouseButtonEvent(0,true);if(p.released||p.cancelled)io.AddMouseButtonEvent(0,false);}
    for(const auto& key:frame.keyboard.keys)io.AddKeyEvent(ImGuiKey(key.keyIndex),key.isKeyDown);
    ImGui::NewFrame();
    ov::OverlayShellDrawInfo info;info.frame=&frame;info.snapshot=&snapshot;info.theme=&s.theme;info.intents=&s;info.translate=[&](const auto& t){return s.translate(t);};info.status_appearance=ov::perfHudAppearance(s.settings);
    ov::drawOverlayShellFrame(info,s.text_session,s.ime);ImGui::Render();
    bool save=false;ov::OverlayCommand command;
    while(s.shell.pollCommand(command)){
        const auto applied=ov::applyPerfHudControl(command,s.settings);save|=applied.settings_changed;
        for(const auto& intent:applied.intents)controller.submitIntent(intent);
        if(command.kind==ov::OverlayCommandKind::PersistentStateChanged)save=true;
        if(command.kind==ov::OverlayCommandKind::ControlChanged){if(const auto* value=std::get_if<bool>(&command.value)){
            const auto& id=command.control.value();if(id=="host.timings")s.timings=*value;else if(id=="host.frame_plot")s.frame_plot=*value;else if(id=="host.compilation")s.compilation=*value;else continue;save=true;
        }}
        if(command.kind==ov::OverlayCommandKind::ActionInvoked&&command.action.value()=="host.hide") {ov::OverlayIntent intent;intent.kind=ov::OverlayIntentKind::HideAllOverlays;controller.submitIntent(intent);save=true;}
    }
    controller.processIntents();s.capture=controller.topModal()!=nullptr;
    if(save)s.save();
}
void Overlay::pointer(int phase,float x,float y) {
    auto& s=*impl;if(!s.ready||phase<0||phase>4)return;
    si::PointerEvent event;event.sequence=++s.sequence;event.source=si::InputSource::Touch;event.phase=si::PointerPhase(phase);event.position={x*s.environment.output.extent.x,y*s.environment.output.extent.y};event.buttons=(phase==1||phase==2)?1:0;
#ifndef NDEBUG
    if(event.sequence<=32)std::fprintf(stderr,"[overlay-input] raw phase=%d %.1f %.1f seq=%llu\n",phase,event.position.x,event.position.y,(unsigned long long)event.sequence);
#endif
    s.shell.submitPointerEvent(event);
}
void Overlay::toggleControls(){impl->toggle=true;impl->capture=true;}
bool Overlay::capturesInput() const{return impl->capture.load();}
void Overlay::cancelInput(){impl->cancel=true;}
bool Overlay::key(int code,bool down) {
    if(code==140||code==110){if(down)toggleControls();return true;}
    if(!capturesInput())return false;
    if(down&&(code==97||code==4||code==111)){impl->back=true;return true;}
    ImGuiKey key=ImGuiKey_None;
    switch(code){case 19:key=ImGuiKey_UpArrow;break;case 20:key=ImGuiKey_DownArrow;break;case 21:key=ImGuiKey_LeftArrow;break;case 22:key=ImGuiKey_RightArrow;break;case 96:case 66:key=ImGuiKey_Enter;break;case 97:case 4:case 111:key=ImGuiKey_Escape;break;case 61:key=ImGuiKey_Tab;break;default:break;}
    if(key!=ImGuiKey_None)impl->shell.submitKeyEvent({int(key),down});return true;
}
}
