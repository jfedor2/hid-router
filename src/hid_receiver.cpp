// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "hid_receiver.h"

#include "common.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "imgui.h"

#include <cstring>
#include <map>
#include <utility>

const uint16_t RECEIVER_PORT = 42734;
const uint8_t PROTOCOL_VERSION = 1;
const int MAX_ENDPOINTS = 16;
const int MAX_PACKETS_PER_UPDATE = 64;

enum OurDescriptorNumber {
    DESCRIPTOR_SWITCH = 2,
    DESCRIPTOR_PS4 = 3,
    DESCRIPTOR_STADIA = 4,
    DESCRIPTOR_XAC_COMPAT = 5
};

bool receiver_enabled = false;
bool receiver_localhost_only = true;

bool wsa_started = false;
SOCKET receiver_socket = INVALID_SOCKET;
bool receiver_socket_localhost_only = false;
bool receiver_listen_error = false;
Uint64 last_listen_attempt = 0;

// IP address (host byte order) and port of the transmitter.
typedef std::pair<uint32_t, uint16_t> Endpoint;
std::map<Endpoint, SDL_JoystickID> endpoints;
SDL_JoystickID next_controller_id = NETWORK_CONTROLLER_ID_BASE;

bool is_loopback(const Endpoint& endpoint) {
    return (endpoint.first >> 24) == 127;
}

std::string ip_to_string(uint32_t ip) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return std::string(buf);
}

void remove_endpoints(bool loopback_too) {
    for (auto it = endpoints.begin(); it != endpoints.end();) {
        if (loopback_too || !is_loopback(it->first)) {
            controllers.erase(it->second);
            it = endpoints.erase(it);
        } else {
            ++it;
        }
    }
}

void stop_listening() {
    if (receiver_socket != INVALID_SOCKET) {
        closesocket(receiver_socket);
        receiver_socket = INVALID_SOCKET;
    }
}

void start_listening() {
    receiver_listen_error = true;

    if (!wsa_started) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return;
        }
        wsa_started = true;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RECEIVER_PORT);
    addr.sin_addr.s_addr = htonl(receiver_localhost_only ? INADDR_LOOPBACK : INADDR_ANY);

    u_long mode = 1;
    if (bind(s, (sockaddr*) &addr, sizeof(addr)) == SOCKET_ERROR || ioctlsocket(s, FIONBIO, &mode) == SOCKET_ERROR) {
        closesocket(s);
        return;
    }

    receiver_socket = s;
    receiver_socket_localhost_only = receiver_localhost_only;
    receiver_listen_error = false;
}

int16_t axis_from_u8(uint8_t val) {
    // inverse of what the transmitter does
    return (int16_t) (val * 257 - 32768);
}

void set_dpad(ControllerInfo& info, uint8_t hat) {
    info.raw_btns[static_cast<int>(ButtonMap::DpadUp)] = (hat == 7 || hat == 0 || hat == 1);
    info.raw_btns[static_cast<int>(ButtonMap::DpadRight)] = (hat >= 1 && hat <= 3);
    info.raw_btns[static_cast<int>(ButtonMap::DpadDown)] = (hat >= 3 && hat <= 5);
    info.raw_btns[static_cast<int>(ButtonMap::DpadLeft)] = (hat >= 5 && hat <= 7);
}

// Returns true if the report was a gamepad report that we understand.
// If info is null, the report is only validated.
bool parse_report(uint8_t descriptor_number, uint8_t report_id, const uint8_t* data, int len, ControllerInfo* info) {
    bool btns[NUM_BUTTONS] = { 0 };
    bool present[NUM_BUTTONS] = { 0 };
    uint8_t trigs[NUM_TRIGGERS] = { 0 };
    const uint8_t* sticks = nullptr;
    uint8_t hat = 0;

    auto btn = [&](ButtonMap b) -> bool& { return btns[static_cast<int>(b)]; };
    auto has = [&](ButtonMap b) { present[static_cast<int>(b)] = true; };
    auto bit = [&](int byte, int n) { return ((data[byte] >> n) & 1) != 0; };

    switch (descriptor_number) {
        case DESCRIPTOR_SWITCH:
            if (report_id != 0 || len < 7) {
                return false;
            }
            btn(ButtonMap::West) = bit(0, 0);
            btn(ButtonMap::South) = bit(0, 1);
            btn(ButtonMap::East) = bit(0, 2);
            btn(ButtonMap::North) = bit(0, 3);
            btn(ButtonMap::L1) = bit(0, 4);
            btn(ButtonMap::R1) = bit(0, 5);
            trigs[0] = bit(0, 6) ? 255 : 0;
            trigs[1] = bit(0, 7) ? 255 : 0;
            btn(ButtonMap::Select) = bit(1, 0);
            btn(ButtonMap::Start) = bit(1, 1);
            btn(ButtonMap::L3) = bit(1, 2);
            btn(ButtonMap::R3) = bit(1, 3);
            btn(ButtonMap::Home) = bit(1, 4);
            btn(ButtonMap::Misc1) = bit(1, 5);
            hat = data[2] & 0x0F;
            sticks = &data[3];
            has(ButtonMap::Home);
            has(ButtonMap::Misc1);
            break;
        case DESCRIPTOR_STADIA:
            if (report_id != 3 || len < 9) {
                return false;
            }
            hat = data[0] & 0x0F;
            btn(ButtonMap::Misc1) = bit(1, 0);
            btn(ButtonMap::Misc2) = bit(1, 1);
            btn(ButtonMap::Home) = bit(1, 4);
            btn(ButtonMap::Start) = bit(1, 5);
            btn(ButtonMap::Select) = bit(1, 6);
            btn(ButtonMap::R3) = bit(1, 7);
            btn(ButtonMap::L3) = bit(2, 0);
            btn(ButtonMap::R1) = bit(2, 1);
            btn(ButtonMap::L1) = bit(2, 2);
            btn(ButtonMap::North) = bit(2, 3);
            btn(ButtonMap::West) = bit(2, 4);
            btn(ButtonMap::East) = bit(2, 5);
            btn(ButtonMap::South) = bit(2, 6);
            sticks = &data[3];
            trigs[0] = data[7] ? data[7] : (bit(1, 2) ? 255 : 0);
            trigs[1] = data[8] ? data[8] : (bit(1, 3) ? 255 : 0);
            has(ButtonMap::Home);
            has(ButtonMap::Misc1);
            has(ButtonMap::Misc2);
            break;
        case DESCRIPTOR_PS4:
            if (report_id != 1 || len < 9) {
                return false;
            }
            sticks = &data[0];
            hat = data[4] & 0x0F;
            btn(ButtonMap::West) = bit(4, 4);
            btn(ButtonMap::South) = bit(4, 5);
            btn(ButtonMap::East) = bit(4, 6);
            btn(ButtonMap::North) = bit(4, 7);
            btn(ButtonMap::L1) = bit(5, 0);
            btn(ButtonMap::R1) = bit(5, 1);
            btn(ButtonMap::Select) = bit(5, 4);
            btn(ButtonMap::Start) = bit(5, 5);
            btn(ButtonMap::L3) = bit(5, 6);
            btn(ButtonMap::R3) = bit(5, 7);
            btn(ButtonMap::Home) = bit(6, 0);
            btn(ButtonMap::Touchpad) = bit(6, 1);
            trigs[0] = data[7] ? data[7] : (bit(5, 2) ? 255 : 0);
            trigs[1] = data[8] ? data[8] : (bit(5, 3) ? 255 : 0);
            has(ButtonMap::Home);
            has(ButtonMap::Touchpad);
            break;
        case DESCRIPTOR_XAC_COMPAT:
            if (report_id != 0 || len < 6) {
                return false;
            }
            sticks = &data[0];
            hat = data[4] & 0x0F;
            // X1 and X2
            btn(ButtonMap::Misc1) = bit(4, 4);
            btn(ButtonMap::Misc2) = bit(4, 5);
            btn(ButtonMap::L3) = bit(4, 6);
            btn(ButtonMap::L1) = bit(4, 7);
            btn(ButtonMap::South) = bit(5, 0);
            btn(ButtonMap::East) = bit(5, 1);
            btn(ButtonMap::Select) = bit(5, 2);
            btn(ButtonMap::Start) = bit(5, 3);
            btn(ButtonMap::R3) = bit(5, 4);
            btn(ButtonMap::R1) = bit(5, 5);
            btn(ButtonMap::West) = bit(5, 6);
            btn(ButtonMap::North) = bit(5, 7);
            has(ButtonMap::Misc1);
            has(ButtonMap::Misc2);
            break;
        default:
            // not a gamepad
            return false;
    }

    if (info) {
        for (int i = 0; i < NUM_BUTTONS; ++i) {
            info->raw_btns[i] = btns[i];
            info->net_btns_present[i] = present[i];
        }
        set_dpad(*info, hat);
        for (int i = 0; i < NUM_TRIGGERS; ++i) {
            info->raw_trigs[i] = trigs[i];
        }
        for (int i = 0; i < NUM_STICK_AXES; ++i) {
            info->raw_sticks[i] = axis_from_u8(sticks[i]);
        }
    }

    return true;
}

void handle_packet(const Endpoint& endpoint, const uint8_t* packet, int len) {
    // protocol version, descriptor number, length, report ID, report
    if (len < 4) {
        return;
    }
    int report_len = len - 4;
    if (packet[0] != PROTOCOL_VERSION || packet[2] != report_len || report_len > 64) {
        return;
    }
    uint8_t descriptor_number = packet[1];
    uint8_t report_id = packet[3];
    const uint8_t* report = packet + 4;

    auto it = endpoints.find(endpoint);
    if (it == endpoints.end()) {
        if (endpoints.size() >= MAX_ENDPOINTS || !parse_report(descriptor_number, report_id, report, report_len, nullptr)) {
            return;
        }
        std::string ip = ip_to_string(endpoint.first);
        std::string name = "Network " + ip + ":" + std::to_string(endpoint.second);
        // The port is likely to be different every time so settings are per address.
        AddController(next_controller_id, nullptr, name.c_str(), "udp_" + ip);
        it = endpoints.emplace(endpoint, next_controller_id).first;
        next_controller_id++;
    }

    auto ctrl = controllers.find(it->second);
    if (ctrl != controllers.end()) {
        parse_report(descriptor_number, report_id, report, report_len, &ctrl->second);
    }
}

bool hid_receiver_load_setting(const std::string& key, const nlohmann::json& value) {
    if (key == "hid_receiver_enabled") {
        if (value.is_boolean()) {
            receiver_enabled = value.get<bool>();
        }
        return true;
    }
    if (key == "hid_receiver_localhost_only") {
        if (value.is_boolean()) {
            receiver_localhost_only = value.get<bool>();
        }
        return true;
    }
    return false;
}

void hid_receiver_save_settings(nlohmann::json& j) {
    j["hid_receiver_enabled"] = receiver_enabled;
    j["hid_receiver_localhost_only"] = receiver_localhost_only;
}

void hid_receiver_update() {
    if (receiver_socket != INVALID_SOCKET && (!receiver_enabled || receiver_socket_localhost_only != receiver_localhost_only)) {
        stop_listening();
        last_listen_attempt = 0;
    }

    if (!receiver_enabled) {
        remove_endpoints(true);
        receiver_listen_error = false;
        last_listen_attempt = 0;
        return;
    }

    if (receiver_localhost_only) {
        remove_endpoints(false);
    }

    if (receiver_socket == INVALID_SOCKET) {
        Uint64 now = SDL_GetTicks();
        if (last_listen_attempt != 0 && now - last_listen_attempt < 1000) {
            return;
        }
        last_listen_attempt = now ? now : 1;
        start_listening();
        if (receiver_socket == INVALID_SOCKET) {
            return;
        }
    }

    for (int i = 0; i < MAX_PACKETS_PER_UPDATE; ++i) {
        uint8_t buf[256];
        sockaddr_in from;
        int from_len = sizeof(from);
        int res = recvfrom(receiver_socket, (char*) buf, sizeof(buf), 0, (sockaddr*) &from, &from_len);
        if (res == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEMSGSIZE || err == WSAECONNRESET) {
                continue;
            }
            break;
        }
        if (from.sin_family != AF_INET) {
            continue;
        }
        handle_packet(Endpoint(ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)), buf, res);
    }
}

void hid_receiver_draw_ui() {
    if (ImGui::Checkbox("Act as HID Receiver", &receiver_enabled)) {
        last_listen_attempt = 0;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Inputs from localhost only", &receiver_localhost_only)) {
        last_listen_attempt = 0;
    }
    if (receiver_enabled && receiver_listen_error) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Failed to listen on UDP port %d", RECEIVER_PORT);
    }
}

void hid_receiver_shutdown() {
    stop_listening();
    if (wsa_started) {
        WSACleanup();
        wsa_started = false;
    }
}
