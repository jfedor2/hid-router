// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

// Optionally listens on the UDP port used by HID Forwarder and makes
// the gamepads it receives inputs for appear as controllers.

#pragma once

#include <nlohmann/json.hpp>

#include <string>

// Returns true if the key was a global setting belonging to the receiver.
bool hid_receiver_load_setting(const std::string& key, const nlohmann::json& value);
void hid_receiver_save_settings(nlohmann::json& j);

// Called once per main loop iteration, before inputs are polled.
void hid_receiver_update();
void hid_receiver_draw_ui();
void hid_receiver_shutdown();
