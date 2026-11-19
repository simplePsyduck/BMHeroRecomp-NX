// One configuration file for everything the pause menu can change: sdmc:/switch/BMHero_nx/config.ini, with a
// [settings] section (video, audio, performance: nx_settings) and a [controls] section (button bindings and the menu
// hotkey: nx_controls). The menu rewrites the whole file whenever something changes.
//
// On the first run after the single file was introduced, the older nx_options.txt and controls.ini are read once and
// their values carried over; they are left where they are and not read again.
#pragma once

namespace nx::config {
    // Reads config.ini (writing it with the defaults when it does not exist). Call once before the game starts.
    void load();
    // Writes both sections to config.ini.
    void save();
}
