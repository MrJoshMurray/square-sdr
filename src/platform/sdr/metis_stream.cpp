/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "metis_stream.h"

#include <cerrno>
#include <cstring>
#include <memory>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace platform::sdr {
namespace {

constexpr uint16_t kMetisPort = 1024;

uint8_t receive_filter_outputs(uint32_t frequency_hz) noexcept {
    if (frequency_hz < 1600000) return 0;
    if (frequency_hz < 2500000) return 0x01;
    if (frequency_hz < 4500000) return 0x42;
    if (frequency_hz < 8500000) return 0x44;
    if (frequency_hz < 16500000) return 0x48;
    if (frequency_hz < 22500000) return 0x50;
    if (frequency_hz <= 30000000) return 0x60;
    return 0;
}

// Constant I with zero Q in each 8-byte sample group yields a carrier at the dial frequency.
void fill_tx_carrier(std::array<uint8_t, 1032>& packet) noexcept {
    constexpr int16_t kCarrierLevel = 0x5A00;
    for (size_t block = 0; block < 2; ++block) {
        const size_t samples = 8 + block * 512 + 8;
        for (size_t group = 0; group < 63; ++group) {
            const size_t offset = samples + group * 8;
            packet[offset + 4] = static_cast<uint8_t>(kCarrierLevel >> 8);
            packet[offset + 5] = static_cast<uint8_t>(kCarrierLevel & 0xFF);
        }
    }
}

void fill_tx_samples(std::array<uint8_t, 1032>& packet, const int16_t* iq) noexcept {
    for (size_t block = 0; block < 2; ++block) {
        const size_t samples = 8 + block * 512 + 8;
        for (size_t group = 0; group < 63; ++group) {
            const size_t offset = samples + group * 8;
            const size_t index = (block * 63 + group) * 2;
            packet[offset + 4] = static_cast<uint8_t>(static_cast<uint16_t>(iq[index]) >> 8);
            packet[offset + 5] = static_cast<uint8_t>(static_cast<uint16_t>(iq[index]) & 0xFF);
            packet[offset + 6] = static_cast<uint8_t>(static_cast<uint16_t>(iq[index + 1]) >> 8);
            packet[offset + 7] = static_cast<uint8_t>(static_cast<uint16_t>(iq[index + 1]) & 0xFF);
        }
    }
}

} // namespace

std::array<uint8_t, 60> make_metis_iq_stream_command(bool enabled) noexcept {
    std::array<uint8_t, 60> command{};
    command[0] = 0xEF;
    command[1] = 0xFE;
    command[2] = 0x04;
    command[3] = enabled ? 0x01 : 0x00;
    return command;
}

std::array<uint8_t, 1032> make_metis_transmit_config_packet(uint32_t frequency_hz,
                                                           const MetisTransmitControl& control) noexcept {
    std::array<uint8_t, 1032> packet{};
    packet[0] = 0xEF;
    packet[1] = 0xFE;
    packet[2] = 0x01;
    packet[3] = 0x02;
    const uint8_t mox = control.mox ? 0x01 : 0x00;
    for (size_t block = 0; block < 2; ++block) {
        const size_t offset = 8 + block * 512;
        packet[offset] = 0x7F;
        packet[offset + 1] = 0x7F;
        packet[offset + 2] = 0x7F;
        if (block == 0) {
            packet[offset + 3] = static_cast<uint8_t>(0x02 | mox);
            packet[offset + 4] = static_cast<uint8_t>(frequency_hz >> 24);
            packet[offset + 5] = static_cast<uint8_t>(frequency_hz >> 16);
            packet[offset + 6] = static_cast<uint8_t>(frequency_hz >> 8);
            packet[offset + 7] = static_cast<uint8_t>(frequency_hz);
        }
        else {
            packet[offset + 3] = static_cast<uint8_t>(0x12 | mox);
            packet[offset + 4] = control.mox ? control.drive : 0;
            // C2 bit 3 enables the PA; it is only set while keyed.
            packet[offset + 5] = control.mox ? 0x08 : 0x00;
        }
    }
    return packet;
}

std::array<uint8_t, 1032> make_metis_receiver_config_packet(uint32_t frequency_hz) noexcept {
    std::array<uint8_t, 1032> packet{};
    packet[0] = 0xEF;
    packet[1] = 0xFE;
    packet[2] = 0x01;
    packet[3] = 0x02;
    for (size_t block = 0; block < 2; ++block) {
        const size_t offset = 8 + block * 512;
        packet[offset] = 0x7F;
        packet[offset + 1] = 0x7F;
        packet[offset + 2] = 0x7F;
        if (block == 0) {
            packet[offset + 3] = 0x00;
            packet[offset + 4] = 0xE4;
            packet[offset + 5] = static_cast<uint8_t>(receive_filter_outputs(frequency_hz) << 1);
            packet[offset + 6] = 0x04;
            packet[offset + 7] = 0x04;
        }
        else {
            packet[offset + 3] = 0x04;
            packet[offset + 4] = static_cast<uint8_t>(frequency_hz >> 24);
            packet[offset + 5] = static_cast<uint8_t>(frequency_hz >> 16);
            packet[offset + 6] = static_cast<uint8_t>(frequency_hz >> 8);
            packet[offset + 7] = static_cast<uint8_t>(frequency_hz);
        }
    }
    return packet;
}

MetisIqStream::~MetisIqStream() {
    stop();
    delete radio_address_;
}

bool MetisIqStream::open(const std::string& radio_address, std::string& error) {
    stop();
    delete radio_address_;
    radio_address_ = nullptr;
    error.clear();

    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        error = std::strerror(errno);
        return false;
    }

    int reuse_address = 1;
    setsockopt(socket_fd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &reuse_address,
               sizeof(reuse_address));

    sockaddr_in local_address{};
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(0);
    if (bind(socket_fd,
             reinterpret_cast<const sockaddr*>(&local_address),
             sizeof(local_address)) < 0) {
        error = std::strerror(errno);
        close(socket_fd);
        return false;
    }

    auto* destination = new sockaddr_in{};
    destination->sin_family = AF_INET;
    destination->sin_port = htons(kMetisPort);
    if (inet_pton(AF_INET, radio_address.c_str(), &destination->sin_addr) != 1) {
        error = "Invalid Metis radio IPv4 address";
        delete destination;
        close(socket_fd);
        return false;
    }

    socket_fd_ = socket_fd;
    radio_address_ = destination;
    return true;
}

bool MetisIqStream::start(std::string& error) {
    error.clear();
    if (socket_fd_ < 0 || !radio_address_) {
        error = "Metis stream is not open";
        return false;
    }

    const auto command = make_metis_iq_stream_command(true);
    const ssize_t sent = sendto(socket_fd_,
                                command.data(),
                                command.size(),
                                0,
                                reinterpret_cast<const sockaddr*>(radio_address_),
                                sizeof(*radio_address_));
    if (sent != static_cast<ssize_t>(command.size())) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool MetisIqStream::configure_receiver(uint32_t frequency_hz,
                                       std::string& error,
                                       const MetisTransmitControl& transmit) {
    error.clear();
    if (socket_fd_ < 0 || !radio_address_) {
        error = "Metis stream is not open";
        return false;
    }

    auto receiver = make_metis_receiver_config_packet(frequency_hz);
    if (transmit.mox) {
        // MOX is bit 0 of every C0 byte.
        receiver[11] |= 0x01;
        receiver[523] |= 0x01;
    }
    const auto send_packet = [&](const std::array<uint8_t, 1032>& packet) {
        const ssize_t sent = sendto(socket_fd_,
                                    packet.data(),
                                    packet.size(),
                                    0,
                                    reinterpret_cast<const sockaddr*>(radio_address_),
                                    sizeof(*radio_address_));
        if (sent != static_cast<ssize_t>(packet.size())) {
            error = std::strerror(errno);
            return false;
        }
        return true;
    };
    if (!send_packet(receiver)) return false;
    // Receive-only operation sends exactly the original receiver frame.
    if (!transmit.mox && !transmit.release) return true;
    return send_packet(make_metis_transmit_config_packet(frequency_hz, transmit));
}

bool MetisIqStream::send_transmit_frame(uint32_t frequency_hz,
                                        const MetisTransmitControl& transmit,
                                        uint32_t phase,
                                        std::string& error) {
    error.clear();
    if (socket_fd_ < 0 || !radio_address_) {
        error = "Metis stream is not open";
        return false;
    }
    std::array<uint8_t, 1032> packet{};
    if (phase % 2 == 0) {
        packet = make_metis_receiver_config_packet(frequency_hz);
        if (transmit.mox) {
            packet[11] |= 0x01;
            packet[523] |= 0x01;
        }
    }
    else {
        packet = make_metis_transmit_config_packet(frequency_hz, transmit);
    }
    if (transmit.mox && transmit.carrier) fill_tx_carrier(packet);
    else if (transmit.mox && transmit.iq) fill_tx_samples(packet, transmit.iq);
    const ssize_t sent = sendto(socket_fd_,
                                packet.data(),
                                packet.size(),
                                0,
                                reinterpret_cast<const sockaddr*>(radio_address_),
                                sizeof(*radio_address_));
    if (sent != static_cast<ssize_t>(packet.size())) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool MetisIqStream::receive(uint8_t* data,
                            size_t capacity,
                            uint32_t timeout_ms,
                            size_t& received,
                            std::string& error) {
    received = 0;
    error.clear();
    if (socket_fd_ < 0) {
        error = "Metis stream is not open";
        return false;
    }
    if (!data || capacity == 0) {
        error = "Metis receive buffer is empty";
        return false;
    }

    timeval wait_time{};
    wait_time.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    wait_time.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_fd_, &read_set);
    const int ready = select(socket_fd_ + 1, &read_set, nullptr, nullptr, &wait_time);
    if (ready < 0) {
        if (errno == EINTR) {
            return true;
        }
        error = std::strerror(errno);
        return false;
    }
    if (ready == 0) {
        return true;
    }

    sockaddr_in sender{};
    socklen_t sender_size = sizeof(sender);
    const ssize_t result = recvfrom(socket_fd_,
                                    data,
                                    capacity,
                                    0,
                                    reinterpret_cast<sockaddr*>(&sender),
                                    &sender_size);
    if (result < 0) {
        error = std::strerror(errno);
        return false;
    }
    received = static_cast<size_t>(result);
    return true;
}

void MetisIqStream::stop() {
    if (socket_fd_ < 0 || !radio_address_) {
        return;
    }
    const auto command = make_metis_iq_stream_command(false);
    sendto(socket_fd_,
           command.data(),
           command.size(),
           0,
           reinterpret_cast<const sockaddr*>(radio_address_),
           sizeof(*radio_address_));
    close(socket_fd_);
    socket_fd_ = -1;
}

} // namespace platform::sdr