/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/metis_protocol.h"
#include "sdr/metis_stream.h"
#include "sdr/receive_audio_pipeline.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

class LevelSink final : public platform::audio::MonoAudioSink {
public:
    bool write(const int16_t* samples, size_t frame_count, std::string&) override {
        for (size_t index = 0; index < frame_count; ++index) {
            peak = std::max(peak, std::abs(static_cast<int32_t>(samples[index])));
        }
        return true;
    }

    int32_t peak{0};
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: square-sdr_stream_probe RADIO_IPV4\n";
        return 2;
    }

    platform::sdr::MetisIqStream stream;
    std::string error;
    if (!stream.open(argv[1], error)
        || !stream.configure_receiver(14200000, error)
        || !stream.start(error)) {
        std::cerr << "Metis IQ stream start failed: " << error << '\n';
        return 1;
    }

    LevelSink sink;
    platform::sdr::ReceiveAudioPipeline pipeline(sink);
    platform::sdr::DemodulatorSettings settings;
    settings.mode = platform::sdr::DemodMode::Usb;
    settings.sample_rate_hz = platform::sdr::kDefaultRxSampleRateHz;
    settings.audio_bandwidth_hz = 2800;
    if (!pipeline.configure(settings, error)) {
        std::cerr << "Demodulator setup failed: " << error << '\n';
        stream.stop();
        return 1;
    }

    std::array<uint8_t, 2048> packet{};
    size_t valid_packets = 0;
    size_t invalid_packets = 0;
    int32_t raw_iq_peak = 0;
    uint64_t raw_iq_sum = 0;
    size_t raw_iq_count = 0;
    for (size_t attempt = 0; attempt < 30; ++attempt) {
        size_t received = 0;
        if (!stream.receive(packet.data(), packet.size(), 100, received, error)) {
            std::cerr << "Metis IQ receive failed: " << error << '\n';
            stream.stop();
            return 1;
        }
        if (received == 0) {
            continue;
        }
        platform::sdr::MetisIqPacket parsed;
        if (platform::sdr::parse_metis_ep6_packet(packet.data(), received, parsed)
            == platform::sdr::MetisPacketError::None) {
            ++valid_packets;
            for (size_t index = 0; index < parsed.sample_count; ++index) {
                const int32_t i_level = std::abs(parsed.samples[index].i);
                const int32_t q_level = std::abs(parsed.samples[index].q);
                raw_iq_peak = std::max(raw_iq_peak, std::max(i_level, q_level));
                raw_iq_sum += static_cast<uint64_t>(i_level) + static_cast<uint64_t>(q_level);
                raw_iq_count += 2;
            }
            if (!pipeline.process_datagram(packet.data(), received, error)) {
                std::cerr << "Demodulation failed: " << error << '\n';
                stream.stop();
                return 1;
            }
        } else {
            ++invalid_packets;
        }
    }
    stream.stop();
    std::cout << "Received valid EP6 packets: " << valid_packets
              << ", invalid packets: " << invalid_packets
              << ", raw I/Q peak: " << raw_iq_peak
              << ", raw I/Q mean: "
              << (raw_iq_count == 0 ? 0 : raw_iq_sum / raw_iq_count)
              << ", demodulated PCM peak: " << sink.peak << '\n';
    return valid_packets > 0 ? 0 : 3;
}