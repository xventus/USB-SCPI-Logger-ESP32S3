//
// vim: ts=4 et
// Copyright (c) 2026 Petr Vanek, petr@fotoventus.cz
//
/// @file   measurement.h
/// @author Petr Vanek
///
/// The list of sampling intervals offered in the LCD menu. The measured
/// quantities themselves now come from the active Profile (profile.h); this
/// file only holds the interval choices, which are a UI concern.

#pragma once

#include <cstdint>
#include <cstddef>

namespace meas
{
    inline constexpr uint32_t INTERVALS_MS[] = {500, 1000, 2000, 5000, 10000};
    inline constexpr const char *INTERVAL_LABELS[] = {"0.5 s", "1 s", "2 s", "5 s", "10 s"};
    inline constexpr size_t INTERVAL_COUNT = sizeof(INTERVALS_MS) / sizeof(INTERVALS_MS[0]);

    /// Index of the interval used before a profile is loaded / as a fallback.
    inline constexpr size_t DEFAULT_INTERVAL_IDX = 1; ///< 1 s

    /// Nearest interval index for an arbitrary interval in ms (profile default).
    inline size_t intervalIndexFor(uint32_t ms)
    {
        for (size_t i = 0; i < INTERVAL_COUNT; ++i)
        {
            if (INTERVALS_MS[i] >= ms)
                return i;
        }
        return INTERVAL_COUNT - 1;
    }
}
