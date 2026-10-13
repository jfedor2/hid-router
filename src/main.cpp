// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbt.h>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backend.h"
#include "common.h"
#include "hid_receiver.h"

const char* const InputButtonMapNames[] = {
    "South", "East", "West", "North", "Home", "Select", "Start", "L3", "R3", "L1", "R1", "Up", "Down", "Left", "Right",
    "Misc 1", "Touchpad", "R4", "L4", "R5", "L5", "Misc 2",
    "Nothing"
};

const char* GetInputButtonLabel(SDL_Gamepad* gamepad, int idx) {
    if (idx >= 0 && idx <= 3 && gamepad != nullptr) {
        SDL_GamepadButton sdl_btn = static_cast<SDL_GamepadButton>(idx);
        SDL_GamepadButtonLabel label = SDL_GetGamepadButtonLabel(gamepad, sdl_btn);
        switch (label) {
            case SDL_GAMEPAD_BUTTON_LABEL_A:
                return "A";
            case SDL_GAMEPAD_BUTTON_LABEL_B:
                return "B";
            case SDL_GAMEPAD_BUTTON_LABEL_X:
                return "X";
            case SDL_GAMEPAD_BUTTON_LABEL_Y:
                return "Y";
            case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
                return "Cross";
            case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
                return "Circle";
            case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
                return "Square";
            case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
                return "Triangle";
            default:
                break;
        }
    }
    return InputButtonMapNames[idx];
}

const char* const InputTriggerMapNames[] = {
    "L2", "R2", "Nothing"
};

const char* StickMapNames[] = {
    "Left X", "Left Y", "Right X", "Right Y", "Nothing"
};

std::atomic<bool> is_running{ true };
std::map<SDL_JoystickID, ControllerInfo> controllers;
std::map<std::string, ControllerSettings> controller_settings;
ControllerState last_sent_state;

std::mutex imgui_mutex;
std::atomic<bool> ui_frame_requested{ true };

std::string GetControllerKey(SDL_Gamepad* gc) {
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(gc);
    const char* serial = SDL_GetJoystickSerial(joystick);
    if (serial && strlen(serial) > 0) {
        Uint16 vid = SDL_GetJoystickVendor(joystick);
        Uint16 pid = SDL_GetJoystickProduct(joystick);
        char buf[128];
        snprintf(buf, sizeof(buf), "%04x_%04x_%s", vid, pid, serial);
        return std::string(buf);
    } else {
        SDL_GUID guid = SDL_GetJoystickGUID(joystick);
        char buf[64];
        SDL_GUIDToString(guid, buf, sizeof(buf));
        return std::string(buf);
    }
}

std::string GetSettingsFilePath() {
    char* prefPath = SDL_GetPrefPath(BACKEND_APP_NAME, BACKEND_APP_NAME);
    if (!prefPath) {
        return "";
    }
    std::string path = std::string(prefPath) + "settings.json";
    SDL_free(prefPath);
    return path;
}

void LoadSettings() {
    std::string path = GetSettingsFilePath();
    if (path.empty()) {
        return;
    }

    try {
        std::ifstream file(path);
        if (file.is_open()) {
            nlohmann::json j;
            file >> j;
            for (auto& [key, value] : j.items()) {
                if (backend_load_setting(key, value) || hid_receiver_load_setting(key, value)) {
                    continue;
                }
                if (value.is_object() && value.contains("enabled") && value["enabled"].is_boolean()) {
                    controller_settings[key].enabled = value["enabled"].get<bool>();
                }
                if (BACKEND_HAS_RUMBLE && value.is_object() && value.contains("rumble") && value["rumble"].is_boolean()) {
                    controller_settings[key].rumble = value["rumble"].get<bool>();
                }
                if (value.is_object() && value.contains("btn_map") && value["btn_map"].is_array()) {
                    for (int i = 0; i < NUM_BUTTONS && i < value["btn_map"].size(); ++i) {
                        controller_settings[key].btn_map[i] = static_cast<ButtonMap>(value["btn_map"][i].get<int>());
                    }
                }
                if (value.is_object() && value.contains("trigger_btn_map") && value["trigger_btn_map"].is_array()) {
                    for (int i = 0; i < 2 && i < value["trigger_btn_map"].size(); ++i) {
                        controller_settings[key].trigger_map[i] = static_cast<ButtonMap>(value["trigger_btn_map"][i].get<int>());
                    }
                }
                if (value.is_object() && value.contains("stick_map") && value["stick_map"].is_array()) {
                    for (int i = 0; i < 4 && i < value["stick_map"].size(); ++i) {
                        controller_settings[key].stick_map[i] = static_cast<StickMap>(value["stick_map"][i].get<int>());
                    }
                }
            }
        }
    } catch (...) {
        // Ignore I/O and JSON parsing errors.
    }
}

void SaveSettings() {
    std::string path = GetSettingsFilePath();
    if (path.empty()) {
        return;
    }

    try {
        nlohmann::json j = nlohmann::json::object();
        backend_save_settings(j);
        hid_receiver_save_settings(j);
        for (auto& pair : controller_settings) {
            j[pair.first] = { { "enabled", pair.second.enabled } };
            if (BACKEND_HAS_RUMBLE) {
                j[pair.first]["rumble"] = pair.second.rumble;
            }
            for (int i = 0; i < NUM_BUTTONS; ++i) {
                j[pair.first]["btn_map"].push_back(static_cast<int>(pair.second.btn_map[i]));
            }
            for (int i = 0; i < 2; ++i) {
                j[pair.first]["trigger_btn_map"].push_back(static_cast<int>(pair.second.trigger_map[i]));
            }
            for (int i = 0; i < 4; ++i) {
                j[pair.first]["stick_map"].push_back(static_cast<int>(pair.second.stick_map[i]));
            }
        }

        std::ofstream file(path);
        if (file.is_open()) {
            file << j.dump(4);
        }
    } catch (...) {
        // Ignore I/O and JSON serialization errors.
    }
}

void AddController(SDL_JoystickID jid, SDL_Gamepad* gc, const char* name, const std::string& key) {
    ControllerInfo new_info;
    new_info.gamepad = gc;
    new_info.name = name ? name : "Unknown Controller";
    new_info.key = key;
    new_info.enabled = true;
    new_info.rumble = true;
    new_info.ui_expanded = false;
    new_info.current_state = ControllerState();
    if (controller_settings.count(key)) {
        new_info.enabled = controller_settings[key].enabled;
        new_info.rumble = controller_settings[key].rumble;
        for (int i = 0; i < NUM_BUTTONS; ++i) {
            new_info.btn_map[i] = controller_settings[key].btn_map[i];
        }
        for (int i = 0; i < 2; ++i) {
            new_info.trigger_map[i] = controller_settings[key].trigger_map[i];
        }
        for (int i = 0; i < 4; ++i) {
            new_info.stick_map[i] = controller_settings[key].stick_map[i];
        }
    } else {
        bool default_enabled = backend_default_enabled(name);
        controller_settings[key].enabled = default_enabled;
        controller_settings[key].rumble = true;
        new_info.enabled = default_enabled;
        new_info.rumble = true;
        for (int i = 0; i < NUM_BUTTONS; ++i) {
            new_info.btn_map[i] = static_cast<ButtonMap>(i);
            controller_settings[key].btn_map[i] = static_cast<ButtonMap>(i);
        }
        for (int i = 0; i < 2; ++i) {
            new_info.trigger_map[i] = (i == 0) ? ButtonMap::L2 : ButtonMap::R2;
            controller_settings[key].trigger_map[i] = (i == 0) ? ButtonMap::L2 : ButtonMap::R2;
        }
        for (int i = 0; i < 4; ++i) {
            new_info.stick_map[i] = static_cast<StickMap>(i);
            controller_settings[key].stick_map[i] = static_cast<StickMap>(i);
        }
        int valid_outputs_count = 0;
        const ButtonMap* valid_outputs = backend_valid_outputs(&valid_outputs_count);
        for (int i = 0; i < NUM_BUTTONS; ++i) {
            bool is_valid = false;
            for (int k = 0; k < valid_outputs_count; ++k) {
                if (new_info.btn_map[i] == valid_outputs[k]) {
                    is_valid = true;
                    break;
                }
            }
            if (!is_valid) {
                new_info.btn_map[i] = ButtonMap::Nothing;
                controller_settings[key].btn_map[i] = ButtonMap::Nothing;
            }
        }
    }
    controllers[jid] = new_info;
}

WNDPROC OriginalWndProc = nullptr;

LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DEVICECHANGE) {
        backend_on_device_change();
    }
    return CallWindowProc(OriginalWndProc, hwnd, msg, wParam, lParam);
}

int16_t clamp_axis(int32_t val) {
    if (val < -32768) {
        return -32768;
    }
    if (val > 32767) {
        return 32767;
    }
    return (int16_t) val;
}

void read_gamepad(ControllerInfo& info) {
    SDL_Gamepad* gc = info.gamepad;

    bool physical_btns[NUM_BUTTONS] = {
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_SOUTH),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_EAST),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_WEST),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_NORTH),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_GUIDE),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_BACK),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_START),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_LEFT_STICK),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_RIGHT_STICK),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_UP),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_DOWN),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_LEFT),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_MISC1),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_TOUCHPAD),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2),
        SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_MISC2)
    };

    uint8_t physical_trigs[2] = {
        (uint8_t) ((SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255) / 32767),
        (uint8_t) ((SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255) / 32767)
    };

    int16_t physical_sticks[4] = {
        SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_LEFTX),
        SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_LEFTY),
        SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_RIGHTX),
        SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_RIGHTY)
    };

    for (int i = 0; i < NUM_BUTTONS; ++i) {
        info.raw_btns[i] = physical_btns[i];
    }
    for (int i = 0; i < 2; ++i) {
        info.raw_trigs[i] = physical_trigs[i];
    }
    for (int i = 0; i < 4; ++i) {
        info.raw_sticks[i] = physical_sticks[i];
    }
}

bool poll_and_fuse() {
    ControllerState total_state;
    int32_t lsx = 0, lsy = 0, rsx = 0, rsy = 0;

    for (auto& pair : controllers) {
        ControllerInfo& info = pair.second;
        SDL_Gamepad* gc = info.gamepad;
        ControllerState c_state;

        // Controllers without an SDL gamepad have their raw state updated elsewhere.
        if (gc) {
            read_gamepad(info);
        }
        const bool* physical_btns = info.raw_btns;
        const uint8_t* physical_trigs = info.raw_trigs;
        const int16_t* physical_sticks = info.raw_sticks;

        auto apply_button_mapping = [&](ButtonMap mapped) {
            switch (mapped) {
                case ButtonMap::South:
                    c_state.south = true;
                    break;
                case ButtonMap::East:
                    c_state.east = true;
                    break;
                case ButtonMap::West:
                    c_state.west = true;
                    break;
                case ButtonMap::North:
                    c_state.north = true;
                    break;
                case ButtonMap::Home:
                    c_state.home = true;
                    break;
                case ButtonMap::Select:
                    c_state.select = true;
                    break;
                case ButtonMap::Start:
                    c_state.start = true;
                    break;
                case ButtonMap::L3:
                    c_state.l3 = true;
                    break;
                case ButtonMap::R3:
                    c_state.r3 = true;
                    break;
                case ButtonMap::L1:
                    c_state.l1 = true;
                    break;
                case ButtonMap::R1:
                    c_state.r1 = true;
                    break;
                case ButtonMap::DpadUp:
                    c_state.dpad_up = true;
                    break;
                case ButtonMap::DpadDown:
                    c_state.dpad_down = true;
                    break;
                case ButtonMap::DpadLeft:
                    c_state.dpad_left = true;
                    break;
                case ButtonMap::DpadRight:
                    c_state.dpad_right = true;
                    break;
                case ButtonMap::Misc1:
                    c_state.misc1 = true;
                    break;
                case ButtonMap::Touchpad:
                    c_state.touchpad = true;
                    break;
                case ButtonMap::R4:
                    c_state.r4 = true;
                    break;
                case ButtonMap::L4:
                    c_state.l4 = true;
                    break;
                case ButtonMap::R5:
                    c_state.r5 = true;
                    break;
                case ButtonMap::L5:
                    c_state.l5 = true;
                    break;
                case ButtonMap::Misc2:
                    c_state.misc2 = true;
                    break;
                case ButtonMap::L2:
                    c_state.l2 = 255;
                    break;
                case ButtonMap::R2:
                    c_state.r2 = 255;
                    break;
                case ButtonMap::Nothing:
                case ButtonMap::Count:
                    break;
            }
        };

        for (int i = 0; i < NUM_BUTTONS; ++i) {
            if (physical_btns[i]) {
                apply_button_mapping(info.btn_map[i]);
            }
        }

        for (int i = 0; i < 2; ++i) {
            ButtonMap mapped = info.trigger_map[i];
            if (mapped == ButtonMap::L2) {
                c_state.l2 = (std::max)(c_state.l2, physical_trigs[i]);
            } else if (mapped == ButtonMap::R2) {
                c_state.r2 = (std::max)(c_state.r2, physical_trigs[i]);
            } else if (physical_trigs[i] > 64) {
                apply_button_mapping(mapped);
            }
        }

        int32_t temp_lsx = 0, temp_lsy = 0, temp_rsx = 0, temp_rsy = 0;

        for (int i = 0; i < 4; ++i) {
            switch (info.stick_map[i]) {
                case StickMap::LeftStickX:
                    temp_lsx += physical_sticks[i];
                    break;
                case StickMap::LeftStickY:
                    temp_lsy += physical_sticks[i];
                    break;
                case StickMap::RightStickX:
                    temp_rsx += physical_sticks[i];
                    break;
                case StickMap::RightStickY:
                    temp_rsy += physical_sticks[i];
                    break;
                case StickMap::Nothing:
                case StickMap::Count:
                    break;
            }
        }

        c_state.left_stick_x = clamp_axis(temp_lsx);
        c_state.left_stick_y = clamp_axis(temp_lsy);
        c_state.right_stick_x = clamp_axis(temp_rsx);
        c_state.right_stick_y = clamp_axis(temp_rsy);

        info.current_state = c_state;

        if (!info.enabled) {
            continue;
        }

        total_state.south |= c_state.south;
        total_state.east |= c_state.east;
        total_state.west |= c_state.west;
        total_state.north |= c_state.north;
        total_state.home |= c_state.home;
        total_state.select |= c_state.select;
        total_state.start |= c_state.start;
        total_state.l3 |= c_state.l3;
        total_state.r3 |= c_state.r3;
        total_state.l1 |= c_state.l1;
        total_state.r1 |= c_state.r1;
        total_state.dpad_up |= c_state.dpad_up;
        total_state.dpad_down |= c_state.dpad_down;
        total_state.dpad_left |= c_state.dpad_left;
        total_state.dpad_right |= c_state.dpad_right;
        total_state.misc1 |= c_state.misc1;
        total_state.touchpad |= c_state.touchpad;
        total_state.r4 |= c_state.r4;
        total_state.l4 |= c_state.l4;
        total_state.r5 |= c_state.r5;
        total_state.l5 |= c_state.l5;
        total_state.misc2 |= c_state.misc2;

        if (c_state.l2 > total_state.l2) {
            total_state.l2 = c_state.l2;
        }
        if (c_state.r2 > total_state.r2) {
            total_state.r2 = c_state.r2;
        }

        lsx += c_state.left_stick_x;
        lsy += c_state.left_stick_y;
        rsx += c_state.right_stick_x;
        rsy += c_state.right_stick_y;
    }

    total_state.left_stick_x = clamp_axis(lsx);
    total_state.left_stick_y = clamp_axis(lsy);
    total_state.right_stick_x = clamp_axis(rsx);
    total_state.right_stick_y = clamp_axis(rsy);

    if (total_state != last_sent_state) {
        if (backend_send_state(total_state)) {
            last_sent_state = total_state;
            return true;
        }
    }
    return false;
}

void RenderThread(SDL_GPUDevice* device, SDL_Window* window, ImGuiContext* ctx) {
    ImGui::SetCurrentContext(ctx);
    while (is_running) {
        SDL_WindowFlags window_flags = SDL_GetWindowFlags(window);
        if (window_flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) {
            SDL_Delay(16);
            continue;
        }

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUTexture* swapchain_tex;

        // block on V-sync
        if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain_tex, NULL, NULL)) {
            std::lock_guard<std::mutex> lock(imgui_mutex);
            ImDrawData* draw_data = ImGui::GetDrawData();

            if (draw_data) {
                ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);
            }

            SDL_GPUColorTargetInfo color_target = {};
            color_target.texture = swapchain_tex;
            color_target.clear_color = { 0.117f, 0.117f, 0.117f, 1.0f };
            color_target.load_op = SDL_GPU_LOADOP_CLEAR;
            color_target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);

            if (draw_data) {
                ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
            }

            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
        ui_frame_requested.store(true);
    }
}

const char* icon_data[32] = {
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "     @@@@@            @@@@@     ",
    "    @@@@@@@@@@@@@@@@@@@@@@@@    ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@  @@@@@@@@  @@@@@@@   ",
    "   @@@@@@ @@ @@@@@@ @@ @@@@@@   ",
    "   @@@@@ @@@@ @@@@ @@@@ @@@@@   ",
    "   @@@@@@ @@ @@@@@@ @@ @@@@@@   ",
    "   @@@@@@@  @@@@@@@@  @@@@@@@   ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@@@@@@@@@@@@@@@@@@@@   ",
    "   @@@@@@@@          @@@@@@@@   ",
    "   @@@@@@@            @@@@@@@   ",
    "   @@@@@@@            @@@@@@@   ",
    "   @@@@@@@            @@@@@@@   ",
    "   @@@@@@              @@@@@@   ",
    "    @@@@                @@@@    ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                ",
    "                                "
};

SDL_Surface* CreateIconSurface(bool connected) {
    SDL_Surface* surface = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        return nullptr;
    }

    Uint32* pixels = (Uint32*) surface->pixels;
    Uint32 fill_color = SDL_MapRGBA(SDL_GetPixelFormatDetails(surface->format), NULL,
        connected ? 50 : 255,  // R
        connected ? 200 : 50,  // G
        50,                    // B
        255);                  // A
    Uint32 trans_color = SDL_MapRGBA(SDL_GetPixelFormatDetails(surface->format), NULL, 0, 0, 0, 0);

    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            if (icon_data[y][x] == '@') {
                pixels[y * 32 + x] = fill_color;
            } else {
                pixels[y * 32 + x] = trans_color;
            }
        }
    }
    return surface;
}

int main(int argc, char* argv[]) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_VIDEO)) {
        return 1;
    }

    if (!backend_init()) {
        SDL_Quit();
        return 1;
    }

    LoadSettings();

    SDL_Window* window = SDL_CreateWindow(BACKEND_APP_NAME, 800, 600, SDL_WINDOW_RESIZABLE);
    if (!window) {
        return 1;
    }

    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        true, NULL);

    SDL_ClaimWindowForGPUDevice(device, window);

    SDL_SetGPUSwapchainParameters(device, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        SDL_GPU_PRESENTMODE_VSYNC);

    IMGUI_CHECKVERSION();
    ImGuiContext* imgui_ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // Disable imgui.ini creation
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLGPU(window);

    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init_info);

    HWND hwnd = (HWND) SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (hwnd) {
        OriginalWndProc = (WNDPROC) SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR) CustomWndProc);
    }

    SDL_Surface* icon_red = CreateIconSurface(false);
    SDL_Surface* icon_green = CreateIconSurface(true);

    SDL_SetWindowIcon(window, icon_red);
    bool current_icon_connected = false;

    std::thread render_thread(RenderThread, device, window, imgui_ctx);

    SDL_Event event;

    while (is_running) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                is_running = false;
            } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                SDL_JoystickID jid = event.gdevice.which;
                SDL_Gamepad* gc = SDL_OpenGamepad(jid);
                if (gc) {
                    if (backend_ignore_gamepad(gc)) {
                        SDL_CloseGamepad(gc);
                        continue;
                    }
                    AddController(jid, gc, SDL_GetGamepadName(gc), GetControllerKey(gc));
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                SDL_JoystickID jid = event.gdevice.which;
                if (controllers.count(jid)) {
                    SDL_CloseGamepad(controllers[jid].gamepad);
                    controllers.erase(jid);
                }
            }
        }

        backend_update();
        hid_receiver_update();

        bool is_connected = backend_is_connected();
        if (is_connected != current_icon_connected) {
            SDL_SetWindowIcon(window, is_connected ? icon_green : icon_red);
            current_icon_connected = is_connected;
        }

        bool sent_report = poll_and_fuse();

        // UI building (synchronized to RenderThread's V-sync rate)
        if (ui_frame_requested.load()) {
            std::unique_lock<std::mutex> lock(imgui_mutex, std::try_to_lock);
            if (lock.owns_lock()) {
                ui_frame_requested.store(false);

                ImGui_ImplSDLGPU3_NewFrame();
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();

                ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
                const ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(viewport->Pos);
                ImGui::SetNextWindowSize(viewport->Size);

                bool window_open = true;
                ImGui::Begin("Main Window", &window_open, flags);

                backend_draw_ui();

                ImGui::Separator();
                hid_receiver_draw_ui();

                ImGui::Separator();
                ImGui::Text("Connected controllers:");
                const char* const* ButtonMapNames = backend_button_map_names();
                int valid_outputs_count = 0;
                const ButtonMap* valid_outputs = backend_valid_outputs(&valid_outputs_count);
                for (auto& pair : controllers) {
                    auto& ctrl = pair.second;
                    std::string label = ctrl.name + "##" + std::to_string(pair.first);

                    if (ImGui::ArrowButton(("##arrow" + std::to_string(pair.first)).c_str(), ctrl.ui_expanded ? ImGuiDir_Down : ImGuiDir_Right)) {
                        ctrl.ui_expanded = !ctrl.ui_expanded;
                    }
                    ImGui::SameLine();
                    float checkbox_x = ImGui::GetCursorPosX();
                    if (ImGui::Checkbox(label.c_str(), &ctrl.enabled)) {
                        controller_settings[ctrl.key].enabled = ctrl.enabled;
                    }
                    if (ImGui::IsItemHovered()) {
                        SDL_Joystick* joystick = ctrl.gamepad ? SDL_GetGamepadJoystick(ctrl.gamepad) : nullptr;
                        const char* path = joystick ? SDL_GetJoystickPath(joystick) : nullptr;
                        if (!ctrl.gamepad) {
                            path = ctrl.name.c_str();
                        }
                        ImGui::SetTooltip("%s", path ? path : "Unknown");
                    }

                    if (!ctrl.ui_expanded) {
                        // Total width is exactly 356 pixels
                        float start_x = ImGui::GetWindowContentRegionMax().x - 356.0f;
                        ImGui::SameLine(start_x);

                        float sz = 10.0f;
                        float spacing = 2.0f;
                        float offset_y = 6.0f;

                        auto draw_btn = [&](bool pressed) {
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            p.y += offset_y;
                            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), pressed ? IM_COL32(255, 255, 255, 255) : IM_COL32(100, 100, 100, 255), 2.0f);
                            ImGui::Dummy(ImVec2(sz, sz));
                            ImGui::SameLine(0, spacing);
                        };

                        auto draw_axis = [&](float fraction, float width) {
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            p.y += offset_y;
                            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + width, p.y + sz), IM_COL32(50, 50, 50, 255), 2.0f);
                            if (fraction > 0.0f) {
                                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + (width * fraction), p.y + sz), IM_COL32(200, 200, 200, 255), 2.0f);
                            }
                            ImGui::Dummy(ImVec2(width, sz));
                            ImGui::SameLine(0, spacing);
                        };

                        auto draw_axis_centered = [&](float fraction, float width) {
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            p.y += offset_y;
                            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + width, p.y + sz), IM_COL32(50, 50, 50, 255), 2.0f);

                            float center = p.x + width / 2.0f;
                            float end = p.x + (width * fraction);
                            if (end < center) {
                                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(end, p.y), ImVec2(center, p.y + sz), IM_COL32(200, 200, 200, 255), 2.0f);
                            } else {
                                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(center, p.y), ImVec2(end, p.y + sz), IM_COL32(200, 200, 200, 255), 2.0f);
                            }

                            ImGui::GetWindowDrawList()->AddLine(ImVec2(center, p.y), ImVec2(center, p.y + sz), IM_COL32(255, 50, 50, 255));

                            ImGui::Dummy(ImVec2(width, sz));
                            ImGui::SameLine(0, spacing);
                        };

                        // Face Buttons
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::South)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::East)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::West)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::North)]);
                        ImGui::SameLine(0, 4.0f);
                        // D-Pad
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::DpadLeft)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::DpadRight)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::DpadUp)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::DpadDown)]);
                        ImGui::SameLine(0, 4.0f);
                        // Bumpers & Stick Clicks (L1, R1, L3, R3)
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::L1)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::R1)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::L3)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::R3)]);
                        ImGui::SameLine(0, 4.0f);
                        // System
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::Select)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::Home)]);
                        draw_btn(ctrl.raw_btns[static_cast<int>(ButtonMap::Start)]);

                        ImGui::SameLine(0, 8.0f);

                        // Triggers
                        draw_axis(ctrl.raw_trigs[static_cast<int>(TriggerMap::L2)] / TRIGGER_MAX, 20.0f);
                        draw_axis(ctrl.raw_trigs[static_cast<int>(TriggerMap::R2)] / TRIGGER_MAX, 20.0f);

                        ImGui::SameLine(0, 4.0f);

                        // Left Stick
                        draw_axis_centered((ctrl.raw_sticks[static_cast<int>(StickMap::LeftStickX)] + 32768.0f) / 65535.0f, 24.0f);
                        draw_axis_centered((ctrl.raw_sticks[static_cast<int>(StickMap::LeftStickY)] + 32768.0f) / 65535.0f, 24.0f);

                        ImGui::SameLine(0, 4.0f);

                        // Right Stick
                        draw_axis_centered((ctrl.raw_sticks[static_cast<int>(StickMap::RightStickX)] + 32768.0f) / 65535.0f, 24.0f);
                        draw_axis_centered((ctrl.raw_sticks[static_cast<int>(StickMap::RightStickY)] + 32768.0f) / 65535.0f, 24.0f);

                        ImGui::NewLine();
                    }

                    if (ctrl.ui_expanded) {
                        float indent_val = checkbox_x - ImGui::GetCursorPosX();
                        ImGui::Indent(indent_val);

                        if (BACKEND_HAS_RUMBLE && ctrl.gamepad) {
                            if (ImGui::Checkbox(("Rumble##rumble" + std::to_string(pair.first)).c_str(), &ctrl.rumble)) {
                                controller_settings[ctrl.key].rumble = ctrl.rumble;
                            }
                        }

                        if (ImGui::Button(("Reset mapping##reset_" + std::to_string(pair.first)).c_str())) {
                            for (int i = 0; i < NUM_BUTTONS; ++i) {
                                ctrl.btn_map[i] = static_cast<ButtonMap>(i);
                                controller_settings[ctrl.key].btn_map[i] = static_cast<ButtonMap>(i);
                            }
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
                                ctrl.trigger_map[i] = (i == 0) ? ButtonMap::L2 : ButtonMap::R2;
                                controller_settings[ctrl.key].trigger_map[i] = (i == 0) ? ButtonMap::L2 : ButtonMap::R2;
                            }
                            for (int i = 0; i < 4; ++i) {
                                ctrl.stick_map[i] = static_cast<StickMap>(i);
                                controller_settings[ctrl.key].stick_map[i] = static_cast<StickMap>(i);
                            }
                        }

                        auto DrawComboBtn = [&](const char* label, int i) {
                            if (ImGui::BeginCombo((std::string("##combo_btn_") + std::to_string(i) + "_" + std::to_string(pair.first)).c_str(), ButtonMapNames[static_cast<int>(ctrl.btn_map[i])])) {
                                for (int n = 0; n < valid_outputs_count; n++) {
                                    int map_idx = static_cast<int>(valid_outputs[n]);
                                    bool is_selected = (static_cast<int>(ctrl.btn_map[i]) == map_idx);
                                    if (ImGui::Selectable(ButtonMapNames[map_idx], is_selected)) {
                                        ctrl.btn_map[i] = valid_outputs[n];
                                        controller_settings[ctrl.key].btn_map[i] = ctrl.btn_map[i];
                                    }
                                    if (is_selected) {
                                        ImGui::SetItemDefaultFocus();
                                    }
                                }
                                ImGui::EndCombo();
                            }
                        };

                        auto DrawComboTrig = [&](const char* label, int i) {
                            if (ImGui::BeginCombo((std::string("##combo_trig_") + std::to_string(i) + "_" + std::to_string(pair.first)).c_str(), ButtonMapNames[static_cast<int>(ctrl.trigger_map[i])])) {
                                for (int n = 0; n < valid_outputs_count; n++) {
                                    int map_idx = static_cast<int>(valid_outputs[n]);
                                    bool is_selected = (static_cast<int>(ctrl.trigger_map[i]) == map_idx);
                                    if (ImGui::Selectable(ButtonMapNames[map_idx], is_selected)) {
                                        ctrl.trigger_map[i] = valid_outputs[n];
                                        controller_settings[ctrl.key].trigger_map[i] = ctrl.trigger_map[i];
                                    }
                                    if (is_selected) {
                                        ImGui::SetItemDefaultFocus();
                                    }
                                }
                                ImGui::EndCombo();
                            }
                        };

                        auto DrawComboStick = [&](const char* label, int i) {
                            if (ImGui::BeginCombo((std::string("##combo_stick_") + std::to_string(i) + "_" + std::to_string(pair.first)).c_str(), StickMapNames[static_cast<int>(ctrl.stick_map[i])])) {
                                for (int n = 0; n < (int) StickMap::Count; n++) {
                                    bool is_selected = (static_cast<int>(ctrl.stick_map[i]) == n);
                                    if (ImGui::Selectable(StickMapNames[n], is_selected)) {
                                        ctrl.stick_map[i] = static_cast<StickMap>(n);
                                        controller_settings[ctrl.key].stick_map[i] = ctrl.stick_map[i];
                                    }
                                    if (is_selected) {
                                        ImGui::SetItemDefaultFocus();
                                    }
                                }
                                ImGui::EndCombo();
                            }
                        };

                        auto DrawRightAlignedLabel = [](const char* text, float intensity) {
                            ImGui::AlignTextToFramePadding();
                            float width = ImGui::CalcTextSize(text).x;
                            float avail = ImGui::GetContentRegionAvail().x;
                            float pad = (avail > width) ? (avail - width) : 0.0f;
                            float pos_x = ImGui::GetCursorPosX() + pad;

                            if (intensity > 0.0f) {
                                intensity = (std::min)(1.0f, intensity);
                                ImU32 bg_col = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, intensity));
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, bg_col);

                                ImVec4 text_color = ImVec4(1.0f - intensity, 1.0f - intensity, 1.0f - intensity, 1.0f);
                                ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                            }

                            ImGui::SetCursorPosX(pos_x);
                            ImGui::Text("%s", text);

                            if (intensity > 0.0f) {
                                ImGui::PopStyleColor();
                            }
                        };

                        if (ImGui::BeginTable(("remapping_btns_" + std::to_string(pair.first)).c_str(), 8)) {
                            std::vector<ButtonMap> btn_layout = {
                                ButtonMap::South, ButtonMap::East, ButtonMap::West, ButtonMap::North,
                                ButtonMap::DpadLeft, ButtonMap::DpadRight, ButtonMap::DpadUp, ButtonMap::DpadDown,
                                ButtonMap::L1, ButtonMap::R1, ButtonMap::L2, ButtonMap::R2,
                                ButtonMap::L3, ButtonMap::R3, ButtonMap::Select, ButtonMap::Start
                            };

                            auto has_button = [&](SDL_GamepadButton sdl_btn, ButtonMap btn) {
                                return ctrl.gamepad ? SDL_GamepadHasButton(ctrl.gamepad, sdl_btn) : ctrl.net_btns_present[static_cast<int>(btn)];
                            };

                            bool has_home = has_button(SDL_GAMEPAD_BUTTON_GUIDE, ButtonMap::Home);
                            bool has_misc = has_button(SDL_GAMEPAD_BUTTON_MISC1, ButtonMap::Misc1);
                            bool has_touch = has_button(SDL_GAMEPAD_BUTTON_TOUCHPAD, ButtonMap::Touchpad);
                            bool has_misc2 = has_button(SDL_GAMEPAD_BUTTON_MISC2, ButtonMap::Misc2);

                            bool has_row5 = has_home || has_misc || has_touch || has_misc2;
                            if (has_row5) {
                                btn_layout.push_back(has_home ? ButtonMap::Home : ButtonMap::Nothing);
                                btn_layout.push_back(has_misc ? ButtonMap::Misc1 : ButtonMap::Nothing);
                                btn_layout.push_back(has_touch ? ButtonMap::Touchpad : ButtonMap::Nothing);
                                btn_layout.push_back(has_misc2 ? ButtonMap::Misc2 : ButtonMap::Nothing);
                            }

                            bool has_l4 = has_button(SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, ButtonMap::L4);
                            bool has_r4 = has_button(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, ButtonMap::R4);
                            bool has_l5 = has_button(SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, ButtonMap::L5);
                            bool has_r5 = has_button(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, ButtonMap::R5);

                            bool has_row6 = has_l4 || has_r4 || has_l5 || has_r5;
                            if (has_row6) {
                                btn_layout.push_back(has_l4 ? ButtonMap::L4 : ButtonMap::Nothing);
                                btn_layout.push_back(has_r4 ? ButtonMap::R4 : ButtonMap::Nothing);
                                btn_layout.push_back(has_l5 ? ButtonMap::L5 : ButtonMap::Nothing);
                                btn_layout.push_back(has_r5 ? ButtonMap::R5 : ButtonMap::Nothing);
                            }

                            for (ButtonMap btn : btn_layout) {
                                if (btn == ButtonMap::L2 || btn == ButtonMap::R2) {
                                    int trig_idx = (btn == ButtonMap::L2) ? 0 : 1;
                                    ImGui::TableNextColumn();
                                    DrawRightAlignedLabel(InputTriggerMapNames[trig_idx], ctrl.raw_trigs[trig_idx] / TRIGGER_MAX);
                                    ImGui::TableNextColumn();
                                    ImGui::SetNextItemWidth(-FLT_MIN);
                                    DrawComboTrig("", trig_idx);
                                } else if (btn != ButtonMap::Nothing) {
                                    int idx = static_cast<int>(btn);
                                    ImGui::TableNextColumn();
                                    DrawRightAlignedLabel(GetInputButtonLabel(ctrl.gamepad, idx), ctrl.raw_btns[idx] ? 1.0f : 0.0f);
                                    ImGui::TableNextColumn();
                                    ImGui::SetNextItemWidth(-FLT_MIN);
                                    DrawComboBtn(ButtonMapNames[idx], idx);
                                } else {
                                    ImGui::TableNextColumn();
                                    ImGui::TableNextColumn();
                                }
                            }

                            auto get_stick_intensity = [&](int idx) {
                                float val = std::abs(static_cast<float>(ctrl.raw_sticks[idx]));
                                return val > STICK_DEADZONE ? (val - STICK_DEADZONE) / (STICK_MAX - STICK_DEADZONE) : 0.0f;
                            };

                            ImGui::TableNextColumn();
                            DrawRightAlignedLabel(StickMapNames[0], get_stick_intensity(0));
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            DrawComboStick(StickMapNames[0], 0);

                            ImGui::TableNextColumn();
                            DrawRightAlignedLabel(StickMapNames[1], get_stick_intensity(1));
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            DrawComboStick(StickMapNames[1], 1);

                            ImGui::TableNextColumn();
                            DrawRightAlignedLabel(StickMapNames[2], get_stick_intensity(2));
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            DrawComboStick(StickMapNames[2], 2);

                            ImGui::TableNextColumn();
                            DrawRightAlignedLabel(StickMapNames[3], get_stick_intensity(3));
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            DrawComboStick(StickMapNames[3], 3);

                            ImGui::EndTable();
                        }
                        ImGui::Unindent(indent_val);
                    }
                    ImGui::Separator();
                }
                ImGui::End();

                ImGui::Render();
            }
        }

        if (!sent_report || BACKEND_DELAY_AFTER_SEND) {
            SDL_DelayNS(750 * 1000);  // 750 microseconds
        }
    }

    render_thread.join();

    if (icon_red) {
        SDL_DestroySurface(icon_red);
    }
    if (icon_green) {
        SDL_DestroySurface(icon_green);
    }

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    hid_receiver_shutdown();
    backend_shutdown();

    SaveSettings();

    for (auto& pair : controllers) {
        if (pair.second.gamepad) {
            SDL_CloseGamepad(pair.second.gamepad);
        }
    }
    SDL_Quit();

    return 0;
}
