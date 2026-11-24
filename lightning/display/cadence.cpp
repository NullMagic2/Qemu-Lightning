/*
 * Module: lightning.display.cadence
 * Purpose: Advance a shared absolute cadence without accumulating timer drift.
 * Created: 2026-10-22
 * Architecture: Extracted unchanged from the existing CPU publication owner.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "cadence.hpp"
#include "hw/misc/lightning-display-host.h"
#include <algorithm>
namespace {
using lightning::display::default_period_ns;
using lightning::display::maximum_clock_ns;
constexpr uint32_t minimum_refresh_millihz = 1000;
constexpr uint32_t maximum_refresh_millihz = 360000;
constexpr uint64_t nanoseconds_times_millihertz =
    uint64_t{LIGHTNING_DISPLAY_NANOSECONDS_PER_SECOND} * 1000;
}
extern "C" uint32_t lightning_display_refresh_period(uint32_t millihz) {
    return millihz >= minimum_refresh_millihz && millihz <= maximum_refresh_millihz ?
        static_cast<uint32_t>((nanoseconds_times_millihertz + millihz - 1) / millihz) : default_period_ns;
}

extern "C" uint64_t lightning_display_advance_deadline(uint64_t previous, uint64_t now, uint32_t period) {
    const uint64_t origin = std::min(previous, maximum_clock_ns);
    const uint64_t bounded_now = std::min(now, maximum_clock_ns);
    if (period == 0) { period = default_period_ns; }
    const uint64_t missed = bounded_now >= origin ? (bounded_now - origin) / period : 0;
    const uint64_t available = maximum_clock_ns - origin;
    return missed < available / period ? origin + (missed + 1) * period : maximum_clock_ns;
}

