// Switch -> N64 button mapping for player 1, and the menu hotkey: the [controls] section of
// sdmc:/switch/BMHero_nx/config.ini, changed in the pause menu (Controllers) or by editing the file.
#pragma once

#include <cstdint>
#include <string>

#include "nx_pad.h"

namespace nx::controls {
    // Rows of the binding list: the 14 N64 inputs, then the menu hotkey.
    constexpr int INPUT_COUNT = 14;
    constexpr int HOTKEY_ROW = INPUT_COUNT;
    constexpr int ROW_COUNT = INPUT_COUNT + 1;

    // The [controls] section of config.ini. apply_text starts from the default layout and takes what the text says
    // (so a missing or half-valid section is still playable); text is the section as it is written back, help_text the
    // comment lines above it. apply_text returns true when the text was written for the older button naming (no
    // LAYOUT line) and was converted: the caller should write it back.
    bool apply_text(const std::string& text);
    std::string text();
    const char* help_text();
    // Writes config.ini (nx_config.cpp).
    void save();
    // Back to the default layout (the menu hotkey included), saved.
    void reset_defaults();

    // N64 button bits (the OS_CONT_* layout) for a Switch pad.
    uint16_t to_n64_buttons(const NxPad& pad);
    // The N64 analog stick, from the left stick (default) or the right stick (STICK = RIGHT).
    void analog_stick(const NxPad& pad, float* x, float* y);

    // The menu hotkey is held down.
    bool hotkey_held(const NxPad& pad);

    // Menu support. Rows are 0..INPUT_COUNT-1 for the N64 inputs and HOTKEY_ROW for the menu hotkey.
    const char* row_label(int row);
    // The Switch inputs bound to a row, e.g. "R / ZR", or "Not bound".
    std::string describe(int row);
    void clear(int row);
    // Waits for a new press: a button, or a stick pushed past the halfway point. `base` is the pad when the wait
    // began; it is kept up to date so that releasing and pressing the same input again counts. Replaces the row's
    // bindings with the input (or adds it, with add) and returns true once one is caught.
    bool capture(int row, const NxPad& pad, NxPad* base, bool add);
}
