/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "metis_protocol.h"

namespace platform::sdr {

constexpr size_t kSpectrumBinCount = 96;

using SpectrumFrame = std::array<uint8_t, kSpectrumBinCount>;

SpectrumFrame make_simulated_spectrum(uint32_t frame_index) noexcept;

class LiveSpectrumBuffer {
public:
	static void push_iq(const IqSample* samples, size_t sample_count) noexcept;
	static bool snapshot(SpectrumFrame& frame) noexcept;
};

} // namespace platform::sdr