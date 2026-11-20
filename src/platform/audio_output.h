/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platform::audio {

struct OutputDevice {
    std::string id;
    std::string name;
    bool is_default{false};
};

bool enumerate_output_devices(std::vector<OutputDevice>& devices, std::string& error);

enum class VolumeAction {
    Down,
    Up,
    ToggleMute,
};

struct VolumeState {
    int percent{0};
    bool muted{false};
};

class SystemVolumeControl {
public:
    using Handler = std::function<bool(VolumeAction, VolumeState&, std::string&)>;

    SystemVolumeControl();
    explicit SystemVolumeControl(Handler handler);
    ~SystemVolumeControl();

    SystemVolumeControl(const SystemVolumeControl&) = delete;
    SystemVolumeControl& operator=(const SystemVolumeControl&) = delete;

    bool request(VolumeAction action);
    bool take_state(VolumeState& state);
    bool take_error(std::string& error);

private:
    void run();

    Handler handler_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<VolumeAction> pending_;
    VolumeState state_{};
    bool state_available_{false};
    std::string error_;
    bool stopping_{false};
    std::thread worker_;
};

class MonoAudioSink {
public:
    virtual ~MonoAudioSink() = default;
    virtual bool write(const int16_t* mono_samples, size_t frame_count, std::string& error) = 0;
};

class AudioOutput final : public MonoAudioSink {
public:
    AudioOutput() = default;
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool open(const std::string& device_id, uint32_t sample_rate, std::string& error);
    bool write(const int16_t* mono_samples, size_t frame_count, std::string& error) override;
    void close();
    bool is_open() const;

private:
    uint32_t sample_rate_{0};
    uint32_t desktop_device_id_{0};
    void* platform_device_{nullptr};
    std::vector<int16_t> stereo_buffer_;
};

} // namespace platform::audio