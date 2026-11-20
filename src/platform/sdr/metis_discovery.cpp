/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "metis_discovery.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace platform::sdr {
namespace {

constexpr uint16_t kMetisPort = 1024;

bool is_discovery_response(const uint8_t* data, size_t size) noexcept {
    return data && size >= 4 && data[0] == 0xEF && data[1] == 0xFE
        && data[2] == 0x02 && data[3] == 0x00;
}

} // namespace

std::array<uint8_t, 4> make_metis_discovery_packet() noexcept {
    return {0xEF, 0xFE, 0x02, 0x00};
}

bool discover_metis_radios(uint32_t timeout_ms,
                           std::vector<MetisRadioEndpoint>& radios,
                           std::string& error) {
    radios.clear();
    error.clear();

    std::vector<int> sockets;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) < 0) {
        error = std::strerror(errno);
        return false;
    }

    const auto packet = make_metis_discovery_packet();
    for (ifaddrs* interface = interfaces; interface; interface = interface->ifa_next) {
        if (!interface->ifa_addr || !interface->ifa_broadaddr
            || interface->ifa_addr->sa_family != AF_INET
            || (interface->ifa_flags & IFF_UP) == 0
            || (interface->ifa_flags & IFF_BROADCAST) == 0
            || (interface->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_fd < 0) {
            continue;
        }
        int broadcast_enabled = 1;
        if (setsockopt(socket_fd,
                       SOL_SOCKET,
                       SO_BROADCAST,
                       &broadcast_enabled,
                       sizeof(broadcast_enabled)) < 0) {
            close(socket_fd);
            continue;
        }

        sockaddr_in local_address = *reinterpret_cast<sockaddr_in*>(interface->ifa_addr);
        local_address.sin_port = htons(0);
        if (bind(socket_fd,
                 reinterpret_cast<const sockaddr*>(&local_address),
                 sizeof(local_address)) < 0) {
            close(socket_fd);
            continue;
        }

        sockaddr_in broadcast_address = *reinterpret_cast<sockaddr_in*>(interface->ifa_broadaddr);
        broadcast_address.sin_port = htons(kMetisPort);
        if (sendto(socket_fd,
                   packet.data(),
                   packet.size(),
                   0,
                   reinterpret_cast<const sockaddr*>(&broadcast_address),
                   sizeof(broadcast_address)) >= 0) {
            sockets.push_back(socket_fd);
        } else {
            close(socket_fd);
        }
    }
    freeifaddrs(interfaces);

    if (sockets.empty()) {
        error = "No active IPv4 broadcast interface is available";
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    std::array<uint8_t, 512> response{};
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            break;
        }

        timeval wait_time{};
        wait_time.tv_sec = static_cast<time_t>(remaining.count() / 1000000);
        wait_time.tv_usec = static_cast<suseconds_t>(remaining.count() % 1000000);
        fd_set read_set;
        FD_ZERO(&read_set);
        int highest_socket = -1;
        for (const int socket_fd : sockets) {
            FD_SET(socket_fd, &read_set);
            highest_socket = std::max(highest_socket, socket_fd);
        }
        const int ready = select(highest_socket + 1, &read_set, nullptr, nullptr, &wait_time);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::strerror(errno);
            for (const int socket_fd : sockets) {
                close(socket_fd);
            }
            return false;
        }
        if (ready == 0) {
            break;
        }

        for (const int socket_fd : sockets) {
            if (!FD_ISSET(socket_fd, &read_set)) {
                continue;
            }

            sockaddr_in sender{};
            socklen_t sender_size = sizeof(sender);
            const ssize_t received = recvfrom(socket_fd,
                                              response.data(),
                                              response.size(),
                                              0,
                                              reinterpret_cast<sockaddr*>(&sender),
                                              &sender_size);
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error = std::strerror(errno);
                for (const int open_socket : sockets) {
                    close(open_socket);
                }
                return false;
            }
            if (!is_discovery_response(response.data(), static_cast<size_t>(received))) {
                continue;
            }

            char address[INET_ADDRSTRLEN]{};
            if (!inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address))) {
                continue;
            }
            const std::string sender_address(address);
            const auto known = std::find_if(radios.begin(), radios.end(),
                                            [&sender_address](const MetisRadioEndpoint& radio) {
                                                return radio.address == sender_address;
                                            });
            if (known == radios.end()) {
                radios.push_back({sender_address, kMetisPort});
            }
        }
    }

    for (const int socket_fd : sockets) {
        close(socket_fd);
    }
    return true;
}

} // namespace platform::sdr