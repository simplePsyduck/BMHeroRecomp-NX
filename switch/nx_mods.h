// The Mods screen of the pause menu: lists the mods in the mods folder and switches them on or off.
//
// librecomp loads mods when the game starts and ignores enable_mod afterwards for mods that cannot be toggled while
// the game runs, so a choice is written to mods.json (next to the mods folder, in librecomp's own format) and takes
// effect after a game reset. All calls into the runtime's mod manager are in nx_mods.cpp.
#pragma once

#include <string>
#include <vector>

namespace nx::mods {
    // Reads the mod list from the runtime. Choices made earlier in this run are kept; what each mod was at the start
    // of the run is remembered the first time, to tell whether a reset is needed.
    void refresh();
    int count();
    const std::string& name(int index);
    // The mod will be on after the next start: chosen, or needed by a chosen mod (see needed).
    bool on(int index);
    bool chosen(int index);
    // On only because a chosen mod depends on it.
    bool needed(int index);
    // Flips the choice and writes mods.json.
    void toggle(int index);
    // Some mod will be on or off differently after the next start than it is now.
    bool reset_needed();
    // The mods folder, for the screen's note.
    std::string folder();

    // librecomp's mods.json with its "enabled_mods" list replaced (the other keys kept; a missing or unreadable file
    // gives a new one). Exposed for tests.
    std::string rewrite_json(const std::string& old_json, const std::vector<std::string>& enabled);
}
