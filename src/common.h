// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <string>

struct ControllerState {
    int16_t left_stick_x = 0;
    int16_t left_stick_y = 0;
    int16_t right_stick_x = 0;
    int16_t right_stick_y = 0;
    uint8_t l2 = 0;
    uint8_t r2 = 0;

    bool south = false;
    bool east = false;
    bool west = false;
    bool north = false;
    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;
    bool l1 = false;
    bool r1 = false;
    bool l3 = false;
    bool r3 = false;
    bool select = false;
    bool start = false;
    bool home = false;
    bool misc1 = false;
    bool touchpad = false;
    bool misc2 = false;
    bool r4 = false;
    bool l4 = false;
    bool r5 = false;
    bool l5 = false;

    bool operator!=(const ControllerState& other) const {
        return left_stick_x != other.left_stick_x ||
               left_stick_y != other.left_stick_y ||
               right_stick_x != other.right_stick_x ||
               right_stick_y != other.right_stick_y ||
               l2 != other.l2 ||
               r2 != other.r2 ||
               dpad_up != other.dpad_up ||
               dpad_down != other.dpad_down ||
               dpad_left != other.dpad_left ||
               dpad_right != other.dpad_right ||
               start != other.start ||
               select != other.select ||
               l3 != other.l3 ||
               r3 != other.r3 ||
               l1 != other.l1 ||
               r1 != other.r1 ||
               south != other.south ||
               east != other.east ||
               west != other.west ||
               north != other.north ||
               home != other.home ||
               misc1 != other.misc1 ||
               touchpad != other.touchpad ||
               r4 != other.r4 ||
               l4 != other.l4 ||
               r5 != other.r5 ||
               l5 != other.l5 ||
               misc2 != other.misc2;
    }
};

enum class ButtonMap {
    South,
    East,
    West,
    North,
    Home,
    Select,
    Start,
    L3,
    R3,
    L1,
    R1,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Misc1,
    Touchpad,
    R4,
    L4,
    R5,
    L5,
    Misc2,
    Nothing,
    L2,
    R2,
    Count
};

enum class StickMap {
    LeftStickX,
    LeftStickY,
    RightStickX,
    RightStickY,
    Nothing,
    Count
};

enum class TriggerMap {
    L2,
    R2,
    Nothing,
    Count
};

constexpr int NUM_BUTTONS = static_cast<int>(ButtonMap::Nothing);
constexpr int NUM_TRIGGERS = static_cast<int>(TriggerMap::Nothing);
constexpr int NUM_STICK_AXES = static_cast<int>(StickMap::Nothing);

constexpr float TRIGGER_MAX = 255.0f;
constexpr float STICK_DEADZONE = 8000.0f;
constexpr float STICK_MAX = 32767.0f;

struct ControllerSettings {
    bool enabled = true;
    bool rumble = true;
    ButtonMap btn_map[NUM_BUTTONS];
    ButtonMap trigger_map[NUM_TRIGGERS];
    StickMap stick_map[NUM_STICK_AXES];

    ControllerSettings() {
        for (int i = 0; i < NUM_BUTTONS; ++i) {
            btn_map[i] = static_cast<ButtonMap>(i);
        }
        for (int i = 0; i < 2; ++i) {
            trigger_map[i] = (i == 0) ? ButtonMap::L2 : ButtonMap::R2;
        }
        for (int i = 0; i < 4; ++i) {
            stick_map[i] = static_cast<StickMap>(i);
        }
    }
};

struct ControllerInfo {
    SDL_Gamepad* gamepad;
    std::string name;
    std::string key;
    bool enabled;
    bool rumble;
    bool ui_expanded = false;
    ControllerState current_state;

    ButtonMap btn_map[NUM_BUTTONS];
    ButtonMap trigger_map[NUM_TRIGGERS];
    StickMap stick_map[NUM_STICK_AXES];

    bool raw_btns[NUM_BUTTONS] = { 0 };
    uint8_t raw_trigs[NUM_TRIGGERS] = { 0 };
    int16_t raw_sticks[NUM_STICK_AXES] = { 0 };

    // Only used if there is no SDL gamepad.
    bool net_btns_present[NUM_BUTTONS] = { 0 };
};

extern std::map<SDL_JoystickID, ControllerInfo> controllers;
extern std::map<std::string, ControllerSettings> controller_settings;

// Controllers that do not come from SDL use IDs starting here.
constexpr SDL_JoystickID NETWORK_CONTROLLER_ID_BASE = 0x80000000;

// gc can be null for controllers that do not come from SDL.
void AddController(SDL_JoystickID jid, SDL_Gamepad* gc, const char* name, const std::string& key);
