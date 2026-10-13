// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "backend.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "imgui.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

const char* const SwitchButtonMapNames[] = {
    "B", "A", "Y", "X", "Home", "Minus", "Plus", "LS", "RS", "L", "R", "Up", "Down", "Left", "Right",
    "Capture", "N/A", "N/A", "N/A", "N/A", "N/A", "N/A",
    "Nothing",
    "ZL", "ZR"
};
const char* const StadiaButtonMapNames[] = {
    "A", "B", "X", "Y", "Stadia", "Options", "Menu", "L3", "R3", "L1", "R1", "Up", "Down", "Left", "Right",
    "Capture", "N/A", "N/A", "N/A", "N/A", "N/A", "Assistant",
    "Nothing",
    "L2", "R2"
};

const ButtonMap SwitchValidOutputs[] = {
    ButtonMap::South, ButtonMap::East, ButtonMap::West, ButtonMap::North,
    ButtonMap::DpadLeft, ButtonMap::DpadRight, ButtonMap::DpadUp, ButtonMap::DpadDown,
    ButtonMap::L1, ButtonMap::R1, ButtonMap::L2, ButtonMap::R2,
    ButtonMap::L3, ButtonMap::R3,
    ButtonMap::Select, ButtonMap::Start, ButtonMap::Home,
    ButtonMap::Misc1, ButtonMap::Nothing
};

const ButtonMap StadiaValidOutputs[] = {
    ButtonMap::South, ButtonMap::East, ButtonMap::West, ButtonMap::North,
    ButtonMap::DpadLeft, ButtonMap::DpadRight, ButtonMap::DpadUp, ButtonMap::DpadDown,
    ButtonMap::L1, ButtonMap::R1, ButtonMap::L2, ButtonMap::R2,
    ButtonMap::L3, ButtonMap::R3,
    ButtonMap::Select, ButtonMap::Start, ButtonMap::Home,
    ButtonMap::Misc1, ButtonMap::Misc2, ButtonMap::Nothing
};

std::atomic<HANDLE> serial_port{ INVALID_HANDLE_VALUE };
std::atomic<bool> is_connecting_serial{ false };
std::atomic<bool> last_serial_error{ false };
std::vector<std::string> available_ports;
int selected_port_idx = 0;
uint64_t last_send_time = 0;

enum TransmissionMode {
    MODE_SERIAL = 0,
    MODE_NETWORK = 1
};
int current_mode = MODE_SERIAL;
enum OutputControllerType {
    OUTPUT_SWITCH = 0,
    OUTPUT_STADIA = 1
};
int output_controller_type = OUTPUT_SWITCH;
char network_address[256] = "";
char serial_port_name[256] = "";
SOCKET udp_socket = INVALID_SOCKET;
bool last_udp_error = false;

std::vector<std::string> get_com_ports() {
    std::vector<std::string> ports;
    char path[5000];
    for (int i = 1; i < 256; i++) {
        std::string comName = "COM" + std::to_string(i);
        DWORD res = QueryDosDeviceA(comName.c_str(), path, sizeof(path));
        if (res != 0) {
            ports.push_back(comName);
        }
    }
    return ports;
}

std::thread tx_thread;
std::mutex tx_mutex;
std::condition_variable tx_cv;
std::vector<uint8_t> tx_shared_buf;
std::atomic<bool> tx_running{ false };
std::atomic<bool> tx_has_data{ false };

void close_serial_port() {
    HANDLE p = serial_port.exchange(INVALID_HANDLE_VALUE);
    if (p != INVALID_HANDLE_VALUE) {
        CloseHandle(p);
    }
    bool expected = true;
    if (tx_running.compare_exchange_strong(expected, false)) {
        tx_cv.notify_all();
        if (tx_thread.joinable()) {
            if (std::this_thread::get_id() != tx_thread.get_id()) {
                tx_thread.join();
            } else {
                tx_thread.detach();
            }
        }
    }
}

void open_serial_port(const std::string& port_name) {
    if (is_connecting_serial.load()) {
        return;
    }

    close_serial_port();

    is_connecting_serial.store(true);
    last_serial_error.store(false);

    std::thread([port_name]() {
        std::string full_port_name = "\\\\.\\" + port_name;
        HANDLE temp_port = CreateFileA(full_port_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (temp_port == INVALID_HANDLE_VALUE) {
            last_serial_error.store(true);
            is_connecting_serial.store(false);
            return;
        }

        DCB dcbSerialParams = { 0 };
        dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
        if (!GetCommState(temp_port, &dcbSerialParams)) {
            CloseHandle(temp_port);
            last_serial_error.store(true);
            is_connecting_serial.store(false);
            return;
        }

        dcbSerialParams.BaudRate = 921600;
        dcbSerialParams.ByteSize = 8;
        dcbSerialParams.StopBits = ONESTOPBIT;
        dcbSerialParams.Parity = NOPARITY;
        if (!SetCommState(temp_port, &dcbSerialParams)) {
            CloseHandle(temp_port);
            last_serial_error.store(true);
            is_connecting_serial.store(false);
            return;
        }

        COMMTIMEOUTS timeouts = { 0 };
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 50;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.WriteTotalTimeoutConstant = 50;
        timeouts.WriteTotalTimeoutMultiplier = 10;
        SetCommTimeouts(temp_port, &timeouts);

        serial_port.store(temp_port);

        tx_running.store(true);
        tx_has_data.store(false);
        tx_thread = std::thread([]() {
            while (tx_running.load()) {
                std::vector<uint8_t> local_buf;
                {
                    std::unique_lock<std::mutex> lock(tx_mutex);
                    tx_cv.wait(lock, [] { return tx_has_data.load() || !tx_running.load(); });
                    if (!tx_running.load()) {
                        break;
                    }

                    local_buf = std::move(tx_shared_buf);
                    tx_has_data.store(false);
                }

                HANDLE port = serial_port.load();
                if (port == INVALID_HANDLE_VALUE) {
                    continue;
                }

                DWORD written = 0;
                if (!WriteFile(port, local_buf.data(), (DWORD) local_buf.size(), &written, NULL) || written != local_buf.size()) {
                    close_serial_port();
                    last_serial_error.store(true);
                }
            }
        });

        is_connecting_serial.store(false);
    }).detach();
}

uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

const uint8_t SLIP_END = 0xC0;      // 0o300
const uint8_t SLIP_ESC = 0xDB;      // 0o333
const uint8_t SLIP_ESC_END = 0xDC;  // 0o334
const uint8_t SLIP_ESC_ESC = 0xDD;  // 0o335

void send_escaped_byte(std::vector<uint8_t>& buf, uint8_t b) {
    if (b == SLIP_END) {
        buf.push_back(SLIP_ESC);
        buf.push_back(SLIP_ESC_END);
    } else if (b == SLIP_ESC) {
        buf.push_back(SLIP_ESC);
        buf.push_back(SLIP_ESC_ESC);
    } else {
        buf.push_back(b);
    }
}

void send_serial_data(const uint8_t* data, size_t length) {
    HANDLE port = serial_port.load();
    if (port == INVALID_HANDLE_VALUE) {
        return;
    }

    uint32_t crc = crc32(data, length);
    std::vector<uint8_t> tx_buf;
    tx_buf.push_back(SLIP_END);
    for (size_t i = 0; i < length; i++) {
        send_escaped_byte(tx_buf, data[i]);
    }
    send_escaped_byte(tx_buf, (crc >> 0) & 0xFF);
    send_escaped_byte(tx_buf, (crc >> 8) & 0xFF);
    send_escaped_byte(tx_buf, (crc >> 16) & 0xFF);
    send_escaped_byte(tx_buf, (crc >> 24) & 0xFF);
    tx_buf.push_back(SLIP_END);

    {
        std::lock_guard<std::mutex> lock(tx_mutex);
        tx_shared_buf = std::move(tx_buf);
        tx_has_data.store(true);
    }
    tx_cv.notify_one();
}

void send_udp_data(const uint8_t* data, size_t length) {
    if (udp_socket == INVALID_SOCKET || strlen(network_address) == 0) {
        return;
    }

    sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(42734);
    if (inet_pton(AF_INET, network_address, &dest.sin_addr) != 1) {
        last_udp_error = true;
        return;
    }

    int res = sendto(udp_socket, (const char*) data, (int) length, 0, (sockaddr*) &dest, sizeof(dest));
    last_udp_error = (res == SOCKET_ERROR);
}

size_t build_switch_report(const ControllerState& state, uint8_t* data) {
    data[0] = 1;  // Protocol Version
    data[1] = 2;  // Switch Gamepad Descriptor Number
    data[2] = 8;  // Length
    data[3] = 0;  // Report ID

    uint8_t b = state.south;
    uint8_t a = state.east;
    uint8_t y = state.west;
    uint8_t x = state.north;
    uint8_t l = state.l1;
    uint8_t r = state.r1;
    uint8_t zl = state.l2 > 64;
    uint8_t zr = state.r2 > 64;

    data[4] = (y << 0) | (b << 1) | (a << 2) | (x << 3) | (l << 4) | (r << 5) | (zl << 6) | (zr << 7);

    uint8_t minus = state.select;
    uint8_t plus = state.start;
    uint8_t ls = state.l3;
    uint8_t rs = state.r3;
    uint8_t home = state.home;
    uint8_t capture = state.misc1;

    data[5] = (minus << 0) | (plus << 1) | (ls << 2) | (rs << 3) | (home << 4) | (capture << 5);

    uint8_t dpad_lut[] = { 15, 6, 2, 15, 0, 7, 1, 0, 4, 5, 3, 4, 15, 6, 2, 15 };
    uint8_t dpad_index = (state.dpad_left << 0) | (state.dpad_right << 1) | (state.dpad_up << 2) | (state.dpad_down << 3);
    data[6] = dpad_lut[dpad_index];

    auto map_axis = [](int16_t val) { return (uint8_t) ((val + 32768) / 257); };
    data[7] = map_axis(state.left_stick_x);
    data[8] = map_axis(state.left_stick_y);
    data[9] = map_axis(state.right_stick_x);
    data[10] = map_axis(state.right_stick_y);
    data[11] = 0;

    return 12;
}

size_t build_stadia_report(const ControllerState& state, uint8_t* data) {
    data[0] = 1;   // Protocol Version
    data[1] = 4;   // Stadia Descriptor Number
    data[2] = 10;  // Length
    data[3] = 3;   // Report ID

    uint8_t std_dpad_lut[16] = {
        8, 6, 2, 8, 0, 7, 1, 0, 4, 5, 3, 4, 8, 6, 2, 8
    };
    uint8_t dpad_index = (state.dpad_left << 0) | (state.dpad_right << 1) | (state.dpad_up << 2) | (state.dpad_down << 3);
    data[4] = std_dpad_lut[dpad_index];

    uint8_t cap = state.misc1;
    uint8_t assistant = state.misc2;
    uint8_t l2 = state.l2 > 64;
    uint8_t r2 = state.r2 > 64;
    uint8_t stadia = state.home;
    uint8_t menu = state.start;
    uint8_t options = state.select;
    uint8_t r3 = state.r3;
    data[5] = (cap << 0) | (assistant << 1) | (l2 << 2) | (r2 << 3) | (stadia << 4) | (menu << 5) | (options << 6) | (r3 << 7);

    uint8_t l3 = state.l3;
    uint8_t r1 = state.r1;
    uint8_t l1 = state.l1;
    uint8_t y = state.north;
    uint8_t x = state.west;
    uint8_t b = state.east;
    uint8_t a = state.south;
    data[6] = (l3 << 0) | (r1 << 1) | (l1 << 2) | (y << 3) | (x << 4) | (b << 5) | (a << 6);

    auto map_axis = [](int16_t val) {
        uint8_t mapped = (uint8_t) ((val + 32768) / 257);
        return mapped == 0 ? (uint8_t) 1 : mapped;
    };
    data[7] = map_axis(state.left_stick_x);
    data[8] = map_axis(state.left_stick_y);
    data[9] = map_axis(state.right_stick_x);
    data[10] = map_axis(state.right_stick_y);
    data[11] = state.l2;
    data[12] = state.r2;
    data[13] = 0;  // extra_buttons

    return 14;
}


const char* const BACKEND_APP_NAME = "sdl-transmitter";
const bool BACKEND_HAS_RUMBLE = false;
const bool BACKEND_DELAY_AFTER_SEND = true;

bool backend_init() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
        udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        u_long mode = 1;
        ioctlsocket(udp_socket, FIONBIO, &mode);
    }
    return true;
}

void backend_shutdown() {
    close_serial_port();
    if (udp_socket != INVALID_SOCKET) {
        closesocket(udp_socket);
    }
    WSACleanup();
}

bool backend_load_setting(const std::string& key, const nlohmann::json& value) {
    if (key == "transmission_mode") {
        current_mode = value.get<int>();
        return true;
    }
    if (key == "output_controller_type") {
        output_controller_type = value.get<int>();
        return true;
    }
    if (key == "network_address") {
        std::string addr = value.get<std::string>();
        snprintf(network_address, sizeof(network_address), "%s", addr.c_str());
        return true;
    }
    if (key == "serial_port_name") {
        std::string port = value.get<std::string>();
        snprintf(serial_port_name, sizeof(serial_port_name), "%s", port.c_str());
        return true;
    }
    return false;
}

void backend_save_settings(nlohmann::json& j) {
    j["transmission_mode"] = current_mode;
    j["output_controller_type"] = output_controller_type;
    j["network_address"] = std::string(network_address);
    j["serial_port_name"] = std::string(serial_port_name);
}

const char* const* backend_button_map_names() {
    return output_controller_type == 0 ? SwitchButtonMapNames : StadiaButtonMapNames;
}

const ButtonMap* backend_valid_outputs(int* count) {
    *count = (output_controller_type == 0) ? (sizeof(SwitchValidOutputs) / sizeof(ButtonMap)) : (sizeof(StadiaValidOutputs) / sizeof(ButtonMap));
    return (output_controller_type == 0) ? SwitchValidOutputs : StadiaValidOutputs;
}

bool backend_ignore_gamepad(SDL_Gamepad* gamepad) {
    return false;
}

bool backend_default_enabled(const char* name) {
    return !(name && strstr(name, "HID Receiver") != nullptr);
}

void backend_on_device_change() {
}

void backend_update() {
}

bool backend_is_connected() {
    if (current_mode == MODE_SERIAL) {
        return serial_port.load() != INVALID_HANDLE_VALUE;
    } else {
        return !last_udp_error && strlen(network_address) > 0;
    }
}

bool backend_send_state(const ControllerState& state) {
    uint64_t now = SDL_GetTicksNS();
    if (now - last_send_time < 4000000) {
        return false;
    }

    uint8_t data[64];
    size_t length = 0;
    if (output_controller_type == OUTPUT_SWITCH) {
        length = build_switch_report(state, data);
    } else {
        length = build_stadia_report(state, data);
    }

    if (current_mode == MODE_SERIAL) {
        send_serial_data(data, length);
    } else {
        send_udp_data(data, length);
    }
    last_send_time = now;
    return true;
}

void backend_draw_ui() {
    bool connecting = is_connecting_serial.load();
    if (connecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::RadioButton("Serial", &current_mode, MODE_SERIAL)) {
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Network", &current_mode, MODE_NETWORK)) {
    }
    if (connecting) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("  Output:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* output_types[] = { "Switch", "Stadia" };
    if (ImGui::BeginCombo("##output_type", output_types[output_controller_type])) {
        for (int n = 0; n < 2; n++) {
            bool is_selected = (output_controller_type == n);
            if (ImGui::Selectable(output_types[n], is_selected)) {
                if (output_controller_type != n) {
                    output_controller_type = n;
                    const ButtonMap* valid_outputs = (n == 0) ? SwitchValidOutputs : StadiaValidOutputs;
                    int valid_outputs_count = (n == 0) ? (sizeof(SwitchValidOutputs) / sizeof(ButtonMap)) : (sizeof(StadiaValidOutputs) / sizeof(ButtonMap));

                    for (auto& pair : controllers) {
                        ControllerInfo& ctrl = pair.second;
                        for (int i = 0; i < NUM_BUTTONS; ++i) {
                            bool is_valid = false;
                            for (int k = 0; k < valid_outputs_count; ++k) {
                                if (ctrl.btn_map[i] == valid_outputs[k]) {
                                    is_valid = true;
                                    break;
                                }
                            }
                            if (!is_valid) {
                                ctrl.btn_map[i] = ButtonMap::Nothing;
                                controller_settings[ctrl.key].btn_map[i] = ButtonMap::Nothing;
                            }
                        }
                        for (int i = 0; i < 2; ++i) {
                            bool is_valid = false;
                            for (int k = 0; k < valid_outputs_count; ++k) {
                                if (ctrl.trigger_map[i] == valid_outputs[k]) {
                                    is_valid = true;
                                    break;
                                }
                            }
                            if (!is_valid) {
                                ctrl.trigger_map[i] = ButtonMap::Nothing;
                                controller_settings[ctrl.key].trigger_map[i] = ButtonMap::Nothing;
                            }
                        }
                    }
                }
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if (current_mode == MODE_SERIAL) {
        if (available_ports.empty()) {
            available_ports = get_com_ports();
            for (int i = 0; i < available_ports.size(); ++i) {
                if (available_ports[i] == serial_port_name) {
                    selected_port_idx = i;
                    break;
                }
            }
        }

        if (serial_port.load() != INVALID_HANDLE_VALUE) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Connected to %s", serial_port_name);
            if (ImGui::Button("Disconnect")) {
                close_serial_port();
            }
        } else {
            if (connecting) {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Connecting to %s...", serial_port_name);
            } else if (last_serial_error.load()) {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "Connection to %s failed", serial_port_name);
            } else {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "Not connected");
            }

            if (connecting) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Refresh")) {
                available_ports = get_com_ports();
                selected_port_idx = 0;
                for (int i = 0; i < available_ports.size(); ++i) {
                    if (available_ports[i] == serial_port_name) {
                        selected_port_idx = i;
                        break;
                    }
                }
            }
            ImGui::SameLine();

            std::string combo_preview = (selected_port_idx >= 0 && selected_port_idx < available_ports.size()) ? available_ports[selected_port_idx] : "";
            ImGui::SetNextItemWidth(100);
            if (ImGui::BeginCombo("##com_port_select", combo_preview.c_str())) {
                for (int i = 0; i < available_ports.size(); i++) {
                    bool is_selected = (selected_port_idx == i);
                    if (ImGui::Selectable(available_ports[i].c_str(), is_selected)) {
                        selected_port_idx = i;
                        snprintf(serial_port_name, sizeof(serial_port_name), "%s", available_ports[i].c_str());
                    }
                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Connect")) {
                if (selected_port_idx >= 0 && selected_port_idx < available_ports.size()) {
                    open_serial_port(available_ports[selected_port_idx]);
                }
            }
            if (connecting) {
                ImGui::EndDisabled();
            }
        }
    } else {
        ImGui::Text("Receiver IP Address:");
        ImGui::SameLine();
        ImGui::InputText("##network_address", network_address, sizeof(network_address));
        if (last_udp_error) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Network Error: Failed to send UDP packet");
        }
    }
}
