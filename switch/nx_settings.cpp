#include "nx_settings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
    constexpr int COUNT = static_cast<int>(nx::Setting::Count);
    constexpr std::initializer_list<const char*> OFF_ON = {"Off", "On"};

    // Order follows nx::Setting.
    const nx::SettingInfo SETTINGS[COUNT] = {
        {"Display", "aspect_ratio", "Aspect Ratio",
         "16:9 fills the screen. 4:3 shows the original framing with black bars on the sides.",
         0, {"16:9", "4:3"}, 0, 0, 0, true},
        {"Display", "render_quality", "Render Quality",
         "How sharp the game is drawn. Native is the N64's own 320x240, 480p and 720p draw it 2x and 3x as large, "
         "and 1080p (experimental) draws it 4x into a 1080p picture, for a docked console: heavier on the GPU.",
         2, {"Native N64 (240p)", "480p", "720p", "1080p (experimental)"}, 0, 0, 0, true},
        {"Display", "texture_lod", "Texture LOD", "GLideN64's level of detail emulation.", 0, OFF_ON, 0, 0, 0, true},
        {"Display", "texture_filter", "Texture Filter",
         "Standard bilinear filtering, or the N64's own 3-point filter. Every shader is compiled again at the next start.",
         0, {"Standard", "N64 3-Point"}, 0, 0, 0, true},
        {"Performance", "frame_rate_fps", "Frame Rate",
         "Frames shown per second. The game runs at its own 30; above that, in-between images are drawn, as on PC.",
         30, {}, 30, 60, 5, false, " fps"},
        {"Performance", "clock_profile", "Clock Profile",
         "Clocks while in the game. Docked GPU (handheld only): the official 768 MHz GPU profile. Horizon OC "
         "(handheld only): the docked clocks set in Horizon OC. CPU Boost (needs Horizon OC): CPU at 1785 MHz.",
         0, {"Console Default", "Docked GPU in Handheld", "Horizon OC Docked Clocks", "CPU Boost (Horizon OC)"}},
        {"Controls", "face_buttons", "Face Buttons", "Flipped swaps the A and B buttons in the game.", 0, {"Normal", "Flipped"}},
        {"Audio", "master_volume", "Master Volume", "Volume of everything the game plays.", 100, {}, 0, 100, 5, false, "%"},
        {"Audio", "bgm_volume", "Music Volume", "Background music volume.", 100, {}, 0, 100, 5, false, "%"},
        {"Display", "cutscene_aspect", "Cutscene Aspect Ratio",
         "Original keeps cutscenes at 4:3, Clamp 16:9 limits them to 16:9, Full uses the whole screen.",
         2, {"Original", "Clamp 16:9", "Full"}},
        {"Display", "render_distance", "Extended Render Distance",
         "Loads more of the stage around you, in every direction, so enemies and objects do not pop in at a distance "
         "or at the sides of the wide view. Each step adds one more ring of chunks and can lower the frame rate.",
         0, {"Off", "+1 chunk", "+2 chunks", "+3 chunks"}},
    };

    std::atomic<int> values[COUNT];
    int startup_values[COUNT];

    int index(nx::Setting setting) { return static_cast<int>(setting); }

    int clamp(const nx::SettingInfo& info, int value) {
        if (info.choices.size() > 0) {
            return std::clamp(value, 0, static_cast<int>(info.choices.size()) - 1);
        }
        return std::clamp(value, info.min, info.max);
    }
}

const nx::SettingInfo& nx::info(Setting setting) { return SETTINGS[index(setting)]; }

int nx::get(Setting setting) { return values[index(setting)].load(std::memory_order_relaxed); }

void nx::set(Setting setting, int value) {
    values[index(setting)].store(clamp(info(setting), value), std::memory_order_relaxed);
}

void nx::apply_text(const std::string& text) {
    for (int i = 0; i < COUNT; i++) values[i] = SETTINGS[i].default_value;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        char key[64];
        char value_text[16];
        if (std::sscanf(line.c_str(), " %63[a-z_] = %15s", key, value_text) != 2) continue;
        char* value_end = nullptr;
        // strtol saturates instead of overflowing like %d.
        const long value = std::strtol(value_text, &value_end, 10);
        if (value_end == value_text) continue;
        for (int i = 0; i < COUNT; i++) {
            if (std::strcmp(key, SETTINGS[i].key) == 0) {
                set(static_cast<Setting>(i), static_cast<int>(std::clamp(value, -1000L, 1000L)));
            }
        }
    }
    for (int i = 0; i < COUNT; i++) startup_values[i] = values[i];
}

std::string nx::settings_text() {
    std::string text;
    for (int i = 0; i < COUNT; i++) {
        const nx::SettingInfo& setting = SETTINGS[i];
        text += std::string("# ") + setting.label + ": ";
        if (setting.choices.size() > 0) {
            int n = 0;
            for (const char* choice : setting.choices) {
                text += (n > 0 ? ", " : "") + std::to_string(n) + " = " + choice;
                n++;
            }
        } else {
            text += std::to_string(setting.min) + " to " + std::to_string(setting.max) + setting.suffix;
        }
        if (setting.needs_restart) text += "  (applies after a restart)";
        text += std::string("\n") + setting.key + " = " + std::to_string(values[i].load(std::memory_order_relaxed)) + "\n";
    }
    return text;
}

int nx::at_startup(Setting setting) { return startup_values[index(setting)]; }

bool nx::restart_pending() {
    for (int i = 0; i < COUNT; i++) {
        if (SETTINGS[i].needs_restart && values[i].load(std::memory_order_relaxed) != startup_values[i]) return true;
    }
    return false;
}
