// Options for the Switch build, the [settings] section of sdmc:/switch/BMHero_nx/config.ini ("key = value" lines).
// The game reads them through the banjo::get_* getters (frontend_switch.cpp). Display options are read once at
// startup; at_startup() returns the value the renderer is actually using.
#pragma once

#include <initializer_list>
#include <string>

namespace nx {
    enum class Setting {
        AspectRatio,      // 0 = 16:9 (RT64 "Expand"), 1 = 4:3
        Resolution,       // render quality: 0 native N64 (1x, 240p), 1 = 480p (2x), 2 = 720p (3x), 3 = 1080p (4x output)
        TextureLod,
        TextureFilter,
        FrameRate,        // 30..60; above 30 draws interpolated in-between images
        ClockProfile,
        FaceButtons,
        MasterVolume,
        BgmVolume,
        CutsceneAspect,   // banjo::CutsceneAspectRatioMode: 0 Original, 1 Clamp16x9, 2 Full
        Count
    };

    struct SettingInfo {
        const char* section;
        const char* key;          // name in nx_options.txt
        const char* label;
        const char* description;
        int default_value;
        // A choice setting stores the index of its choice; a number setting a value in [min, max].
        std::initializer_list<const char*> choices;
        int min = 0;
        int max = 0;
        int step = 0;
        bool needs_restart = false;
        const char* suffix = "";
    };

    const SettingInfo& info(Setting setting);
    int get(Setting setting);
    void set(Setting setting, int value);  // clamps to the setting's range
    // The [settings] section of config.ini: "key = value" lines. apply_text starts from the defaults, takes what the
    // text says (unknown keys and malformed lines are skipped, out-of-range values clamped) and remembers the result as
    // the startup values; settings_text is the section as it is written back, each setting under a comment line.
    void apply_text(const std::string& text);
    std::string settings_text();
    void save();                           // writes config.ini (nx_config.cpp)
    int at_startup(Setting setting);
    // True when a setting that is read once at startup (needs_restart) differs from the value in use.
    bool restart_pending();

    inline int resolution_factor() { return at_startup(Setting::Resolution) + 1; }
    inline bool output_1080p() { return at_startup(Setting::Resolution) == 3; }
    inline int screen_width() { return output_1080p() ? 1920 : 1280; }
    inline int screen_height() { return output_1080p() ? 1080 : 720; }
    inline bool widescreen() { return at_startup(Setting::AspectRatio) == 0; }
    inline int target_fps() { return get(Setting::FrameRate); }
    inline bool above_30() { return target_fps() > 30; }
    // BMHero has no "game runs faster than 30" mode (that needs per-game RDRAM hooks), so above 30 is always
    // interpolated in-between images.
    inline bool true_frames() { return false; }
    inline bool interpolating() { return above_30(); }
}
