#include "rg_nestopia_profile.hpp"
#ifdef RG_NESTOPIA_PROFILE
namespace RetroGo {
uint64_t ppuCycles, apuWalkCycles, apuSampleCycles;
uint32_t tileBatches;
}
#endif
