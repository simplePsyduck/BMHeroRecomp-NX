// Switch entry point for Bomberman Hero (BMHero_nx). Mirrors src/main/main.cpp (same N64ModernRuntime API,
// same positional recomp::start) without the PC frontend (RmlUi launcher, RT64, native file dialogs, texture
// packs): the game boots straight from the ROM placed in sdmc:/switch/BMHero_nx/.
// Derived from DK64Recomp-NX's main_switch.cpp (GPL-3.0).
#include <algorithm>
#include <array>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <exception>
#include <vector>

#include <unistd.h>

#include <SDL.h>
#include <switch.h>

#include "banjo_config.h"
#include "banjo_game.h"
#include "nx_config.h"
#include "nx_controls.h"
#include "nx_paths.h"
#include "nx_renderer.h"
#include "nx_settings.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "ovl_patches.hpp"
#include "recomp_data.h"
#include "recompinput/recompinput.h"
#include "recompui/recompui.h"
#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "../patches/graphics.h"
#include "../patches/input.h"
#include "../patches/misc_funcs.h"
#include "../patches/sound.h"
#include "../patches/ui_funcs.h"

SDL_Window* bmhnx_window = nullptr;

// nvdrv keeps the bookkeeping of every GPU allocation (mesa makes one nvMapCreate per texture) in the transfer memory
// libnx hands it at startup: 8 MiB by default (a weak symbol). Measured on the console, uploads start failing with
// GL_OUT_OF_MEMORY near 5,000 live textures; GLideN64 caches up to 8,000, so a long session ended in black and noisy
// textures until a restart. 32 MiB holds about 20,000 allocations.
extern "C" u32 __nx_nv_transfermem_size;
u32 __nx_nv_transfermem_size = 32 * 1024 * 1024;

void bmhnx_input_handle_event(const SDL_Event& event);
void bmhnx_input_update();
void bmhnx_perf_start();
void bmhnx_perf_update();
void bmhnx_perf_stop();
extern "C" void bmhnx_jit_close();
extern "C" void bmhnx_window_set_size(int width, int height);  // window_switch.c
#ifdef BMHNX_REMOTE
void bmhnx_remote_start();
void bmhnx_remote_stop();
extern "C" void bmhnx_trace(const char* what);
#endif
bool bmhnx_get_input(int controller_num, uint16_t* buttons, float* x, float* y);
ultramodern::input::connected_device_info_t bmhnx_get_connected_device_info(int controller_num);
std::unique_ptr<ultramodern::renderer::RendererContext> bmhnx_create_selected_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);  // renderer_select.cpp

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
extern "C" void bmhnx_log_status();  // render_gliden64.cpp
extern "C" void bmhnx_input_log_status();  // input_switch.cpp
gpr get_entrypoint_address();
extern RspUcodeFunc n_aspMain;

// Milliseconds since the console booted, for the start-up timeline in the log (also core_switch.cpp).
extern "C" unsigned long long bmhnx_ms_since_boot() { return armTicksToNs(armGetSystemTick()) / 1000000; }

namespace {
    const std::filesystem::path DATA_DIR = BMHNX_DATA_DIR;
    std::u8string GAME_ID = u8"bmhero";  // non-const: recomp::is_rom_valid/select_rom take std::u8string&
    // librecomp's --game switch looks games up by their mod id.
    char MOD_GAME_ID[] = "bmhero";
    const std::string VERSION_STRING = "0.7.3";  // BMHeroRecomp's version_string (src/main/main.cpp)

    // The log is fully buffered in memory and written out by log_sync_thread: the game prints through
    // patches/print.c -> recomp_puts, which calls fputc once per character, so an unbuffered log made every debug
    // line cost one SD card write per character (about 200 ms a line: ROM reads and scene changes took seconds).
    constexpr size_t LOG_BUFFER_BYTES = 256 * 1024;
    char log_buffer[LOG_BUFFER_BYTES];

    // Writes the buffer out, then syncs: the SD card only learns a file's new size when it is synced, so lines
    // printed just before the process dies (svcExitProcess, abort) are lost without this. Called at every step, from
    // the terminate handler, and by log_sync_thread. The FILE lock is held only for the write, not for the (slow)
    // sync, so a game thread printing meanwhile waits for a memcpy, not for the card.
    void log_sync() {
        flockfile(stdout);
        fflush(stdout);
        funlockfile(stdout);
        fsync(fileno(stdout));
    }

    Thread log_sync_thread;
    bool log_thread_started = false;
    std::atomic<bool> log_thread_stop{false};
    void log_sync_loop(void*) {
        // A status line every 2 s for the first minute, then every 20 s.
        for (unsigned tick = 1; !log_thread_stop.load(); tick++) {
            svcSleepThread(250000000ULL);
            if (tick % (tick < 240 ? 8 : 80) == 0) {
                bmhnx_log_status();
                bmhnx_input_log_status();
            }
            // Write out what the game printed; sync to the card about once a second.
            flockfile(stdout);
            fflush(stdout);
            funlockfile(stdout);
            if (tick % 4 == 0) {
                fsync(fileno(stdout));
            }
        }
    }

    // The homebrew loader reuses this process for the next .nro (Restart) and unmaps this one's code: a thread still
    // running here would execute unmapped memory and take the whole app down. Every thread this file starts must be
    // stopped and joined before the return to the loader; the game threads are ultramodern::stop_game_threads().
    void stop_log_thread() {
        if (!log_thread_started) return;
        log_thread_stop = true;
        threadWaitForExit(&log_sync_thread);
        threadClose(&log_sync_thread);
        log_thread_started = false;
    }

    std::atomic<bool> quitting{false};

    // With the time since the console booted, to see where the start of the game goes.
    void step(const char* what) {
        printf("[BMHNX] step: %s (%llu ms)\n", what, bmhnx_ms_since_boot());
        log_sync();
    }

    [[noreturn]] void log_terminate() {
        try {
            if (auto current = std::current_exception()) std::rethrow_exception(current);
            fprintf(stderr, "[BMHNX] std::terminate without an active exception\n");
        } catch (const std::exception& e) {
            fprintf(stderr, "[BMHNX] uncaught exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "[BMHNX] uncaught non-standard exception\n");
        }
        log_sync();
        svcSleepThread(2000000000ULL);
        std::abort();
    }

    void init_logging() {
        std::filesystem::create_directories(DATA_DIR);
        socketInitializeDefault();
        if (nxlinkStdio() >= 0) {
            setvbuf(stdout, nullptr, _IOLBF, 0);  // line buffered: the game prints one character at a time
            setvbuf(stderr, nullptr, _IOLBF, 0);
            return;
        }
#ifndef BMHNX_REMOTE
        socketExit();
#endif
        // Launched from hbmenu: keep a log on the SD card instead.
        const std::string log_path = (DATA_DIR / (std::string(BMHNX_APP_NAME) + ".log")).string();
        // The previous run's log survives one launch: after a crash, relaunching would otherwise erase its trace.
        const std::string previous_path = (DATA_DIR / (std::string(BMHNX_APP_NAME) + ".prev.log")).string();
        std::remove(previous_path.c_str());
        std::rename(log_path.c_str(), previous_path.c_str());
        freopen(log_path.c_str(), "w", stdout);
        stderr = stdout;  // one handle: two descriptors on the same file overwrote each other's lines
        setvbuf(stdout, log_buffer, _IOFBF, sizeof(log_buffer));
        if (R_SUCCEEDED(threadCreate(&log_sync_thread, log_sync_loop, nullptr, nullptr, 0x4000, 0x3B, -2))) {
            threadStart(&log_sync_thread);
            log_thread_started = true;
        }
    }

    // Before the app was named BMHero_nx, everything it keeps (the ROM and its stored copy, config, saves, mods, markers)
    // was in sdmc:/switch/bmherorecomp/. Moves what is there into the new folder, once: an entry that already exists in
    // the new folder is left alone, and the old .nro and logs stay behind. The folder itself is left for the user to delete.
    void migrate_old_data_dir() {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path old_dir = BMHNX_OLD_DATA_DIR;
        if (!fs::is_directory(old_dir, ec)) {
            return;
        }
        std::vector<fs::path> entries;  // collected first: renaming while iterating can skip entries on FAT
        for (const auto& entry : fs::directory_iterator(old_dir, ec)) {
            const std::string extension = entry.path().extension().string();
            if (extension != ".nro" && extension != ".log") {
                entries.push_back(entry.path());
            }
        }
        int moved = 0;
        int kept = 0;
        for (const fs::path& from : entries) {
            const fs::path to = DATA_DIR / from.filename();
            if (fs::exists(to, ec)) {
                kept++;
                continue;
            }
            fs::rename(from, to, ec);
            if (ec) {
                printf("[BMHNX] migrate: cannot move %s (%s)\n", from.c_str(), ec.message().c_str());
                kept++;
            } else {
                moved++;
            }
        }
        if (moved > 0 || kept > 0) {
            printf("[BMHNX] migrate: %d item(s) moved from %s to %s, %d left where they were\n", moved, BMHNX_OLD_DATA_DIR,
                   BMHNX_DATA_DIR, kept);
        }
    }

    bool is_rom_extension(std::string ext) {
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        return ext == ".z64" || ext == ".n64" || ext == ".v64";
    }

    // librecomp keeps a validated, byteswapped copy as <config>/bmhero.z64. On first boot, import
    // whichever dump the user dropped next to it.
    bool ensure_rom() {
        recomp::check_all_stored_roms();
        if (recomp::is_rom_valid(GAME_ID)) {
            if (!recomp::is_rom_loaded()) {
                printf("[BMHNX] ensure_rom: loading the stored ROM -> %d\n", recomp::load_stored_rom(GAME_ID) ? 1 : 0);
            }
            return true;
        }
        std::error_code ec;
        printf("[BMHNX] ensure_rom: no stored ROM yet, scanning %s\n", DATA_DIR.c_str());
        for (const auto& entry : std::filesystem::directory_iterator(DATA_DIR, ec)) {
            printf("[BMHNX]   found %s\n", entry.path().c_str());
            if (!entry.is_regular_file() || !is_rom_extension(entry.path().extension().string())) {
                continue;
            }
            auto result = recomp::select_rom(entry.path(), GAME_ID);
            printf("[BMHNX] ROM %s -> validation %d\n", entry.path().c_str(), static_cast<int>(result));
            if (result == recomp::RomValidationError::Good) {
                return true;
            }
        }
        recompui::message_box(
            "No valid Bomberman Hero ROM found.\n"
            "Copy your NTSC-U (USA) 1.0 dump (.z64/.n64/.v64) to " BMHNX_DATA_DIR "/");
        return false;
    }

    // Original framerate. In 16:9, Expand + Full HUD, as on PC: the game's patches size their UI for the 16:9
    // window via recomp_get_target_aspect_ratio, and GLideN64's adjust mode lays it out like RT64.
    void set_switch_graphics_config() {
        ultramodern::renderer::GraphicsConfig config{};
        config.developer_mode = false;
        config.res_option = ultramodern::renderer::Resolution::Original;
        config.wm_option = ultramodern::renderer::WindowMode::Fullscreen;
        config.hr_option = ultramodern::renderer::HUDRatioMode::Full;
        config.api_option = ultramodern::renderer::GraphicsApi::Auto;
        config.ar_option = nx::widescreen() ? ultramodern::renderer::AspectRatio::Expand
                                            : ultramodern::renderer::AspectRatio::Original;
        config.msaa_option = ultramodern::renderer::Antialiasing::None;
        config.rr_option = ultramodern::renderer::RefreshRate::Original;
        config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off;
        config.rr_manual_value = 30;
        config.ds_option = 1;
        ultramodern::renderer::set_graphics_config(config);
    }

    ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0) {
            fprintf(stderr, "[BMHNX] SDL_Init failed: %s\n", SDL_GetError());
        }
        return {};
    }

    ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
        step("create window");
        bmhnx_window_set_size(nx::screen_width(), nx::screen_height());
        bmhnx_window = SDL_CreateWindow(BMHNX_DISPLAY_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         nx::screen_width(), nx::screen_height(), SDL_WINDOW_OPENGL);
        if (bmhnx_window == nullptr) {
            fprintf(stderr, "[BMHNX] SDL_CreateWindow failed: %s\n", SDL_GetError());
        }
        step("window created");
        return bmhnx_window;
    }

    // ultramodern only runs the game (VI/audio events, tasks) once recomp::start_game has been called; until then it
    // shows its dummy VI. On PC the launcher's Start button calls it; here the first pass of the main loop does.
    void start_game_once() {
        static bool started = false;
        if (started) return;
        // Wait for ultramodern's dummy VI to have run once (it sets the VI mode, which stays until the game's own
        // osViSetMode). Starting before that leaves the VI state without a mode and the VI thread's update_vi
        // dereferences it (events.cpp: common_regs->hStart, far=0x1c). On PC the launcher is on screen long
        // enough for this to be true by the time Start is pressed.
        const auto* vi = ultramodern::renderer::get_vi_regs();
        if (vi->VI_ORIGIN_REG == 0 || vi->VI_STATUS_REG == 0) return;
        started = true;
        step("start game");
        recomp::start_game(GAME_ID, "");
        step("start game requested");
    }

    void update_gfx(void*) {
        start_game_once();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quitting = true;  // lets a game thread halted by the pause menu go
                ultramodern::quit();
            }
            bmhnx_input_handle_event(event);
        }
        bmhnx_input_update();
        bmhnx_perf_update();
    }

    RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
        if (task->t.type == M_AUDTASK) {
            return n_aspMain;
        }
        fprintf(stderr, "[BMHNX] Unknown RSP task type %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

// Audio path copied from src/main/main.cpp (SDL queue + resampler), volume fixed at 100%.
namespace {
    SDL_AudioCVT audio_convert;
    SDL_AudioDeviceID audio_device = 0;
    uint32_t sample_rate = 48000;
    uint32_t output_sample_rate = 48000;
    constexpr uint32_t input_channels = 2;
    uint32_t output_channels = 2;
    // Frames duplicated across chunks so the resampler interpolates over the seam.
    constexpr uint32_t duplicated_input_frames = 4;
    uint32_t discarded_output_frames;
    constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);
    // Queue depth past which whole chunks are dropped (see queue_samples).
    constexpr uint64_t MAX_QUEUED_AUDIO_US = 250000;
    constexpr Uint16 AUDIO_BUFFER_FRAMES = 1024;

    void queue_samples(int16_t* audio_data, size_t sample_count) {
        static std::vector<float> swap_buffer;
        static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

        size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
        size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
        if (max_sample_count > swap_buffer.size()) {
            swap_buffer.resize(max_sample_count);
        }
        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            swap_buffer[i] = duplicated_sample_buffer[i];
        }
        // NX Options' Master Volume scales everything the game mixed (music and effects have their own settings).
        const float gain = (0.5f / 32768.0f) * static_cast<float>(nx::get(nx::Setting::MasterVolume)) / 100.0f;
        // Swap the channels back: the rdram address xor swaps adjacent 16-bit samples.
        for (size_t i = 0; i < sample_count; i += input_channels) {
            swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * gain;
            swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * gain;
        }
        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
        }

        audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
        audio_convert.len = (sample_count + duplicated_input_frames * input_channels) * sizeof(swap_buffer[0]);
        if (SDL_ConvertAudio(&audio_convert) < 0) {
            throw std::runtime_error(std::string("SDL_ConvertAudio failed: ") + SDL_GetError());
        }

        // The SDL queue holds converted frames at the output rate; upstream divides by the game's rate
        // (22050 Hz for DK64), overestimating latency 2.2x and thinning audio (high pitch on Switch).
        uint64_t queued_us = uint64_t(SDL_GetQueuedAudioSize(audio_device)) / bytes_per_frame * 1000000 / output_sample_rate;
        uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(swap_buffer[0]);
        float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

        // Upstream thins each chunk by 2^(queued/100 ms), which shifts the pitch up; on Switch the
        // queue sits around 110 ms on its own, so that guard fired constantly. Drop whole chunks
        // only past a real runaway instead: a short gap, never a pitch change.
        if (queued_us > MAX_QUEUED_AUDIO_US) {
            return;
        }
        SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
    }

    size_t get_frames_remaining() {
        constexpr float buffer_offset_frames = 1.0f;
        uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);
        buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;
        uint32_t frames_per_vi = sample_rate / 60;
        if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
            buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
        }
        else {
            buffered_byte_count = 0;
        }
        return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
    }

    void update_audio_converter() {
        if (SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate) < 0) {
            throw std::runtime_error(std::string("SDL_BuildAudioCVT failed: ") + SDL_GetError());
        }
        discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
    }

    void set_frequency(uint32_t freq) {
        printf("[BMHNX] audio frequency set to %u Hz\n", freq);
        sample_rate = freq;
        update_audio_converter();
    }

    bool reset_audio(uint32_t output_freq) {
        SDL_AudioSpec spec_desired{};
        spec_desired.freq = static_cast<int>(output_freq);
        spec_desired.format = AUDIO_F32;
        spec_desired.channels = static_cast<Uint8>(output_channels);
        // Upstream asks for 256-frame buffers to keep PC latency low. SDL's Switch backend releases one
        // buffer per ~10 ms audio service tick, which capped playback at 25.6k of 48k frames/s; 1024
        // frames (21 ms) leaves ample headroom.
        spec_desired.samples = AUDIO_BUFFER_FRAMES;
        audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
        if (audio_device == 0) {
            fprintf(stderr, "[BMHNX] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_PauseAudioDevice(audio_device, 0);
        output_sample_rate = output_freq;
        update_audio_converter();
        return true;
    }
}

#define REGISTER_FUNC(name) recomp::overlays::register_base_export(#name, name)

// The .nro's path, for Restart Game (nx_menu.cpp).
const char* bmhnx_program_path = nullptr;
const char* bmhnx_mod_game_id = "bmhero";  // the id of the game entry's mod_game_id, for nx_mods.cpp

extern "C" bool bmhnx_is_quitting() {
    return quitting.load();
}

int main(int argc, char** argv) {
#ifdef BMHNX_REMOTE
    bmhnx_trace("main: start");
#endif
    if (argc > 0) bmhnx_program_path = argv[0];
    std::set_terminate(log_terminate);
    init_logging();
    // The build's date and time, so a log tells which .nro produced it (and the menu build from an older one).
    printf("[BMHNX] Bomberman Hero (" BMHNX_APP_NAME ") %s for Switch, built %s %s, with pause menu, applet type %d (%llu ms)\n",
           VERSION_STRING.c_str(), __DATE__, __TIME__, static_cast<int>(appletGetAppletType()), bmhnx_ms_since_boot());
    migrate_old_data_dir();
    nx::config::load();
    bmhnx_perf_start();
#ifdef BMHNX_REMOTE
    bmhnx_remote_start();
#endif

    recomp::Version project_version{};
    recomp::Version::from_string(VERSION_STRING, project_version);

    step("audio init");
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!reset_audio(48000)) {
        return EXIT_FAILURE;
    }

    step("register game");
    recomp::register_config_path(DATA_DIR);

    // Same entry as BMHeroRecomp's supported_games (src/main/main.cpp); the ROM is read as is, no decompression.
    recomp::GameEntry game{
        .rom_hash = 0x89134C0E1F16E0F8ULL,
        .internal_name = "BOMBERMAN HERO",
        .display_name = "Bomberman Hero",
        .game_id = GAME_ID,
        .mod_game_id = MOD_GAME_ID,
        .save_type = recomp::SaveType::Eep4k,
        .thumbnail_bytes = std::span<const char>{},
        .is_enabled = false,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
        .on_init_callback = banjo::bk_on_init,
    };
    recomp::register_game(game);

    REGISTER_FUNC(recomp_get_window_resolution);
    REGISTER_FUNC(recomp_get_target_aspect_ratio);
    REGISTER_FUNC(recomp_get_target_framerate);
    REGISTER_FUNC(recomp_get_cutscene_aspect_ratio);
    REGISTER_FUNC(recomp_get_analog_cam_enabled);
    REGISTER_FUNC(recomp_get_right_analog_inputs);
    REGISTER_FUNC(recomp_get_bgm_volume);
    REGISTER_FUNC(recomp_get_inverted_axes);
    REGISTER_FUNC(recomp_get_analog_inverted_axes);
    recomputil::register_data_api_exports();

    banjo::register_bk_overlays();
    banjo::register_bk_patches();
    // Extensions for two object types, as in BMHeroRecomp's main.cpp.
    recomputil::init_extended_object_data(2);
    banjo::init_config();
    set_switch_graphics_config();
    step("rom check");

    if (!ensure_rom()) {
        return EXIT_FAILURE;
    }

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };
    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = bmhnx_create_selected_render_context,
    };
    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };
    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = recompinput::poll_inputs,
        .get_input = bmhnx_get_input,
        .set_rumble = recompinput::set_rumble,
        .get_connected_device_info = bmhnx_get_connected_device_info,
    };
    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };
    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = nullptr,
    };

    ultramodern::events::callbacks_t events_callbacks{
        .vi_callback = recompinput::update_rumble,
        .gfx_init_callback = nullptr,
    };
    (void)argc;
    (void)argv;

    step("recomp::start");
    // Positional, exactly as src/main/main.cpp calls it. The empty window handle is filled in by create_window.
    recomp::start(
        project_version,
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        events_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );
    step("recomp::start returned");
#ifdef BMHNX_REMOTE
    bmhnx_remote_stop();
#endif
    // The homebrew loader reuses this process: threads left running would execute unmapped code, and their stacks
    // (like the mods' code memory) would keep pages of the heap the next .nro gets.
    const int threads_left = ultramodern::stop_game_threads();
    printf("[BMHNX] game threads stopped, %d left\n", threads_left);
    bmhnx_jit_close();
    bmhnx_perf_stop();
    step("SDL_Quit");
    SDL_Quit();
    socketExit();
    step("back to the loader");
    // Everything that has to stop is stopped above; the loader reclaims the rest. _exit skips the global C++
    // destructors, which may wait on threads that are gone, and still runs libnx's cleanup before returning to the
    // loader (which then starts the .nro again after Restart Game).
    stop_log_thread();
    fflush(nullptr);
    fsync(fileno(stdout));
#ifdef BMHNX_REMOTE
    bmhnx_trace("main: _exit");
#endif
    if (threads_left > 0) {
        // Going back to the loader would crash: end the whole process instead (no restart).
        svcExitProcess();
    }
    _exit(EXIT_SUCCESS);
}
