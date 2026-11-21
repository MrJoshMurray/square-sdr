/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "audio_input.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if !USE_DESKTOP && SQUARE_SDR_HAS_ALSA
#include <alsa/asoundlib.h>
#endif

namespace platform::audio {
namespace {
constexpr size_t kMaxBufferedSamples = 9600;
}

std::string find_preferred_capture_device() {
#if !USE_DESKTOP && SQUARE_SDR_HAS_ALSA
    std::string onboard = "default";
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char ctl_name[16];
        std::snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);
        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, ctl_name, 0) < 0) continue;
        snd_pcm_info_t* info = nullptr;
        snd_pcm_info_alloca(&info);
        snd_pcm_info_set_device(info, 0);
        snd_pcm_info_set_subdevice(info, 0);
        snd_pcm_info_set_stream(info, SND_PCM_STREAM_CAPTURE);
        const bool has_capture = snd_ctl_pcm_info(ctl, info) >= 0;
        char* id = nullptr;
        const bool has_id = snd_card_get_name(card, &id) >= 0 && id;
        const std::string name = has_id ? id : "";
        free(id);
        snd_ctl_close(ctl);
        if (!has_capture) continue;
        const bool is_onboard = name.find("ES8389") != std::string::npos
                             || name.find("cardputer") != std::string::npos;
        if (!is_onboard) {
            // PipeWire owns the USB card for playback; direct ALSA capture then stalls, so
            // capture through the PipeWire source node instead when one exists.
            if (FILE* pipe = popen("pactl list short sources 2>/dev/null", "r")) {
                char line[512];
                std::string node;
                while (node.empty() && std::fgets(line, sizeof(line), pipe)) {
                    const char* hit = std::strstr(line, "alsa_input.usb-");
                    if (!hit) continue;
                    const char* end = hit;
                    while (*end && *end != '\t' && *end != ' ' && *end != '\n') ++end;
                    node.assign(hit, end);
                }
                pclose(pipe);
                if (!node.empty()) return "pw:" + node;
            }
            return "plughw:" + std::to_string(card) + ",0";
        }
    }
    return onboard;
#else
    return "default";
#endif
}

AudioInput::~AudioInput() {
    close();
}

bool AudioInput::open(const std::string& device_id, uint32_t sample_rate, std::string& error) {
    close();
#if !USE_DESKTOP && SQUARE_SDR_HAS_ALSA
    const bool pipewire_node = device_id.rfind("pw:", 0) == 0;
    const char* name = device_id.empty() || pipewire_node ? "default" : device_id.c_str();
    if (pipewire_node) setenv("PIPEWIRE_NODE", device_id.c_str() + 3, 1);
    int last_error = 0;
    // Some USB headsets fail stereo capture with EIO; plug downmixes to mono reliably.
    const int order[2] = {1, 2};
    for (const int channels : order) {
        snd_pcm_t* pcm = nullptr;
        int result = snd_pcm_open(&pcm, name, SND_PCM_STREAM_CAPTURE, 0);
        if (result < 0) {
            error = snd_strerror(result);
            if (pipewire_node) unsetenv("PIPEWIRE_NODE");
            return false;
        }
        // 5 ms periods keep microphone latency low.
        result = snd_pcm_set_params(pcm,
                                    SND_PCM_FORMAT_S16_LE,
                                    SND_PCM_ACCESS_RW_INTERLEAVED,
                                    static_cast<unsigned>(channels),
                                    sample_rate,
                                    1,
                                    40000);
        if (result < 0) {
            last_error = result;
            snd_pcm_close(pcm);
            continue;
        }
        platform_device_ = pcm;
        channels_ = channels;
        device_id_ = device_id.empty() ? name : device_id;
        running_.store(true);
        thread_ = std::thread(&AudioInput::run, this);
        if (pipewire_node) {
            // The PipeWire ALSA plugin reads PIPEWIRE_NODE when the stream actually connects
            // (on the first read), so keep it set until samples arrive.
            for (int i = 0; i < 100 && available() == 0 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            unsetenv("PIPEWIRE_NODE");
        }
        return true;
    }
    if (pipewire_node) unsetenv("PIPEWIRE_NODE");
    error = snd_strerror(last_error);
    return false;
#else
    (void)device_id;
    (void)sample_rate;
    error = "Microphone capture is unavailable in this build";
    return false;
#endif
}

void AudioInput::run() {
#if !USE_DESKTOP && SQUARE_SDR_HAS_ALSA
    auto* pcm = static_cast<snd_pcm_t*>(platform_device_);
    constexpr size_t kChunkFrames = 240;
    std::vector<int16_t> raw(kChunkFrames * static_cast<size_t>(channels_));
    std::vector<int16_t> mono(kChunkFrames);
    while (running_.load()) {
        const snd_pcm_sframes_t frames = snd_pcm_readi(pcm, raw.data(), kChunkFrames);
        if (frames < 0) {
            if (snd_pcm_recover(pcm, static_cast<int>(frames), 1) < 0) {
                running_.store(false);
                break;
            }
            continue;
        }
        for (snd_pcm_sframes_t i = 0; i < frames; ++i) {
            if (channels_ == 2) {
                mono[static_cast<size_t>(i)] = static_cast<int16_t>(
                    (raw[static_cast<size_t>(i) * 2] + raw[static_cast<size_t>(i) * 2 + 1]) / 2);
            } else {
                mono[static_cast<size_t>(i)] = raw[static_cast<size_t>(i)];
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        received_.store(true);
        samples_.insert(samples_.end(), mono.begin(), mono.begin() + frames);
        while (samples_.size() > kMaxBufferedSamples) samples_.pop_front();
    }
#endif
}

void AudioInput::close() {
    running_.store(false);
#if !USE_DESKTOP && SQUARE_SDR_HAS_ALSA
    // Abort a read that is stalled waiting for samples so the join cannot hang.
    if (platform_device_) snd_pcm_drop(static_cast<snd_pcm_t*>(platform_device_));
#endif
    received_.store(false);
    if (thread_.joinable()) thread_.join();
#if !USE_DESKTOP && SQUARE_SDR_HAS_ALSA
    if (platform_device_) {
        auto* pcm = static_cast<snd_pcm_t*>(platform_device_);
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
#endif
    platform_device_ = nullptr;
    device_id_.clear();
    clear();
}

bool AudioInput::is_open() const {
    return platform_device_ != nullptr && running_.load();
}

size_t AudioInput::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

size_t AudioInput::read(int16_t* out, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t n = std::min(count, samples_.size());
    for (size_t i = 0; i < n; ++i) {
        out[i] = samples_.front();
        samples_.pop_front();
    }
    return n;
}

void AudioInput::trim(size_t keep) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (samples_.size() > keep) samples_.pop_front();
}

void AudioInput::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
}

} // namespace platform::audio
