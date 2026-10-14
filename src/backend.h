// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

// Everything that differs between the applications built from this codebase.
// Each application provides its own implementation of this interface.

#pragma once

#include <nlohmann/json.hpp>

#include "common.h"

// Window title and settings directory name.
extern const char* const BACKEND_APP_NAME;
// Whether per-controller rumble settings are available.
extern const bool BACKEND_HAS_RUMBLE;
// Whether the main loop sleeps even after a report was sent.
extern const bool BACKEND_DELAY_AFTER_SEND;

bool backend_init();
void backend_shutdown();

// Returns true if the key was a global setting belonging to the backend.
bool backend_load_setting(const std::string& key, const nlohmann::json& value);
void backend_save_settings(nlohmann::json& j);

// Names of the outputs, indexed by ButtonMap.
const char* const* backend_button_map_names();
// Outputs that inputs can be mapped to.
const ButtonMap* backend_valid_outputs(int* count);

// Returns true if the controller should not be used as an input.
bool backend_ignore_gamepad(SDL_Gamepad* gamepad);
// Whether a controller we have no settings for starts out enabled.
// gamepad is null for controllers that do not come from SDL.
bool backend_default_enabled(SDL_Gamepad* gamepad, const char* name);

// Called on WM_DEVICECHANGE.
void backend_on_device_change();
// Called once per main loop iteration, before inputs are polled.
void backend_update();
bool backend_is_connected();
// Returns true if the state was sent (or at least consumed).
bool backend_send_state(const ControllerState& state);

// Draws the part of the UI above the list of controllers.
void backend_draw_ui();
