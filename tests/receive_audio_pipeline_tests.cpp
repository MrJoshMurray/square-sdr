/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/receive_audio_pipeline.h"
#include "sdr/radio_settings_adapter.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <liquid/liquid.h>

namespace {

constexpr size_t kPacketSize = platform::sdr::kMetisEp6PacketSize;
constexpr size_t kBlockSize = 512;
constexpr size_t kBlockHeaderSize = 8;
constexpr size_t kSampleStride = 8;
constexpr size_t kPacketsToGenerate = 300;
constexpr uint32_t kSampleRateHz = 48000;
constexpr float kPi = 3.14159265358979323846f;

class CapturingSink final : public platform::audio::MonoAudioSink {
public:
    bool write(const int16_t* samples, size_t frame_count, std::string& error) override {
        if (!samples && frame_count != 0) {
            error = "null samples";
            return false;
        }
        pcm.insert(pcm.end(), samples, samples + frame_count);
        return true;
    }

    std::vector<int16_t> pcm;
};

void encode_signed_24(uint8_t* destination, int32_t value) {
    const auto encoded = static_cast<uint32_t>(value);
    destination[0] = static_cast<uint8_t>(encoded >> 16);
    destination[1] = static_cast<uint8_t>(encoded >> 8);
    destination[2] = static_cast<uint8_t>(encoded);
}

void write_iq_sample(uint8_t* destination, liquid_float_complex signal) {
    const auto to_24 = [](float sample) {
        const float clipped = std::clamp(sample, -1.0f, 0.99999988f);
        return static_cast<int32_t>(std::lrint(clipped * 8388608.0f));
    };
    encode_signed_24(destination, to_24(signal.real));
    encode_signed_24(destination + 3, to_24(signal.imag));
    destination[6] = 0;
    destination[7] = 0;
}

void encode_packet_header(std::array<uint8_t, kPacketSize>& packet, uint32_t sequence) {
    packet[0] = 0xEF;
    packet[1] = 0xFE;
    packet[2] = 0x01;
    packet[3] = 0x06;
    packet[4] = static_cast<uint8_t>(sequence >> 24);
    packet[5] = static_cast<uint8_t>(sequence >> 16);
    packet[6] = static_cast<uint8_t>(sequence >> 8);
    packet[7] = static_cast<uint8_t>(sequence);
    for (size_t block = 0; block < 2; ++block) {
        const size_t offset = 8 + block * kBlockSize;
        packet[offset] = 0x7F;
        packet[offset + 1] = 0x7F;
        packet[offset + 2] = 0x7F;
    }
}

} // namespace

int main() {
    model::BaseModel model;

    CapturingSink sink;
    platform::sdr::ReceiveAudioPipeline pipeline(sink);
    std::string error;
    assert(pipeline.configure(model, error));
    assert(!pipeline.signal_level_dbfs().has_value());
    const auto selected_settings = platform::sdr::make_demodulator_settings(model);

    auto modulator = ampmodem_create(0.5f, LIQUID_AMPMODEM_LSB, 1);
    assert(modulator != nullptr);
    std::array<uint8_t, kPacketSize> packet{};
    uint32_t sample_index = 0;

    for (uint32_t packet_index = 0; packet_index < kPacketsToGenerate; ++packet_index) {
        std::fill(packet.begin(), packet.end(), 0);
        encode_packet_header(packet, packet_index);
        for (size_t block = 0; block < 2; ++block) {
            const size_t block_offset = 8 + block * kBlockSize;
            for (size_t sample = 0; sample < 63; ++sample) {
                const float message = 0.25f * std::sin(2.0f * kPi * 1000.0f
                                                     * sample_index / kSampleRateHz);
                liquid_float_complex signal{};
                assert(ampmodem_modulate(modulator, message, &signal) == LIQUID_OK);
                const size_t sample_offset = block_offset + kBlockHeaderSize + sample * kSampleStride;
                write_iq_sample(packet.data() + sample_offset, signal);
                ++sample_index;
            }
        }
        assert(pipeline.configure_if_changed(selected_settings, error));
        assert(pipeline.process_datagram(packet.data(), packet.size(), error));
    }

    assert(sink.pcm.size() == kPacketsToGenerate * platform::sdr::kMetisIqSamplesPerPacket);
    int32_t peak = 0;
    for (const int16_t sample : sink.pcm) {
        peak = std::max(peak, std::abs(static_cast<int32_t>(sample)));
    }
    assert(peak > 100);
    assert(peak <= 2622);
    assert(sink.pcm.front() == 0);
    const auto normal_level = pipeline.signal_level_dbfs();
    assert(normal_level.has_value());
    assert(*normal_level > -35.0f);
    model.cycle_radio_mode(1);
    model.cycle_filter_bandwidth(1);
    const auto changed_settings = platform::sdr::make_demodulator_settings(model.radio_mode(), model.filter_bandwidth_hz());
    assert(changed_settings.mode == platform::sdr::DemodMode::Lsb);
    assert(changed_settings.audio_bandwidth_hz == 3000);
    assert(pipeline.configure_if_changed(changed_settings, error));
    assert(!pipeline.signal_level_dbfs());
    const size_t before_change = sink.pcm.size();
    assert(pipeline.process_datagram(packet.data(), packet.size(), error));
    assert(sink.pcm[before_change] == 0);
    for (int mode = 0; mode < 3; ++mode) {
        model.cycle_radio_mode(1);
        const auto next_settings = platform::sdr::make_demodulator_settings(model);
        assert(pipeline.configure_if_changed(next_settings, error));
        const size_t start = sink.pcm.size();
        for (int index = 0; index < 210; ++index) {
            assert(pipeline.configure_if_changed(next_settings, error));
            assert(pipeline.process_datagram(packet.data(), packet.size(), error));
        }
        assert(sink.pcm[start] == 0);
        for (size_t index = start; index < sink.pcm.size(); ++index) {
            assert(std::abs(static_cast<int32_t>(sink.pcm[index])) <= 2622);
        }
    }

    assert(pipeline.configure_if_changed(selected_settings, error));
    pipeline.reset();
    assert(!pipeline.signal_level_dbfs().has_value());
    sink.pcm.clear();
    for (uint32_t packet_index = 0; packet_index < 600; ++packet_index) {
        std::fill(packet.begin(), packet.end(), 0);
        encode_packet_header(packet, packet_index);
        for (size_t block = 0; block < 2; ++block) {
            const size_t block_offset = 8 + block * kBlockSize;
            for (size_t sample = 0; sample < 63; ++sample) {
                const float amplitude = packet_index >= 250 && packet_index < 300 ? 0.5f : 0.000012f;
                const float message = amplitude * std::sin(2.0f * kPi * 1000.0f
                                                         * sample_index / kSampleRateHz);
                liquid_float_complex signal{};
                assert(ampmodem_modulate(modulator, message, &signal) == LIQUID_OK);
                const size_t sample_offset = block_offset + kBlockHeaderSize + sample * kSampleStride;
                write_iq_sample(packet.data() + sample_offset, signal);
                ++sample_index;
            }
        }
        assert(pipeline.process_datagram(packet.data(), packet.size(), error));
        if (packet_index == 240) {
            assert(pipeline.signal_level_dbfs().has_value());
            assert(*pipeline.signal_level_dbfs() < *normal_level - 40.0f);
        }
    }
    ampmodem_destroy(modulator);

    int32_t weak_peak = 0;
    int32_t transition_peak = 0;
    int32_t recovered_peak = 0;
    for (size_t index = 24000; index < sink.pcm.size(); ++index) {
        const auto magnitude = std::abs(static_cast<int32_t>(sink.pcm[index]));
        if (index < 250 * platform::sdr::kMetisIqSamplesPerPacket) {
            weak_peak = std::max(weak_peak, magnitude);
        }
        if (index >= 500 * platform::sdr::kMetisIqSamplesPerPacket) {
            recovered_peak = std::max(recovered_peak, magnitude);
        }
        transition_peak = std::max(transition_peak, magnitude);
    }
    assert(weak_peak > 10);
    assert(recovered_peak > 10);
    assert(transition_peak <= 2622);
    assert(sink.pcm.front() == 0);
}