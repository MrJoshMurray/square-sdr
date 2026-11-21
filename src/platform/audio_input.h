/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace platform::audio {

// Continuously captures mono microphone audio into a short ring buffer.
// Picks the microphone to use: a USB headset or mic when one is plugged in, otherwise the
// on-board codec (built-in mic and 3.5mm headset). Returns an ALSA device name.
std::string find_preferred_capture_device();

class AudioInput final {
public:
    AudioInput() = default;
    ~AudioInput();

    AudioInput(const AudioInput&) = delete;
    AudioInput& operator=(const AudioInput&) = delete;

    bool open(const std::string& device_id, uint32_t sample_rate, std::string& error);
    void close();
    bool is_open() const;
    bool has_received() const { return received_.load(); }
    const std::string& device_id() const { return device_id_; }

    size_t available() const;
    // Removes up to `count` samples, returning how many were copied.
    size_t read(int16_t* out, size_t count);
    // Discards the oldest samples so at most `keep` remain.
    void trim(size_t keep);
    void clear();

private:
    void run();

    void* platform_device_{nullptr};
    int channels_{1};
    std::string device_id_;
    std::atomic<bool> running_{false};
    std::atomic<bool> received_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::deque<int16_t> samples_;
};

} // namespace platform::audio
