// Switch replacements for the recompui calls and the banjo:: config getters declared in include/banjo_config.h.
// The PC port reads these options from RmlUi menus (src/game/config.cpp, not compiled here); on Switch they come
// from nx_settings. The game patches still import some Banjo-template options (analog cam, note saving, chunk
// radius): they get fixed defaults so the shared recomp_api.cpp links unchanged.
#include <cstdio>

#include <SDL.h>
#include <switch.h>
#include <unistd.h>

#include "banjo_config.h"
#include "banjo_sound.h"
#include "nx_paths.h"
#include "nx_settings.h"
#include "recomp.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"

extern SDL_Window* bmhnx_window;

void recompui::message_box(const char* msg) {
    fprintf(stderr, "[BMHNX] %s\n", msg);
    flockfile(stderr);
    fflush(stderr);
    fsync(fileno(stderr));
    funlockfile(stderr);
    ErrorApplicationConfig config;
    errorApplicationCreate(&config, BMHNX_DISPLAY_NAME, msg);
    errorApplicationShow(&config);
}

void recompui::get_window_size(int& width, int& height) {
    if (bmhnx_window == nullptr) {
        width = 1280;
        height = 720;
        return;
    }
    SDL_GetWindowSize(bmhnx_window, &width, &height);
}

bool recompui::renderer::RT64HighPrecisionFBEnabled() {
    return false;
}

void banjo::init_config() {}
void banjo::open_quit_game_prompt() {}

banjo::CameraInvertMode banjo::get_camera_invert_mode() { return CameraInvertMode::InvertNone; }
banjo::CameraInvertMode banjo::get_third_person_camera_mode() { return CameraInvertMode::InvertNone; }
banjo::CameraInvertMode banjo::get_flying_and_swimming_invert_mode() { return CameraInvertMode::InvertNone; }
banjo::CameraInvertMode banjo::get_first_person_invert_mode() { return CameraInvertMode::InvertNone; }
banjo::AnalogCamMode banjo::get_analog_cam_mode() { return AnalogCamMode::Off; }
uint32_t banjo::get_analog_cam_sensitivity() { return 50; }
banjo::NoteSavingMode banjo::get_note_saving_mode() { return NoteSavingMode::Off; }
// Added to every chunk-range limit of the level view culling (patches/required_patches.c func_800663EC), read every
// frame. 0 keeps the game's own ranges; a mod can raise it later.
uint32_t banjo::get_render_chunk_radius_count() { return 0; }
banjo::CutsceneAspectRatioMode banjo::get_cutscene_aspect_ratio_mode() {
    return static_cast<CutsceneAspectRatioMode>(nx::get(nx::Setting::CutsceneAspect));
}

// banjo_sound.h: music volume, 0..100.
int banjo::get_bgm_volume() { return nx::get(nx::Setting::BgmVolume); }

// Pumps RmlUi callbacks registered by mods (patches/ui_funcs.h); there is no UI nor mods on Switch.
extern "C" void recomp_run_ui_callbacks(uint8_t*, recomp_context*) {}
