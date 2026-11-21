/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "auto_notch.h"

namespace platform::sdr {
namespace {

constexpr float kStep = 0.01f;
constexpr float kLeak = 0.99995f;
constexpr float kEpsilon = 1e-9f;

} // namespace

AutoNotch::AutoNotch() { reset(); }

void AutoNotch::reset() {
    weights_.fill(0.0f);
    history_.fill(0.0f);
    pos_ = 0;
}

void AutoNotch::process(float* samples, size_t count) {
    for (size_t n = 0; n < count; ++n) {
        // history_ is a doubled ring buffer, so the reference span is contiguous.
        const float* ref = &history_[pos_ + kDelay];
        float prediction = 0.0f;
        float energy = 0.0f;
        for (size_t k = 0; k < kTaps; ++k) {
            prediction += weights_[k] * ref[k];
            energy += ref[k] * ref[k];
        }
        const float input = samples[n];
        const float error = input - prediction;

        const float gain = kStep * error / (energy + kEpsilon);
        for (size_t k = 0; k < kTaps; ++k) weights_[k] = kLeak * weights_[k] + gain * ref[k];
        samples[n] = error;

        pos_ = pos_ == 0 ? kHistory - 1 : pos_ - 1;
        history_[pos_] = input;
        history_[pos_ + kHistory] = input;
    }
}

} // namespace platform::sdr
