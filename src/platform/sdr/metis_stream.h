/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <netinet/in.h>


namespace platform::sdr {

std::array<uint8_t, 60> make_metis_iq_stream_command(bool enabled) noexcept;
std::array<uint8_t, 1032> make_metis_receiver_config_packet(uint32_t frequency_hz) noexcept;

struct MetisTransmitControl {
    bool mox{false};
    uint8_t drive{0};
    bool release{false};
    bool carrier{false};
    // Optional 126 interleaved I,Q pairs of 16-bit transmit samples for voice modes.
    const int16_t* iq{nullptr};
};

std::array<uint8_t, 1032> make_metis_transmit_config_packet(uint32_t frequency_hz,
                                                           const MetisTransmitControl& control) noexcept;

class MetisIqStream final {
public:
    MetisIqStream() = default;
    ~MetisIqStream();

    MetisIqStream(const MetisIqStream&) = delete;
    MetisIqStream& operator=(const MetisIqStream&) = delete;

    bool open(const std::string& radio_address, std::string& error);
    bool configure_receiver(uint32_t frequency_hz,
                            std::string& error,
                            const MetisTransmitControl& transmit = {});
    // Sends one EP2 frame; call once per received EP6 packet while keyed so the
    // radio's transmit path is paced by a continuous stream.
    bool send_transmit_frame(uint32_t frequency_hz,
                             const MetisTransmitControl& transmit,
                             uint32_t phase,
                             std::string& error);
    bool start(std::string& error);
    bool receive(uint8_t* data,
                 size_t capacity,
                 uint32_t timeout_ms,
                 size_t& received,
                 std::string& error);
    void stop();

private:
    int socket_fd_{-1};
    ::sockaddr_in* radio_address_{nullptr};
};

} // namespace platform::sdr