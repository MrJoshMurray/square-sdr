/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "audio_output.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <utility>

#if USE_DESKTOP
#include <SDL.h>
#elif SQUARE_SDR_HAS_ALSA
#include <alsa/asoundlib.h>
#endif

namespace platform::audio {
namespace {

constexpr size_t kMaxFramesPerWrite = 4096;
constexpr size_t kMaxPendingVolumeActions = 32;

bool apply_system_volume_action(VolumeAction action, VolumeState& state, std::string& error) {
#if !USE_DESKTOP && SQUARE_SDR_HAS_ALSA
    snd_mixer_t* mixer = nullptr;
    int result = snd_mixer_open(&mixer, 0);
    if (result < 0) {
        error = snd_strerror(result);
        return false;
    }

    result = snd_mixer_attach(mixer, "pulse");
    if (result >= 0) result = snd_mixer_selem_register(mixer, nullptr, nullptr);
    if (result >= 0) result = snd_mixer_load(mixer);

    snd_mixer_elem_t* element = nullptr;
    if (result >= 0) {
        snd_mixer_selem_id_t* id = nullptr;
        snd_mixer_selem_id_alloca(&id);
        snd_mixer_selem_id_set_index(id, 0);
        snd_mixer_selem_id_set_name(id, "Master");
        element = snd_mixer_find_selem(mixer, id);
        if (!element) {
            error = "System audio mixer is unavailable";
            snd_mixer_close(mixer);
            return false;
        }
    }

    if (result >= 0 && (action == VolumeAction::Mute || action == VolumeAction::Unmute)) {
        result = snd_mixer_selem_set_playback_switch_all(element, action == VolumeAction::Unmute);
        state.silent = true;
    } else if (result >= 0 && action == VolumeAction::ToggleMute) {
        int enabled = 0;
        result = snd_mixer_selem_get_playback_switch(element, SND_MIXER_SCHN_FRONT_LEFT, &enabled);
        if (result >= 0) result = snd_mixer_selem_set_playback_switch_all(element, !enabled);
    } else if (result >= 0) {
        long minimum = 0;
        long maximum = 0;
        long volume = 0;
        result = snd_mixer_selem_get_playback_volume_range(element, &minimum, &maximum);
        if (result >= 0) {
            result = snd_mixer_selem_get_playback_volume(element, SND_MIXER_SCHN_FRONT_LEFT, &volume);
        }
        if (result >= 0) {
            const long step = std::max(1L, (maximum - minimum + 10) / 20);
            const long adjustment = action == VolumeAction::Up ? step : -step;
            result = snd_mixer_selem_set_playback_volume_all(
                element, std::clamp(volume + adjustment, minimum, maximum));
        }
    }

    if (result >= 0) {
        long minimum = 0;
        long maximum = 0;
        long volume = 0;
        int enabled = 0;
        result = snd_mixer_selem_get_playback_volume_range(element, &minimum, &maximum);
        if (result >= 0) result = snd_mixer_selem_get_playback_volume(element, SND_MIXER_SCHN_FRONT_LEFT, &volume);
        if (result >= 0) result = snd_mixer_selem_get_playback_switch(element, SND_MIXER_SCHN_FRONT_LEFT, &enabled);
        if (result >= 0 && maximum > minimum) {
            state.percent = static_cast<int>(std::clamp(
                ((volume - minimum) * 100 + (maximum - minimum) / 2) / (maximum - minimum), 0L, 100L));
            state.muted = !enabled;
        } else if (result >= 0) {
            error = "Invalid system volume range";
            snd_mixer_close(mixer);
            return false;
        }
    }
    if (result < 0) error = snd_strerror(result);
    snd_mixer_close(mixer);
    return result >= 0;
#else
    (void)action;
    (void)state;
    error = "System audio controls are unavailable";
    return false;
#endif
}

#if USE_DESKTOP
bool ensure_audio_subsystem(std::string& error) {
    if (SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        return true;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        return true;
    }
    error = SDL_GetError();
    return false;
}
#endif

} // namespace

SystemVolumeControl::SystemVolumeControl()
#if USE_DESKTOP
    : SystemVolumeControl([state = VolumeState{50, false}](VolumeAction action,
                                                          VolumeState& result,
                                                          std::string&) mutable {
        result.silent = false;
        if (action == VolumeAction::Mute || action == VolumeAction::Unmute) {
            state.muted = action == VolumeAction::Mute;
            result = state;
            result.silent = true;
            return true;
        }
        if (action == VolumeAction::ToggleMute) state.muted = !state.muted;
        else state.percent = std::clamp(state.percent + (action == VolumeAction::Up ? 5 : -5), 0, 100);
        result = state;
        return true;
    }) {}
#else
    : SystemVolumeControl(apply_system_volume_action) {}
#endif

SystemVolumeControl::SystemVolumeControl(Handler handler)
    : handler_(std::move(handler)), worker_(&SystemVolumeControl::run, this) {}

SystemVolumeControl::~SystemVolumeControl() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        pending_.clear();
    }
    ready_.notify_one();
    worker_.join();
}

bool SystemVolumeControl::request(VolumeAction action) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || pending_.size() >= kMaxPendingVolumeActions) return false;
        pending_.push_back(action);
    }
    ready_.notify_one();
    return true;
}

bool SystemVolumeControl::take_state(VolumeState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_available_) return false;
    state = state_;
    state_available_ = false;
    return true;
}

bool SystemVolumeControl::take_error(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_.empty()) return false;
    error = std::move(error_);
    error_.clear();
    return true;
}

void SystemVolumeControl::run() {
    for (;;) {
        VolumeAction action;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;
            action = pending_.front();
            pending_.pop_front();
        }
        std::string error;
        VolumeState state;
        const bool succeeded = handler_(action, state, error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (succeeded) {
                state_ = state;
                state_available_ = true;
            } else {
                error_ = error.empty() ? "System audio control failed" : std::move(error);
            }
        }
    }
}

bool enumerate_output_devices(std::vector<OutputDevice>& devices, std::string& error) {
    devices.clear();
    error.clear();

#if USE_DESKTOP
    if (!ensure_audio_subsystem(error)) {
        return false;
    }
    devices.push_back({"", "System default", true});
    const int count = SDL_GetNumAudioDevices(0);
    if (count < 0) {
        error = SDL_GetError();
        devices.clear();
        return false;
    }
    for (int index = 0; index < count; ++index) {
        if (const char* name = SDL_GetAudioDeviceName(index, 0)) {
            devices.push_back({name, name, false});
        }
    }
    return true;
#elif SQUARE_SDR_HAS_ALSA
    devices.push_back({"default", "System default (speaker/headphones)", true});

    void** hints = nullptr;
    const int hint_result = snd_device_name_hint(-1, "pcm", &hints);
    if (hint_result < 0) {
        error = snd_strerror(hint_result);
        devices.clear();
        return false;
    }

    std::set<std::string> seen{"default"};
    for (void** hint = hints; *hint != nullptr; ++hint) {
        char* name = snd_device_name_get_hint(*hint, "NAME");
        char* description = snd_device_name_get_hint(*hint, "DESC");
        if (name && *name && seen.insert(name).second && std::string(name) != "null") {
            std::string label = description && *description ? description : name;
            std::replace(label.begin(), label.end(), '\n', ' ');
            devices.push_back({name, std::move(label), false});
        }
        std::free(name);
        std::free(description);
    }
    snd_device_name_free_hint(hints);
    return true;
#else
    error = "Audio output is unavailable in this build";
    return false;
#endif
}

AudioOutput::~AudioOutput() {
    close();
}

void BandEdgeTone::request() {
    requested_.store(true);
}

int16_t BandEdgeTone::next_sample(int16_t input, uint32_t sample_rate) {
    if (requested_.exchange(false)) {
        position_ = 0;
        length_ = sample_rate * 80 / 1000;
    }
    if (position_ >= length_ || sample_rate == 0) return input;
    constexpr float kPi = 3.14159265358979323846f;
    const uint32_t ramp = std::max<uint32_t>(1, sample_rate / 200);
    const float envelope = std::min({1.0f, static_cast<float>(position_) / ramp,
                                     static_cast<float>(length_ - position_) / ramp});
    const float phase = 2.0f * kPi * 880.0f * static_cast<float>(position_) / sample_rate;
    ++position_;
    return static_cast<int16_t>(std::lrint(std::sin(phase) * envelope * 655.0f));
}

void BandEdgeTone::reset() {
    requested_.store(false);
    position_ = 0;
    length_ = 0;
}

void AudioOutput::request_band_edge_tone() {
    if (accepting_tones_.load()) band_edge_tone_.request();
}

bool AudioOutput::open(const std::string& device_id, uint32_t sample_rate, std::string& error) {
    close();
    error.clear();
    if (sample_rate == 0) {
        error = "Sample rate must be greater than zero";
        return false;
    }

#if USE_DESKTOP
    if (!ensure_audio_subsystem(error)) {
        return false;
    }

    SDL_AudioSpec requested{};
    requested.freq = static_cast<int>(sample_rate);
    requested.format = AUDIO_S16SYS;
    requested.channels = 2;
    requested.samples = 1024;

    SDL_AudioSpec obtained{};
    const char* device_name = device_id.empty() ? nullptr : device_id.c_str();
    const auto device = SDL_OpenAudioDevice(device_name, 0, &requested, &obtained, 0);
    if (device == 0) {
        error = SDL_GetError();
        return false;
    }
    if (obtained.freq != requested.freq || obtained.format != requested.format || obtained.channels != 2) {
        SDL_CloseAudioDevice(device);
        error = "Audio device does not support 48 kHz stereo PCM";
        return false;
    }

    desktop_device_id_ = device;
    sample_rate_ = sample_rate;
    stereo_buffer_.reserve(kMaxFramesPerWrite * 2);
    SDL_PauseAudioDevice(device, 0);
    accepting_tones_.store(true);
    return true;
#elif SQUARE_SDR_HAS_ALSA
    snd_pcm_t* pcm = nullptr;
    const char* name = device_id.empty() ? "default" : device_id.c_str();
    int result = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, 0);
    if (result < 0) {
        error = snd_strerror(result);
        return false;
    }

    result = snd_pcm_set_params(pcm,
                                SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                2,
                                sample_rate,
                                1,
                                100000);
    if (result < 0) {
        error = snd_strerror(result);
        snd_pcm_close(pcm);
        return false;
    }

    platform_device_ = pcm;
    sample_rate_ = sample_rate;
    stereo_buffer_.reserve(kMaxFramesPerWrite * 2);
    accepting_tones_.store(true);
    return true;
#else
    (void)device_id;
    error = "Audio output is unavailable in this build";
    return false;
#endif
}

bool AudioOutput::write(const int16_t* mono_samples, size_t frame_count, std::string& error) {
    error.clear();
    if (!is_open()) {
        error = "Audio output is not open";
        return false;
    }
    if (!mono_samples && frame_count != 0) {
        error = "Audio buffer is null";
        return false;
    }
    if (frame_count == 0) {
        return true;
    }
    if (frame_count > kMaxFramesPerWrite) {
        error = "Audio write exceeds the maximum chunk size";
        return false;
    }

    stereo_buffer_.resize(frame_count * 2);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        const int16_t sample = band_edge_tone_.next_sample(mono_samples[frame], sample_rate_);
        stereo_buffer_[frame * 2] = sample;
        stereo_buffer_[frame * 2 + 1] = sample;
    }

#if USE_DESKTOP
    const size_t byte_count = stereo_buffer_.size() * sizeof(int16_t);
    const size_t max_queued_bytes = static_cast<size_t>(sample_rate_) * 2 * sizeof(int16_t) / 4;
    if (byte_count > max_queued_bytes) {
        error = "Audio output queue is full";
        return false;
    }
    for (size_t attempt = 0; attempt < 100; ++attempt) {
        const size_t queued_bytes = SDL_GetQueuedAudioSize(desktop_device_id_);
        if (queued_bytes <= max_queued_bytes - byte_count) {
            break;
        }
        SDL_Delay(1);
        if (attempt == 99) {
            error = "Audio output queue is full";
            return false;
        }
    }
    if (SDL_QueueAudio(desktop_device_id_, stereo_buffer_.data(), static_cast<uint32_t>(byte_count)) != 0) {
        error = SDL_GetError();
        return false;
    }
    return true;
#elif SQUARE_SDR_HAS_ALSA
    auto* pcm = static_cast<snd_pcm_t*>(platform_device_);
    size_t written = 0;
    while (written < frame_count) {
        const snd_pcm_sframes_t result = snd_pcm_writei(pcm,
                                stereo_buffer_.data() + written * 2,
                                                        frame_count - written);
        if (result < 0) {
            const int recovered = snd_pcm_recover(pcm, static_cast<int>(result), 1);
            if (recovered < 0) {
                error = snd_strerror(recovered);
                return false;
            }
            continue;
        }
        written += static_cast<size_t>(result);
    }
    return true;
#else
    error = "Audio output is unavailable in this build";
    return false;
#endif
}

void AudioOutput::flush() {
#if USE_DESKTOP
    if (desktop_device_id_ != 0) SDL_ClearQueuedAudio(desktop_device_id_);
#elif SQUARE_SDR_HAS_ALSA
    if (platform_device_) {
        auto* pcm = static_cast<snd_pcm_t*>(platform_device_);
        snd_pcm_drop(pcm);
        snd_pcm_prepare(pcm);
    }
#endif
}

size_t AudioOutput::queued_frames() const {
#if USE_DESKTOP
    if (desktop_device_id_ != 0) return SDL_GetQueuedAudioSize(desktop_device_id_) / (2 * sizeof(int16_t));
#elif SQUARE_SDR_HAS_ALSA
    if (platform_device_) {
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(static_cast<snd_pcm_t*>(platform_device_), &delay) == 0 && delay > 0) {
            return static_cast<size_t>(delay);
        }
    }
#endif
    return 0;
}

void AudioOutput::start_now() {
#if SQUARE_SDR_HAS_ALSA && !USE_DESKTOP
    if (platform_device_) {
        auto* pcm = static_cast<snd_pcm_t*>(platform_device_);
        if (snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED) snd_pcm_start(pcm);
    }
#endif
}

void AudioOutput::close() {
    accepting_tones_.store(false);
    band_edge_tone_.reset();
#if USE_DESKTOP
    if (desktop_device_id_ != 0) {
        SDL_ClearQueuedAudio(desktop_device_id_);
        SDL_CloseAudioDevice(desktop_device_id_);
        desktop_device_id_ = 0;
    }
#elif SQUARE_SDR_HAS_ALSA
    if (platform_device_) {
        auto* pcm = static_cast<snd_pcm_t*>(platform_device_);
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
        platform_device_ = nullptr;
    }
#endif
    sample_rate_ = 0;
}

bool AudioOutput::is_open() const {
#if USE_DESKTOP
    return desktop_device_id_ != 0;
#elif SQUARE_SDR_HAS_ALSA
    return platform_device_ != nullptr;
#else
    return false;
#endif
}

} // namespace platform::audio