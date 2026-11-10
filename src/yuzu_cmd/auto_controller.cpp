// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "yuzu_cmd/auto_controller.h"
#include "yuzu_cmd/sdl_config.h"

namespace {
bool HasConnectedController(const Settings::PlayerInput& player,
                            const std::vector<Common::ParamPackage>& devices) {
    if (!player.connected) {
        return false;
    }
    bool found_controller = false;
    const auto connected = [&](const std::string& binding) {
        const Common::ParamPackage params{binding};
        const auto engine = params.Get("engine", "");
        if (engine.empty() || engine == "keyboard" || engine == "analog_from_button" ||
            engine == "mouse") {
            return true;
        }
        found_controller = true;
        return std::ranges::any_of(devices, [&](const auto& device) {
            if (device.Get("engine", "") != engine) {
                return false;
            }
            for (const auto* key : {"guid", "port", "pad"}) {
                if (params.Has(key) && params.Get(key, "") != device.Get(key, "")) {
                    return false;
                }
            }
            return true;
        });
    };
    const bool available = std::ranges::all_of(player.buttons, connected) &&
                           std::ranges::all_of(player.analogs, connected);
    return found_controller && available;
}

// NOVAS_BUTTON_MAP (NovaS: Ajustes > Configurar mando): "A=south;B=east;ZR=rt;..." says which button of the
// player's pad does each Switch button. Pad buttons are named by position on an Xbox-style pad (south is the
// bottom face button); each one's binding is the one the default mapping gives the Switch button at that place.
std::optional<Settings::NativeButton::Values> SwitchButton(std::string_view name) {
    using namespace Settings::NativeButton;
    static constexpr std::pair<std::string_view, Values> names[]{
        {"A", A},         {"B", B},         {"X", X},           {"Y", Y},         {"L", L},
        {"R", R},         {"ZL", ZL},       {"ZR", ZR},         {"Plus", Plus},   {"Minus", Minus},
        {"LStick", LStick}, {"RStick", RStick}, {"DUp", DUp},   {"DDown", DDown}, {"DLeft", DLeft},
        {"DRight", DRight},
    };
    for (const auto& [key, value] : names) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

// The Switch button the default mapping puts on each pad button (the defaults go by position).
std::optional<Settings::NativeButton::Values> DefaultOnPadButton(std::string_view name) {
    using namespace Settings::NativeButton;
    static constexpr std::pair<std::string_view, Values> pad[]{
        {"east", A},       {"south", B},       {"north", X},   {"west", Y},       {"lb", L},
        {"rb", R},         {"lt", ZL},         {"rt", ZR},     {"start", Plus},   {"back", Minus},
        {"ls", LStick},    {"rs", RStick},     {"up", DUp},    {"down", DDown},   {"left", DLeft},
        {"right", DRight},
    };
    for (const auto& [key, value] : pad) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

// (Switch button, Switch button whose default binding it takes), from NOVAS_BUTTON_MAP.
std::vector<std::pair<Settings::NativeButton::Values, Settings::NativeButton::Values>> CustomButtons() {
    std::vector<std::pair<Settings::NativeButton::Values, Settings::NativeButton::Values>> result;
    const char* value = std::getenv("NOVAS_BUTTON_MAP");
    if (value == nullptr) {
        return result;
    }
    std::string_view rest{value};
    while (!rest.empty()) {
        const auto end = rest.find(';');
        const auto item = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        const auto equals = item.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const auto target = SwitchButton(item.substr(0, equals));
        const auto source = DefaultOnPadButton(item.substr(equals + 1));
        if (target && source) {
            result.emplace_back(*target, *source);
        }
    }
    return result;
}

bool SameBindings(const Settings::PlayerInput& lhs, const Settings::PlayerInput& rhs) {
    return lhs.connected == rhs.connected && lhs.controller_type == rhs.controller_type &&
           lhs.buttons == rhs.buttons && lhs.analogs == rhs.analogs && lhs.motions == rhs.motions;
}
} // namespace

AutoController::AutoController()
    : configured{Settings::values.players.GetValue()[0]}, keyboard{configured} {
    keyboard.connected = true;
    keyboard.controller_type = Settings::ControllerType::ProController;
    for (std::size_t i = 0; i < keyboard.buttons.size(); ++i) {
        if (Common::ParamPackage{keyboard.buttons[i]}.Get("engine", "") != "keyboard") {
            keyboard.buttons[i] = InputCommon::GenerateKeyboardParam(SdlConfig::default_buttons[i]);
        }
    }
    for (std::size_t i = 0; i < keyboard.analogs.size(); ++i) {
        if (Common::ParamPackage{keyboard.analogs[i]}.Get("engine", "") != "analog_from_button") {
            const auto& keys = SdlConfig::default_analogs[i];
            keyboard.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(
                keys[0], keys[1], keys[2], keys[3], SdlConfig::default_stick_mod[i], 0.5f);
        }
    }
    for (std::size_t i = 0; i < keyboard.motions.size(); ++i) {
        if (Common::ParamPackage{keyboard.motions[i]}.Get("engine", "") != "keyboard") {
            keyboard.motions[i] = InputCommon::GenerateKeyboardParam(SdlConfig::default_motions[i]);
        }
    }
}

void AutoController::Update(InputCommon::InputSubsystem& input, Core::System& system) {
    const auto devices = input.GetInputDevices();
    auto& player = Settings::values.players.GetValue()[0];
    if (HasConnectedController(player, devices)) {
        return;
    }

    auto replacement = keyboard;
    std::string device_name = "keyboard";
    if (HasConnectedController(configured, devices)) {
        replacement = configured;
        device_name = "configured controller";
    } else {
        for (const auto& device : devices) {
            if (device.Get("engine", "") != "sdl") {
                continue;
            }
            const auto buttons = input.GetButtonMappingForDevice(device);
            const auto analogs = input.GetAnalogMappingForDevice(device);
            if (buttons.empty() || analogs.empty()) {
                continue;
            }
            const auto motions = input.GetMotionMappingForDevice(device);
            for (const auto& [index, params] : buttons) {
                replacement.buttons[index] = params.Serialize();
            }
            for (const auto& [index, params] : analogs) {
                replacement.analogs[index] = params.Serialize();
            }
            for (const auto& [index, params] : motions) {
                replacement.motions[index] = params.Serialize();
            }
            // NOVAS_BUTTON_LABELS=1 (NovaS: Ajustes > Botones del mando, "Según la letra"): the pad's
            // A/B and X/Y act as the Switch buttons with the same letter (Xbox style). By default they
            // go by position, like on the console (the right face button is A).
            static const bool by_label = [] {
                const char* value = std::getenv("NOVAS_BUTTON_LABELS");
                return value != nullptr && value[0] == '1';
            }();
            static const auto custom = CustomButtons();
            if (!custom.empty()) {
                const auto defaults = replacement.buttons;
                for (const auto& [target, source] : custom) {
                    replacement.buttons[target] = defaults[source];
                }
                LOG_INFO(Input, "NovaS player 1: {} buttons set in Configurar mando", custom.size());
            } else if (by_label) {
                LOG_INFO(Input, "NovaS player 1: buttons by label (A/B and X/Y swapped)");
                std::swap(replacement.buttons[Settings::NativeButton::A],
                          replacement.buttons[Settings::NativeButton::B]);
                std::swap(replacement.buttons[Settings::NativeButton::X],
                          replacement.buttons[Settings::NativeButton::Y]);
            }
            device_name = device.Get("display", "SDL controller");
            break;
        }
    }

    if (SameBindings(player, replacement)) {
        return;
    }
    // Session-only bindings: the saved configuration remains the user's preference.
    player = std::move(replacement);
    system.HIDCore().GetEmulatedControllerByIndex(0)->ReloadFromSettings();
    LOG_INFO(Input, "NovaS player 1: {}", device_name);
}
