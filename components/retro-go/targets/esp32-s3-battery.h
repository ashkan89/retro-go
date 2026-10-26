#pragma once

// One protected 1S Li-ion/LiPo cell, 4.2 V maximum. See BATTERY.md.
// Targets select a free ADC1 channel; a 100k/100k divider halves cell voltage.
#define RG_BATTERY_DRIVER              1
#define RG_BATTERY_ADC_UNIT            ADC_UNIT_1
#define RG_BATTERY_DIVIDER_RATIO       2.f
#define RG_BATTERY_CALC_VOLTAGE(mv)    ((mv) * RG_BATTERY_DIVIDER_RATIO * 0.001f)
// Voltage-only estimate, not a fuel gauge. The driver clamps this to 0..100%.
#define RG_BATTERY_CALC_PERCENT(mv)    (((mv) * RG_BATTERY_DIVIDER_RATIO - 3300.f) * (100.f / 900.f))
// A fitted divider pulls the ADC low when the cell/sense switch is disconnected.
#define RG_BATTERY_IS_PRESENT(mv)      ((mv) >= 1000)
