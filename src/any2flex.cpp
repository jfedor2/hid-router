// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "backend.h"

#include <hidapi.h>

#include <atomic>
#include <cstring>
#include <random>
#include <thread>

#include "imgui.h"

// The PC input port of the HORI Flex Controller ("Flex Controller(View)").
const unsigned short FLEX_VID = 0x0F0D;
const unsigned short FLEX_PID = 0x0168;
const int FLEX_INTERFACE = 0;
// The gamepads the Flex presents in Switch and PC mode.
const unsigned short FLEX_PAD_PID = 0x00FD;
const unsigned short FLEX_PAD_PC_PID = 0x014B;

const uint8_t FLEX_MAGIC[] = { 0x78, 0xC1, 0x87, 0x3E };
const uint8_t FLEX_PRESSED = 0x64;
const int FLEX_HANDSHAKE_TIMEOUT_MS = 1000;
// The input endpoint has a 4 ms interval.
const uint64_t FLEX_MIN_SEND_INTERVAL_NS = 4000000;

const char* const SwitchButtonMapNames[] = {
    "B", "A", "Y", "X", "Home", "Minus", "Plus", "LS", "RS", "L", "R", "Up", "Down", "Left", "Right",
    "Capture", "N/A", "N/A", "N/A", "N/A", "N/A", "N/A",
    "Nothing",
    "ZL", "ZR"
};
const char* const XboxButtonMapNames[] = {
    "A", "B", "X", "Y", "Guide", "Back", "Start", "LSB", "RSB", "LB", "RB", "Up", "Down", "Left", "Right",
    "N/A", "N/A", "N/A", "N/A", "N/A", "N/A", "N/A",
    "Nothing",
    "LT", "RT"
};

const ButtonMap SwitchValidOutputs[] = {
    ButtonMap::South, ButtonMap::East, ButtonMap::West, ButtonMap::North,
    ButtonMap::DpadLeft, ButtonMap::DpadRight, ButtonMap::DpadUp, ButtonMap::DpadDown,
    ButtonMap::L1, ButtonMap::R1, ButtonMap::L2, ButtonMap::R2,
    ButtonMap::L3, ButtonMap::R3,
    ButtonMap::Select, ButtonMap::Start, ButtonMap::Home,
    ButtonMap::Misc1, ButtonMap::Nothing
};
const ButtonMap XboxValidOutputs[] = {
    ButtonMap::South, ButtonMap::East, ButtonMap::West, ButtonMap::North,
    ButtonMap::DpadLeft, ButtonMap::DpadRight, ButtonMap::DpadUp, ButtonMap::DpadDown,
    ButtonMap::L1, ButtonMap::R1, ButtonMap::L2, ButtonMap::R2,
    ButtonMap::L3, ButtonMap::R3,
    ButtonMap::Select, ButtonMap::Start, ButtonMap::Home,
    ButtonMap::Nothing
};

// The Flex can't tell us which mode it's in when its output goes to another machine.
enum OutputControllerType {
    OUTPUT_SWITCH = 0,
    OUTPUT_XBOX = 1
};
int output_controller_type = OUTPUT_SWITCH;

std::atomic<hid_device*> flex{ nullptr };
std::atomic<bool> flex_connecting{ false };
std::atomic<bool> flex_handshake_failed{ false };
std::atomic<bool> flex_just_connected{ false };
std::atomic<bool> flex_needs_check{ true };
std::thread connect_thread;

ControllerState last_state;
uint64_t last_send_time = 0;

void flex_cipher(uint8_t r, const uint8_t* in, uint8_t* out) {
    uint8_t key[16];
    key[0] = 0x54;
    key[1] = 0x21;
    for (int n = 2; n < 16; n++) {
        key[n] = key[n - 2] + key[n - 1] + n;
    }

    uint8_t s[16];
    for (int i = 0; i < 16; i++) {
        s[i] = (4 * i + r) & 0xFF;
        if (s[i] == 0) {
            s[i] = 0xF9;
        }
    }

    int j = 0;
    for (int i = 0; i < 16; i++) {
        j = (j + key[i] + s[i]) & 15;
        std::swap(s[i], s[j]);
    }

    // j is carried over from the key schedule
    int i = 0;
    for (int n = 0; n < 16; n++) {
        i = (i + 1) & 15;
        j = (j + s[i]) & 15;
        std::swap(s[i], s[j]);
        out[n] = in[n] ^ s[(s[i] + s[j]) & 15];
    }
}

// Sends a handshake command and waits for the matching response.
bool flex_command(hid_device* dev, uint8_t cmd, uint8_t r, const uint8_t* body, bool flag, uint8_t* resp) {
    uint8_t pkt[64] = { 0 };
    pkt[0] = 0x02;  // report ID
    pkt[1] = cmd;
    pkt[3] = r;
    if (body) {
        memcpy(&pkt[4], body, 16);
    }
    if (flag) {
        pkt[12] = 0x01;
    }
    memcpy(&pkt[60], FLEX_MAGIC, sizeof(FLEX_MAGIC));

    if (hid_write(dev, pkt, sizeof(pkt)) < 0) {
        return false;
    }

    Uint64 deadline = SDL_GetTicks() + FLEX_HANDSHAKE_TIMEOUT_MS;
    while (true) {
        Uint64 now = SDL_GetTicks();
        if (now >= deadline) {
            return false;
        }
        int res = hid_read_timeout(dev, resp, 64, (int) (deadline - now));
        if (res < 0) {
            return false;
        }
        if (res >= 20 && resp[0] == 0x01 && resp[1] == cmd) {
            return true;
        }
    }
}

// The device ignores input state until this succeeds, once after each power-up.
bool flex_handshake(hid_device* dev) {
    std::random_device rd;
    uint8_t resp[64];

    // info
    if (!flex_command(dev, 0x01, 0, nullptr, true, resp)) {
        return false;
    }

    // we challenge the device
    uint8_t r = rd() & 0xFF;
    uint8_t challenge[16];
    for (int i = 0; i < 16; i++) {
        challenge[i] = rd() & 0xFF;
    }
    if (!flex_command(dev, 0x02, r, challenge, false, resp)) {
        return false;
    }
    uint8_t expected[16];
    flex_cipher(r, challenge, expected);
    if (memcmp(&resp[4], expected, 16) != 0) {
        return false;
    }

    // the device challenges us
    if (!flex_command(dev, 0x03, 0, nullptr, false, resp)) {
        return false;
    }
    r = resp[3];
    uint8_t answer[16];
    flex_cipher(r, &resp[4], answer);
    if (!flex_command(dev, 0x03, r, answer, false, resp)) {
        return false;
    }
    return resp[2] == 0x01;
}

void try_open_flex() {
    if (flex.load() || flex_connecting.load()) {
        return;
    }

    if (connect_thread.joinable()) {
        connect_thread.join();
    }

    flex_connecting.store(true);
    connect_thread = std::thread([]() {
        hid_device* dev = nullptr;
        bool found = false;

        struct hid_device_info *devs, *cur_dev;
        devs = hid_enumerate(FLEX_VID, FLEX_PID);
        cur_dev = devs;
        while (cur_dev) {
            if (cur_dev->interface_number == FLEX_INTERFACE) {
                found = true;
                dev = hid_open_path(cur_dev->path);
                if (dev) {
                    break;
                }
            }
            cur_dev = cur_dev->next;
        }
        hid_free_enumeration(devs);

        if (dev) {
            if (flex_handshake(dev)) {
                flex_handshake_failed.store(false);
                flex_just_connected.store(true);
                flex.store(dev);
            } else {
                hid_close(dev);
                flex_handshake_failed.store(true);
            }
        } else if (!found) {
            flex_handshake_failed.store(false);
        }

        flex_connecting.store(false);
    });
}

void close_flex() {
    hid_device* dev = flex.exchange(nullptr);
    if (dev) {
        hid_close(dev);
    }
}

void send_flex_report(const ControllerState& state) {
    hid_device* dev = flex.load();
    if (!dev) {
        return;
    }

    uint8_t report[64] = { 0 };
    report[0] = 0x02;  // report ID
    report[1] = 0x00;  // set input state

    // Face buttons go by label, which is in a different place in each mode.
    bool switch_mode = (output_controller_type == OUTPUT_SWITCH);
    bool y = switch_mode ? state.west : state.north;
    bool b = switch_mode ? state.south : state.east;
    bool a = switch_mode ? state.east : state.south;
    bool x = switch_mode ? state.north : state.west;

    bool buttons[] = {
        y, b, a, x,
        state.l1, state.r1, state.l2 > 64, state.r2 > 64,
        state.select, state.start, state.l3, state.r3,
        state.home, switch_mode && state.misc1,
        state.dpad_up, state.dpad_down, state.dpad_left, state.dpad_right
    };
    for (int i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        report[4 + i] = buttons[i] ? FLEX_PRESSED : 0;
    }

    // 0..127, X: 0 is left, Y: 0 is down
    auto map_axis = [](int16_t val) { return (uint8_t) (((int32_t) val + 32768) * 127 / 65535); };
    report[22] = map_axis(state.left_stick_x);
    report[23] = 127 - map_axis(state.left_stick_y);
    report[24] = map_axis(state.right_stick_x);
    report[25] = 127 - map_axis(state.right_stick_y);

    if (hid_write(dev, report, sizeof(report)) < 0) {
        close_flex();
    }
}

const char* const BACKEND_APP_NAME = "any2flex";
const bool BACKEND_HAS_RUMBLE = false;
const bool BACKEND_DELAY_AFTER_SEND = true;

bool backend_init() {
    return hid_init() == 0;
}

void backend_shutdown() {
    if (connect_thread.joinable()) {
        connect_thread.join();
    }
    close_flex();
    hid_exit();
}

bool backend_load_setting(const std::string& key, const nlohmann::json& value) {
    if (key == "output_controller_type") {
        if (value.is_number_integer()) {
            int type = value.get<int>();
            if (type == OUTPUT_SWITCH || type == OUTPUT_XBOX) {
                output_controller_type = type;
            }
        }
        return true;
    }
    return false;
}

void backend_save_settings(nlohmann::json& j) {
    j["output_controller_type"] = output_controller_type;
}

const char* const* backend_button_map_names() {
    return output_controller_type == OUTPUT_SWITCH ? SwitchButtonMapNames : XboxButtonMapNames;
}

const ButtonMap* backend_valid_outputs(int* count) {
    if (output_controller_type == OUTPUT_SWITCH) {
        *count = sizeof(SwitchValidOutputs) / sizeof(ButtonMap);
        return SwitchValidOutputs;
    }
    *count = sizeof(XboxValidOutputs) / sizeof(ButtonMap);
    return XboxValidOutputs;
}

bool backend_ignore_gamepad(SDL_Gamepad* gamepad) {
    return false;
}

bool backend_default_enabled(SDL_Gamepad* gamepad, const char* name) {
    // A Flex's gamepad output could be the one we're sending to, which would
    // feed back into itself. The PC input port has no serial number, so we
    // can't tell whether it is, and the user has to enable it.
    if (!gamepad) {
        return true;
    }
    Uint16 pid = SDL_GetGamepadProduct(gamepad);
    return !(SDL_GetGamepadVendor(gamepad) == FLEX_VID && (pid == FLEX_PAD_PID || pid == FLEX_PAD_PC_PID));
}

void backend_on_device_change() {
    if (!flex.load()) {
        flex_needs_check.store(true);
    }
}

void backend_update() {
    static Uint64 last_flex_check = 0;
    Uint64 now = SDL_GetTicks();

    if (!flex.load() && (now - last_flex_check > 1000)) {
        flex_needs_check.store(true);
        last_flex_check = now;
    }

    if (flex_needs_check.exchange(false)) {
        try_open_flex();
    }

    // The device starts out neutral, so send what it has missed.
    if (flex_just_connected.exchange(false)) {
        send_flex_report(last_state);
        last_send_time = SDL_GetTicksNS();
    }
}

bool backend_is_connected() {
    return flex.load() != nullptr;
}

bool backend_send_state(const ControllerState& state) {
    uint64_t now = SDL_GetTicksNS();
    if (now - last_send_time < FLEX_MIN_SEND_INTERVAL_NS) {
        return false;
    }
    last_state = state;
    send_flex_report(state);
    last_send_time = now;
    return true;
}

void backend_draw_ui() {
    if (flex.load()) {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Flex: connected");
    } else if (flex_connecting.load()) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Flex: connecting...");
    } else if (flex_handshake_failed.load()) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Flex: handshake failed");
    } else {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Flex: not detected");
    }

    ImGui::SameLine();
    ImGui::Text("  Output:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* output_types[] = { "Switch", "Xbox 360" };
    if (ImGui::BeginCombo("##output_type", output_types[output_controller_type])) {
        for (int n = 0; n < 2; n++) {
            bool is_selected = (output_controller_type == n);
            if (ImGui::Selectable(output_types[n], is_selected)) {
                if (output_controller_type != n) {
                    output_controller_type = n;
                    int valid_outputs_count = 0;
                    const ButtonMap* valid_outputs = backend_valid_outputs(&valid_outputs_count);

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

                    // Face buttons move around, so resend.
                    last_send_time = 0;
                    send_flex_report(last_state);
                }
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}
