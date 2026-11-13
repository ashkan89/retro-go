#pragma once
#ifdef RG_NESTOPIA_PROFILE
#include <stdint.h>
#include <esp_cpu.h>
namespace RetroGo {
extern uint64_t ppuCycles, apuWalkCycles, apuSampleCycles;
extern uint32_t tileBatches;
struct ProfileScope {
    uint64_t &total;
    uint32_t start;
    explicit ProfileScope(uint64_t &value) : total(value), start(esp_cpu_get_cycle_count()) {}
    ~ProfileScope() { total += uint32_t(esp_cpu_get_cycle_count() - start); }
};
}
#define RG_NES_SCOPE(counter) RetroGo::ProfileScope scope(RetroGo::counter)
#else
#define RG_NES_SCOPE(counter)
#endif
