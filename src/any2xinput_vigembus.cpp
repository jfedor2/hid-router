// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "backend.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ViGEm/Client.h>

#include <atomic>
#include <cstdio>

#include "imgui.h"

// The virtual controller gets its own identity, so that we can recognize it
// among our inputs.
const unsigned short VIRTUAL_VID = 0xCAFE;
const unsigned short VIRTUAL_PID = 0xBAE2;

// The dongle takes reports at 1000 Hz and sending to it blocks, which limits
// how often any2xinput sends. We do the same here.
const Uint64 MIN_SEND_INTERVAL_NS = 1000000;

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

PVIGEM_CLIENT vigem = nullptr;
PVIGEM_TARGET target = nullptr;
// Result of the last attempt to connect to the bus and plug in the target.
VIGEM_ERROR vigem_error = VIGEM_ERROR_NONE;
bool vigem_connected = false;
Uint64 last_send_time = 0;

// Rumble arrives on a ViGEm thread and is applied on the main thread.
std::atomic<bool> rumble_pending{ false };
std::atomic<uint8_t> rumble_large_motor{ 0 };
std::atomic<uint8_t> rumble_small_motor{ 0 };

VOID CALLBACK x360_notification(PVIGEM_CLIENT client, PVIGEM_TARGET target, UCHAR large_motor, UCHAR small_motor, UCHAR led_number, LPVOID user_data) {
    rumble_large_motor.store(large_motor);
    rumble_small_motor.store(small_motor);
    rumble_pending.store(true);
}

void disconnect_vigem() {
    if (target) {
        vigem_target_x360_unregister_notification(target);
        if (vigem_connected) {
            vigem_target_remove(vigem, target);
        }
        vigem_target_free(target);
        target = nullptr;
    }
    if (vigem) {
        if (vigem_connected) {
            vigem_disconnect(vigem);
        }
        vigem_free(vigem);
        vigem = nullptr;
    }
    vigem_connected = false;
}

void try_connect_vigem() {
    if (vigem_connected) {
        return;
    }

    vigem = vigem_alloc();
    if (!vigem) {
        vigem_error = VIGEM_ERROR_WINAPI;
        return;
    }
    vigem_error = vigem_connect(vigem);
    if (!VIGEM_SUCCESS(vigem_error)) {
        vigem_free(vigem);
        vigem = nullptr;
        return;
    }
    vigem_connected = true;

    target = vigem_target_x360_alloc();
    vigem_target_set_vid(target, VIRTUAL_VID);
    vigem_target_set_pid(target, VIRTUAL_PID);
    vigem_error = vigem_target_add(vigem, target);
    if (!VIGEM_SUCCESS(vigem_error)) {
        vigem_target_free(target);
        target = nullptr;
        disconnect_vigem();
        return;
    }
    vigem_target_x360_register_notification(vigem, target, x360_notification, nullptr);
}

void process_rumble() {
    if (!rumble_pending.exchange(false)) {
        return;
    }

    uint8_t left_motor = rumble_large_motor.load();
    uint8_t right_motor = rumble_small_motor.load();

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

void send_xusb_report(const ControllerState& state) {
    if (!target) {
        return;
    }

    XUSB_REPORT report = { 0 };

    if (state.dpad_up) {
        report.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    }
    if (state.dpad_down) {
        report.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
    }
    if (state.dpad_left) {
        report.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    }
    if (state.dpad_right) {
        report.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;
    }
    if (state.start) {
        report.wButtons |= XUSB_GAMEPAD_START;
    }
    if (state.select) {
        report.wButtons |= XUSB_GAMEPAD_BACK;
    }
    if (state.l3) {
        report.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    }
    if (state.r3) {
        report.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;
    }
    if (state.l1) {
        report.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    }
    if (state.r1) {
        report.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    }
    if (state.home) {
        report.wButtons |= XUSB_GAMEPAD_GUIDE;
    }
    if (state.south) {
        report.wButtons |= XUSB_GAMEPAD_A;
    }
    if (state.east) {
        report.wButtons |= XUSB_GAMEPAD_B;
    }
    if (state.west) {
        report.wButtons |= XUSB_GAMEPAD_X;
    }
    if (state.north) {
        report.wButtons |= XUSB_GAMEPAD_Y;
    }

    report.bLeftTrigger = state.l2;
    report.bRightTrigger = state.r2;

    auto invert = [](int16_t val) {
        int32_t inv = -(int32_t) val;
        return (SHORT) (inv > 32767 ? 32767 : inv);
    };

    report.sThumbLX = state.left_stick_x;
    report.sThumbLY = invert(state.left_stick_y);
    report.sThumbRX = state.right_stick_x;
    report.sThumbRY = invert(state.right_stick_y);

    VIGEM_ERROR res = vigem_target_x360_update(vigem, target, report);
    if (!VIGEM_SUCCESS(res)) {
        vigem_error = res;
        disconnect_vigem();
    }
}

const char* const BACKEND_APP_NAME = "any2xinput-vigembus";
const bool BACKEND_HAS_RUMBLE = true;
const bool BACKEND_DELAY_AFTER_SEND = false;

bool backend_init() {
    return true;
}

void backend_shutdown() {
    disconnect_vigem();
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
    // Our own virtual controller would feed back into itself.
    return SDL_GetGamepadVendor(gamepad) == VIRTUAL_VID && SDL_GetGamepadProduct(gamepad) == VIRTUAL_PID;
}

bool backend_default_enabled(SDL_Gamepad* gamepad, const char* name) {
    return true;
}

void backend_on_device_change() {
}

void backend_update() {
    static Uint64 last_vigem_check = 0;
    Uint64 now = SDL_GetTicks();

    if (!vigem_connected && (last_vigem_check == 0 || now - last_vigem_check > 1000)) {
        last_vigem_check = now ? now : 1;
        try_connect_vigem();
    }

    process_rumble();
}

bool backend_is_connected() {
    return target != nullptr;
}

bool backend_send_state(const ControllerState& state) {
    Uint64 since_last_send = SDL_GetTicksNS() - last_send_time;
    if (target && since_last_send < MIN_SEND_INTERVAL_NS) {
        SDL_DelayPrecise(MIN_SEND_INTERVAL_NS - since_last_send);
    }
    send_xusb_report(state);
    last_send_time = SDL_GetTicksNS();
    return true;
}

void backend_draw_ui() {
    if (target) {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "ViGEmBus: connected");
    } else if (vigem_error == VIGEM_ERROR_BUS_NOT_FOUND) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "ViGEmBus driver not installed.");
    } else {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "ViGEmBus: error 0x%08X", (unsigned int) vigem_error);
    }

    ImGui::Checkbox("Rumble", &global_rumble_enabled);
}
