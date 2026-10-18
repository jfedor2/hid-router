// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "backend.h"

#include <hidapi.h>

#include <atomic>
#include <cstring>

#include "imgui.h"

const unsigned short DONGLE_VID = 0xCAFE;
const unsigned short DONGLE_PID = 0xBAFD;
const int DONGLE_INTERFACE = 1;

const char* const ButtonMapNames[] = {
    "A", "B", "X", "Y", "Guide", "Back", "Start", "LSB", "RSB", "LB", "RB", "Up", "Down", "Left", "Right",
    "N/A", "N/A", "N/A", "N/A", "N/A", "N/A", "N/A",
    "Nothing",
    "LT", "RT"
};

const ButtonMap XinputValidOutputs[] = {
    ButtonMap::South, ButtonMap::East, ButtonMap::West, ButtonMap::North,
    ButtonMap::DpadLeft, ButtonMap::DpadRight, ButtonMap::DpadUp, ButtonMap::DpadDown,
    ButtonMap::L1, ButtonMap::R1, ButtonMap::L2, ButtonMap::R2,
    ButtonMap::L3, ButtonMap::R3,
    ButtonMap::Select, ButtonMap::Start, ButtonMap::Home,
    ButtonMap::Nothing
};
constexpr int NUM_VALID_OUTPUTS = sizeof(XinputValidOutputs) / sizeof(ButtonMap);

bool global_rumble_enabled = true;
hid_device* dongle = nullptr;
std::atomic<bool> dongle_needs_check{ true };

void try_open_dongle() {
    if (dongle) {
        return;
    }

    struct hid_device_info *devs, *cur_dev;
    devs = hid_enumerate(DONGLE_VID, DONGLE_PID);
    cur_dev = devs;

    while (cur_dev) {
        if (cur_dev->interface_number == DONGLE_INTERFACE) {
            dongle = hid_open_path(cur_dev->path);
            if (dongle) {
                hid_set_nonblocking(dongle, 1);
                break;
            }
        }
        cur_dev = cur_dev->next;
    }
    hid_free_enumeration(devs);
}

void process_dongle_input() {
    if (!dongle) {
        return;
    }
    unsigned char buf[64];
    int res;
    while ((res = hid_read(dongle, buf, sizeof(buf))) > 0) {
        if (res == 8) {
            if (buf[0] == 0x00 && buf[1] == 0x08) {
                uint8_t left_motor = buf[3];
                uint8_t right_motor = buf[4];

                uint16_t low_freq = (left_motor << 8) | left_motor;
                uint16_t high_freq = (right_motor << 8) | right_motor;

                for (auto& pair : controllers) {
                    if (pair.second.gamepad) {
                        if (global_rumble_enabled && pair.second.enabled && pair.second.rumble) {
                            SDL_RumbleGamepad(pair.second.gamepad, low_freq, high_freq, 0xFFFF);
                        } else {
                            SDL_RumbleGamepad(pair.second.gamepad, 0, 0, 0);
                        }
                    }
                }
            }
        }
    }
    if (res < 0) {
        hid_close(dongle);
        dongle = nullptr;
    }
}

void send_xusb_report(const ControllerState& state) {
    if (!dongle) {
        return;
    }

    unsigned char report[20] = { 0 };
    report[0] = 0x00;  // message type
    report[1] = 0x14;  // size

    if (state.dpad_up) {
        report[2] |= 0x01;
    }
    if (state.dpad_down) {
        report[2] |= 0x02;
    }
    if (state.dpad_left) {
        report[2] |= 0x04;
    }
    if (state.dpad_right) {
        report[2] |= 0x08;
    }
    if (state.start) {
        report[2] |= 0x10;
    }
    if (state.select) {
        report[2] |= 0x20;
    }
    if (state.l3) {
        report[2] |= 0x40;
    }
    if (state.r3) {
        report[2] |= 0x80;
    }

    if (state.l1) {
        report[3] |= 0x01;
    }
    if (state.r1) {
        report[3] |= 0x02;
    }
    if (state.home) {
        report[3] |= 0x04;
    }
    if (state.south) {
        report[3] |= 0x10;
    }
    if (state.east) {
        report[3] |= 0x20;
    }
    if (state.west) {
        report[3] |= 0x40;
    }
    if (state.north) {
        report[3] |= 0x80;
    }

    report[4] = state.l2;
    report[5] = state.r2;

    auto set_le16 = [](unsigned char* ptr, int16_t val) {
        ptr[0] = val & 0xFF;
        ptr[1] = (val >> 8) & 0xFF;
    };

    set_le16(&report[6], state.left_stick_x);

    int32_t lsy_inv = -(int32_t) state.left_stick_y;
    if (lsy_inv > 32767) {
        lsy_inv = 32767;
    } else if (lsy_inv < -32768) {
        lsy_inv = -32768;
    }
    set_le16(&report[8], (int16_t) lsy_inv);

    set_le16(&report[10], state.right_stick_x);

    int32_t rsy_inv = -(int32_t) state.right_stick_y;
    if (rsy_inv > 32767) {
        rsy_inv = 32767;
    } else if (rsy_inv < -32768) {
        rsy_inv = -32768;
    }
    set_le16(&report[12], (int16_t) rsy_inv);

    unsigned char write_buf[21] = { 0 };
    write_buf[0] = 0x00;  // Report ID
    std::memcpy(&write_buf[1], report, sizeof(report));

    int res = hid_write(dongle, write_buf, sizeof(write_buf));
    if (res < 0) {
        hid_close(dongle);
        dongle = nullptr;
    }
}

const char* const BACKEND_APP_NAME = "any2xinput-vgd";
const bool BACKEND_HAS_RUMBLE = true;
const bool BACKEND_DELAY_AFTER_SEND = false;

bool backend_init() {
    return hid_init() == 0;
}

void backend_shutdown() {
    if (dongle) {
        hid_close(dongle);
    }
    hid_exit();
}

bool backend_load_setting(const std::string& key, const nlohmann::json& value) {
    if (key == "global_rumble_enabled") {
        if (value.is_boolean()) {
            global_rumble_enabled = value.get<bool>();
        }
        return true;
    }
    return false;
}

void backend_save_settings(nlohmann::json& j) {
    j["global_rumble_enabled"] = global_rumble_enabled;
}

const char* const* backend_button_map_names() {
    return ButtonMapNames;
}

const ButtonMap* backend_valid_outputs(int* count) {
    *count = NUM_VALID_OUTPUTS;
    return XinputValidOutputs;
}

bool backend_ignore_gamepad(SDL_Gamepad* gamepad) {
    return SDL_GetGamepadVendor(gamepad) == DONGLE_VID && SDL_GetGamepadProduct(gamepad) == DONGLE_PID;
}

bool backend_default_enabled(SDL_Gamepad* gamepad, const char* name) {
    return true;
}

void backend_on_device_change() {
    if (!dongle) {
        dongle_needs_check.store(true);
    }
}

void backend_update() {
    static Uint64 last_dongle_check = 0;
    Uint64 now = SDL_GetTicks();

    if (!dongle && (now - last_dongle_check > 1000)) {
        dongle_needs_check.store(true);
        last_dongle_check = now;
    }

    if (dongle_needs_check.exchange(false)) {
        try_open_dongle();
    }

    process_dongle_input();
}

bool backend_is_connected() {
    return dongle != nullptr;
}

bool backend_send_state(const ControllerState& state) {
    send_xusb_report(state);
    return true;
}

void backend_draw_ui() {
    if (dongle) {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Dongle: connected");
    } else {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Dongle: not detected");
    }

    ImGui::Checkbox("Rumble", &global_rumble_enabled);
}

