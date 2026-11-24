/*
 * Module: lightning.display.cadence
 * Purpose: Share absolute monotonic cadence bounds between CPU and GPU publication.
 * Created: 2026-10-22
 * Architecture: Reused by presentation owners; no timer or native UI ownership.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once
#include "hw/misc/lightning-display.h"
extern "C" {
#include "hw/misc/lightning-memory.h"
}
#include <cstdint>
#include <limits>
namespace lightning::display {
constexpr uint32_t default_period_ns = LIGHTNING_DISPLAY_NANOSECONDS_PER_SECOND /
    LIGHTNING_SCANOUT_MODE_REFRESH_HZ;
constexpr uint64_t maximum_clock_ns = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
}
