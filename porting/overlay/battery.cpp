// SPDX-License-Identifier: MIT
#include "overlay.h"
#include <spatial/perf/PerfMetrics.hpp>
#include <mutex>
#include <limits>
namespace {
std::mutex mutex;
spatial::perf::BatteryReading reading;
class HostBattery final : public spatial::perf::BatterySource {
public:
    spatial::perf::BatteryReading read() const override {std::lock_guard lock(mutex);return reading;}
};
}
#if defined(__ANDROID__)
namespace spatial::perf {
std::shared_ptr<const BatterySource> platformBatterySource() {return std::make_shared<HostBattery>();}
}
#endif
void mng::overlay::battery(int level,int64_t current,int64_t charge,int voltage,int temperature,bool charging) {
    std::lock_guard lock(mutex);reading={};
    if(level>=0&&level<=100)reading.level_percent=float(level);
    if(current!=std::numeric_limits<int64_t>::min()&&current)reading.current_microamps=current;
    if(charge>0)reading.charge_microamp_hours=charge;
    if(voltage>0)reading.voltage_microvolts=int64_t(voltage)*1000;
    if(temperature>-400&&temperature<1200)reading.temperature_celsius=temperature/10.f;
    reading.charging=charging;
}
