// SDL pad -> N64 pad for player 1. The default layout follows PaperBoat-NX (NSO N64 convention):
// A->A, B->B, ZL->Z, L->L, R/ZR->R, +->Start, right stick->C buttons, D-pad->D-pad. It can be changed in
// sdmc:/switch/bmherorecomp/controls.ini (nx_controls.cpp).
//
// All four ports are reported as plugged-in controllers with a Rumble Pak, exactly as BMHero's own
// get_connected_device_info does in single-player mode (src/main/main.cpp). That matters: patches/required_patches.c
// reverses the bit pattern osContInit returns (a workaround for the pinned runtime) and then tests bit 0 to decide
// whether to show "no controller", so with only port 1 connected the test lands on the empty port 4. Only port 1
// carries input; ports 2-4 read neutral. A pad that is missing (or not detected yet) also reads as neutral. The pad is found by
// scanning at startup and on every hot-plug: an SDL game controller when it has a mapping, otherwise the raw joystick
// (switch-sdl2 reports the HID pad that way, with buttons in HidNpadButton bit order).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>

#include <SDL.h>

extern "C" void bmhnx_menu_pause_point();  // nx_menu.cpp

#include "nx_controls.h"
#include "nx_pad.h"
#include "nx_settings.h"
#include "recompinput/recompinput.h"
#include "ultramodern/input.hpp"

namespace {
    constexpr float AXIS_MAX = 32767.0f;
    constexpr float STICK_DEADZONE = 0.1f;
    constexpr uint16_t RUMBLE_STRENGTH = 0x6000;
    // Re-armed on every VI while the game keeps rumble on, so a short duration stops it promptly.
    constexpr uint32_t RUMBLE_PULSE_MS = 100;

    std::atomic<SDL_GameController*> current_controller{nullptr};
    std::atomic<SDL_Joystick*> current_joystick{nullptr};  // pad without a controller mapping
    std::atomic_bool rumble_on{false};
    std::atomic_bool right_analog_suppressed{false};
    NxStartFilter start_filter;  // game thread only
    std::atomic<uint32_t> input_polls{0};      // get_input calls for port 1
    std::atomic<uint32_t> device_queries{0};   // get_connected_device_info calls, all ports
    std::atomic<uint16_t> last_buttons{0};     // last N64 buttons given to the game

    float read_axis(SDL_GameController* controller, SDL_GameControllerAxis axis) {
        float value = SDL_GameControllerGetAxis(controller, axis) / AXIS_MAX;
        return std::fabs(value) < STICK_DEADZONE ? 0.0f : value;
    }

    NxPad read_controller(SDL_GameController* controller) {
        struct Binding { SDL_GameControllerButton button; uint32_t nx; };
        static constexpr Binding bindings[] = {
            // switch-sdl2 reports the Xbox-style positional layout: SDL's A is the bottom button, which is the
            // Switch's B, and SDL's X is the left button, which is the Switch's Y. NX_A/NX_B/NX_X/NX_Y are the
            // buttons' printed labels, as in the menu, in config.ini and in read_joystick below.
            {SDL_CONTROLLER_BUTTON_A, NX_B},
            {SDL_CONTROLLER_BUTTON_B, NX_A},
            {SDL_CONTROLLER_BUTTON_X, NX_Y},
            {SDL_CONTROLLER_BUTTON_Y, NX_X},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, NX_L},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, NX_R},
            {SDL_CONTROLLER_BUTTON_START, NX_PLUS},
            {SDL_CONTROLLER_BUTTON_BACK, NX_MINUS},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, NX_UP},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, NX_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, NX_LEFT},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, NX_RIGHT},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, NX_LSTICK},
            {SDL_CONTROLLER_BUTTON_RIGHTSTICK, NX_RSTICK},
        };
        NxPad pad;
        for (const Binding& binding : bindings) {
            if (SDL_GameControllerGetButton(controller, binding.button)) {
                pad.buttons |= binding.nx;
            }
        }
        // ZL/ZR are analog axes in SDL.
        if (read_axis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f) pad.buttons |= NX_ZL;
        if (read_axis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f) pad.buttons |= NX_ZR;
        // SDL's Y axes point down.
        pad.lx = read_axis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        pad.ly = -read_axis(controller, SDL_CONTROLLER_AXIS_LEFTY);
        pad.rx = read_axis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
        pad.ry = -read_axis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
        return pad;
    }

    // switch-sdl2 numbers the HID pad's buttons in HidNpadButton bit order; axes are LX, LY, RX, RY.
    NxPad read_joystick(SDL_Joystick* joystick) {
        static constexpr uint32_t buttons[16] = {
            NX_A, NX_B, NX_X, NX_Y, NX_LSTICK, NX_RSTICK, NX_L, NX_R,
            NX_ZL, NX_ZR, NX_PLUS, NX_MINUS, NX_LEFT, NX_UP, NX_RIGHT, NX_DOWN,
        };
        NxPad pad;
        const int count = std::min(SDL_JoystickNumButtons(joystick), 16);
        for (int i = 0; i < count; i++) {
            if (SDL_JoystickGetButton(joystick, i)) pad.buttons |= buttons[i];
        }
        auto axis = [&](int index) {
            if (index >= SDL_JoystickNumAxes(joystick)) return 0.0f;
            const float value = SDL_JoystickGetAxis(joystick, index) / AXIS_MAX;
            return std::fabs(value) < STICK_DEADZONE ? 0.0f : value;
        };
        pad.lx = axis(0);
        pad.ly = -axis(1);
        pad.rx = axis(2);
        pad.ry = -axis(3);
        return pad;
    }

    void log_joysticks(const char* why) {
        std::printf("[BMHNX] input (%s): %d joystick(s)\n", why, SDL_NumJoysticks());
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            char guid[40];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
            const char* name = SDL_JoystickNameForIndex(i);
            std::printf("[BMHNX]   #%d '%s' guid %s game controller %d\n", i, name ? name : "?", guid,
                        SDL_IsGameController(i) ? 1 : 0);
        }
    }

    // Main thread. Opens the first usable pad when none is open yet.
    void try_open_pad() {
        if (current_controller.load() != nullptr || current_joystick.load() != nullptr) {
            return;
        }
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                if (SDL_GameController* controller = SDL_GameControllerOpen(i)) {
                    std::printf("[BMHNX] input: using game controller '%s'\n", SDL_GameControllerName(controller));
                    current_controller.store(controller);
                    return;
                }
            }
        }
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_Joystick* joystick = SDL_JoystickOpen(i)) {
                std::printf("[BMHNX] input: using joystick '%s' (%d buttons, %d axes), no controller mapping\n",
                            SDL_JoystickName(joystick), SDL_JoystickNumButtons(joystick), SDL_JoystickNumAxes(joystick));
                current_joystick.store(joystick);
                return;
            }
        }
    }

    // The pad as the game sees it: neutral while the NX Options menu owns it.
    bool read_game_pad(NxPad* pad) {
        const bool connected = bmhnx_read_pad(pad);
        if (bmhnx_menu_owns_pad()) {
            *pad = NxPad{};
        }
        return connected;
    }
}

#ifdef BMHNX_REMOTE
bool bmhnx_remote_apply(NxPad* pad);
#endif

bool bmhnx_read_pad(NxPad* pad) {
    *pad = NxPad{};
    bool connected = false;
    if (SDL_GameController* controller = current_controller.load()) {
        *pad = read_controller(controller);
        connected = true;
    } else if (SDL_Joystick* joystick = current_joystick.load()) {
        *pad = read_joystick(joystick);
        connected = true;
    }
#ifdef BMHNX_REMOTE
    connected = bmhnx_remote_apply(pad) || connected;
#endif
    return connected;
}

// Called on the main thread, which owns the SDL event queue, on every pass of its loop.
void bmhnx_input_update() {
    static bool logged = false;
    if (!logged) {
        logged = true;
        log_joysticks("startup");
    }
    try_open_pad();
}

// Called on the main thread, which owns the SDL event queue.
void bmhnx_input_handle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_JOYDEVICEADDED:
        std::printf("[BMHNX] input: %s (device %d)\n",
                    event.type == SDL_CONTROLLERDEVICEADDED ? "controller added" : "joystick added",
                    event.type == SDL_CONTROLLERDEVICEADDED ? event.cdevice.which : event.jdevice.which);
        try_open_pad();
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
    case SDL_JOYDEVICEREMOVED: {
        const SDL_JoystickID removed = event.type == SDL_CONTROLLERDEVICEREMOVED ? event.cdevice.which : event.jdevice.which;
        SDL_GameController* controller = current_controller.load();
        if (controller != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == removed) {
            // ponytail: the handle is dropped, not closed, because the game thread may be reading it.
            current_controller.store(nullptr);
            std::printf("[BMHNX] input: controller removed\n");
        }
        SDL_Joystick* joystick = current_joystick.load();
        if (joystick != nullptr && SDL_JoystickInstanceID(joystick) == removed) {
            current_joystick.store(nullptr);
            std::printf("[BMHNX] input: joystick removed\n");
        }
        try_open_pad();  // another pad may already be connected
        break;
    }
    default:
        break;
    }
}

bool bmhnx_get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num < 0 || controller_num > 3) {
        return false;
    }
    if (controller_num != 0) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return true;  // connected, but only player 1 has input
    }
    bmhnx_menu_pause_point();  // halts the game while the pause menu is open
    input_polls.fetch_add(1);
    NxPad pad;
    read_game_pad(&pad);  // neutral while no pad is detected
    if (nx::get(nx::Setting::FaceButtons) != 0) {
        const uint32_t a = pad.buttons & NX_A;
        const uint32_t b = pad.buttons & NX_B;
        pad.buttons = (pad.buttons & ~(NX_A | NX_B)) | (a ? NX_B : 0) | (b ? NX_A : 0);
    }
    pad.buttons = start_filter.apply(pad.buttons, SDL_GetTicks64());
    *buttons = nx::controls::to_n64_buttons(pad);
    last_buttons.store(*buttons);
    nx::controls::analog_stick(pad, x, y);
    return true;  // always plugged in
}

ultramodern::input::connected_device_info_t bmhnx_get_connected_device_info(int controller_num) {
    device_queries.fetch_add(1);
    if (controller_num >= 0 && controller_num <= 3) {
        return {ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak};
    }
    return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
}

// One status line for the log thread: whether a pad is open and whether the game is polling it.
extern "C" void bmhnx_input_log_status() {
    const char* pad = current_controller.load() != nullptr ? "controller"
                      : current_joystick.load() != nullptr ? "joystick" : "none";
    std::printf("[BMHNX] input status: pad %s, game polled port 1 %u times, asked about devices %u times, "
                "last N64 buttons 0x%04x\n", pad, input_polls.load(), device_queries.load(), last_buttons.load());
}

// The main thread pumps SDL events every millisecond, so controller state is already fresh.
void recompinput::poll_inputs() {}

void recompinput::set_rumble(int controller_num, bool on) {
    if (controller_num == 0) {
        rumble_on.store(on);
    }
}

void recompinput::update_rumble() {
    if (!rumble_on.load()) {
        return;
    }
    if (SDL_GameController* controller = current_controller.load()) {
        SDL_GameControllerRumble(controller, RUMBLE_STRENGTH, RUMBLE_STRENGTH, RUMBLE_PULSE_MS);
    } else if (SDL_Joystick* joystick = current_joystick.load()) {
        SDL_JoystickRumble(joystick, RUMBLE_STRENGTH, RUMBLE_STRENGTH, RUMBLE_PULSE_MS);
    }
}

void recompinput::get_gyro_deltas(int, float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

void recompinput::get_mouse_deltas(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

void recompinput::get_right_analog(int controller_num, float* x, float* y) {
    NxPad pad;
    if (controller_num != 0 || right_analog_suppressed.load() || !read_game_pad(&pad)) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    *x = pad.rx;
    *y = pad.ry;
}

void recompinput::set_right_analog_suppressed(bool suppressed) {
    right_analog_suppressed.store(suppressed);
}

bool recompinput::game_input_disabled() {
    return false;
}
