// Pause menu for the Switch build. A hotkey (default MINUS, rebindable in the menu or controls.ini) opens it over the
// game: Continue, Controllers (rebind every input and the hotkey itself), Video Settings (render quality, 4:3
// letterbox), Mods (switch the mods in the mods folder on or off), Reset Game (start over, with the mods as chosen)
// and Quit.
//
// While it is open the game is halted: the game thread blocks at its next controller poll
// (bmhnx_menu_pause_point, called from input_switch.cpp), so no new frames are produced, and the renderer keeps
// presenting the last one so the menu is drawn over it on every refresh (render_gliden64.cpp update_screen).
//
// Drawn with Dear ImGui (the version RT64 pins) on GLideN64's GL context, right before each swap, in 1280x720
// units. The widgets are DK64Recomp-NX's (nx_menu_widgets.*, GPL-3.0); the window itself takes no input: this file
// moves the focus from the pad.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <SDL.h>
#include <switch.h>

#include <glad/glad.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include "nx_controls.h"
#include "nx_menu_widgets.h"
#include "nx_mods.h"
#include "nx_pad.h"
#include "nx_paths.h"
#include "nx_settings.h"

extern const char* bmhnx_program_path;   // main_switch.cpp: argv[0], for Restart
extern "C" bool bmhnx_is_quitting();      // main_switch.cpp

namespace {
    using nx::Setting;
    using nx::menu::Theme;

    constexpr float SCREEN_W = 1280.0f;
    constexpr float SCREEN_H = 720.0f;
    constexpr float FONT_PX = 24.0f;
    constexpr float PANEL_ALPHA = 0.80f;          // the game shows through the panel
    constexpr int DIM_ALPHA = 105;                // and through the dimmed screen behind it
    constexpr uint64_t REPEAT_DELAY_MS = 350;
    constexpr uint64_t REPEAT_INTERVAL_MS = 90;
    constexpr uint64_t CAPTURE_TIMEOUT_MS = 8000;
    constexpr float STICK_PRESS = 0.6f;
    constexpr float STICK_RELEASE = 0.35f;
    constexpr uint32_t DIRECTIONS = NX_UP | NX_DOWN | NX_LEFT | NX_RIGHT;
    constexpr uint32_t MENU_BUTTONS = NX_A | NX_B | NX_X | NX_Y | DIRECTIONS;

    enum class Screen { Main, Controllers, Video, Mods };
    enum class Modal { None, Capture, ConfirmReset, ConfirmGameReset, NoRestart };
    enum class ImGuiState { NotStarted, Ready, Failed };

    // The items of each screen.
    enum MainItem { MAIN_CONTINUE, MAIN_CONTROLLERS, MAIN_VIDEO, MAIN_MODS, MAIN_RESET_GAME, MAIN_QUIT, MAIN_COUNT };
    // Controllers: the hotkey, the 14 N64 inputs (CONTROL_ORDER), then Reset and Back.
    constexpr int CONTROL_HOTKEY = 0;
    constexpr int CONTROL_FIRST_INPUT = 1;
    constexpr int CONTROL_RESET = CONTROL_FIRST_INPUT + nx::controls::INPUT_COUNT;
    constexpr int CONTROL_BACK = CONTROL_RESET + 1;
    constexpr int CONTROL_COUNT = CONTROL_BACK + 1;
    // The N64 inputs in the order players think of them (indices into the N64 button table).
    constexpr int CONTROL_ORDER[nx::controls::INPUT_COUNT] = {0, 1, 2, 3, 8, 9, 4, 5, 6, 7, 10, 11, 12, 13};
    // Video: two settings, Apply and Restart, Back.
    constexpr Setting VIDEO_SETTINGS[] = {Setting::Resolution, Setting::AspectRatio};
    constexpr int VIDEO_SETTING_COUNT = 2;
    constexpr int VIDEO_APPLY = VIDEO_SETTING_COUNT;
    constexpr int VIDEO_BACK = VIDEO_APPLY + 1;
    constexpr int VIDEO_COUNT = VIDEO_BACK + 1;

    std::atomic_bool menu_open{false};
    std::atomic_bool owns_pad{false};   // the game reads a neutral pad until every button is released after closing
    ImGuiState imgui_state = ImGuiState::NotStarted;
    Screen screen = Screen::Main;
    Modal modal = Modal::None;
    int selected[4] = {0, 0, 0, 0};
    int mods_top = 0;                   // first visible item of the Mods list
    int controls_top = 0;               // first visible item of the Controllers list
    bool release_pending = false;       // closed, waiting for the pad to be released
    bool needs_release = false;         // ignore actions until every button is up
    bool hotkey_was_held = false;
    bool settings_changed = false;
    uint32_t previous_buttons = 0;
    uint32_t stick_direction = 0;
    uint64_t repeat_at = 0;
    uint64_t last_frame_ms = 0;
    int capture_row = 0;
    bool capture_add = false;
    uint64_t capture_started = 0;
    NxPad capture_base;
    PlFontData shared_font;
    bool have_shared_font = false;

    const Theme& look() { return nx::menu::theme(0); }

    // ---- Opening and closing ----

    void save_changes() {
        if (settings_changed) {
            nx::save();
            settings_changed = false;
        }
    }

    // The atlas is built at the font's pixel size: sharper than scaling one size up or down.
    void load_font(float size) {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        ImFontConfig config;
        if (have_shared_font) {
            config.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF(shared_font.address, static_cast<int>(shared_font.size), size, &config);
        } else {
            config.SizePixels = size;
            io.Fonts->AddFontDefault(&config);
        }
    }

    bool start_imgui() {
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) == 0) {
            std::printf("[BMHNX] menu: GL functions not found\n");
            return false;
        }
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        // The console's own font; pl stays initialized because the font lives in its shared memory.
        have_shared_font = R_SUCCEEDED(plInitialize(PlServiceType_User)) &&
                           R_SUCCEEDED(plGetSharedFontByType(&shared_font, PlSharedFontType_Standard));
        load_font(FONT_PX);
        // GLideN64 creates a 3.3 core profile context.
        return ImGui_ImplOpenGL3_Init("#version 330 core");
    }

    void open_menu() {
        if (imgui_state == ImGuiState::NotStarted) {
            imgui_state = start_imgui() ? ImGuiState::Ready : ImGuiState::Failed;
            std::printf("[BMHNX] menu: ImGui %s\n", imgui_state == ImGuiState::Ready ? "ready" : "failed to start");
        }
        if (imgui_state != ImGuiState::Ready) {
            return;  // a menu nobody can see must not halt the game
        }
        screen = Screen::Main;
        modal = Modal::None;
        selected[0] = 0;
        release_pending = false;
        needs_release = true;   // the hotkey press itself is still down
        owns_pad = true;
        menu_open = true;       // the game thread stops at its next controller poll
        std::printf("[BMHNX] menu: opened\n");
    }

    void close_menu() {
        modal = Modal::None;
        save_changes();
        menu_open = false;      // releases the game thread
        release_pending = true;
        std::printf("[BMHNX] menu: closed\n");
    }

    // Quit, or Restart: the homebrew loader then starts the .nro again.
    void leave_game(bool restart) {
        save_changes();
        if (restart) {
            const Result rc = envSetNextLoad(bmhnx_program_path, bmhnx_program_path);
            std::printf("[BMHNX] restart: next load %s (rc 0x%x)\n", bmhnx_program_path, rc);
        }
        SDL_Event quit{};
        quit.type = SDL_QUIT;
        SDL_PushEvent(&quit);
    }

    bool can_restart() { return bmhnx_program_path != nullptr && envHasNextLoad(); }

    // ---- Input ----

    // The left stick acts as the D-pad along its dominant axis only (a diagonal would move and change a value at
    // once), pressed past STICK_PRESS and released below STICK_RELEASE.
    uint32_t menu_buttons(const NxPad& pad) {
        const float x = std::fabs(pad.lx);
        const float y = std::fabs(pad.ly);
        const float magnitude = std::max(x, y);
        if (magnitude < STICK_RELEASE) {
            stick_direction = 0;
        } else if (magnitude > STICK_PRESS || stick_direction != 0) {
            if (x > y) {
                stick_direction = pad.lx > 0 ? NX_RIGHT : NX_LEFT;
            } else {
                stick_direction = pad.ly > 0 ? NX_UP : NX_DOWN;
            }
        }
        return (pad.buttons & MENU_BUTTONS) | stick_direction;
    }

    int item_count() {
        switch (screen) {
        case Screen::Main: return MAIN_COUNT;
        case Screen::Controllers: return CONTROL_COUNT;
        case Screen::Video: return VIDEO_COUNT;
        case Screen::Mods: return nx::mods::count() + 2;  // the mods, Reset Game to Apply, Back
        }
        return 1;
    }

    int& cursor() { return selected[static_cast<int>(screen)]; }

    void move_cursor(int delta) {
        const int count = item_count();
        cursor() = (cursor() + delta + count) % count;
    }

    // The binding list row (N64 input or hotkey) of a Controllers item, or -1 for Reset and Back.
    int control_row(int item) {
        if (item == CONTROL_HOTKEY) return nx::controls::HOTKEY_ROW;
        if (item >= CONTROL_FIRST_INPUT && item < CONTROL_RESET) return CONTROL_ORDER[item - CONTROL_FIRST_INPUT];
        return -1;
    }

    void go_to(Screen next) {
        if (screen == Screen::Video) save_changes();
        if (next == Screen::Mods) nx::mods::refresh();  // reads the mods folder's list as the runtime has it
        screen = next;
    }

    // Reset Game and Apply: start the whole game again through the loader, which loads the mods as chosen.
    void request_game_reset() {
        if (can_restart()) {
            leave_game(true);
        } else {
            modal = Modal::NoRestart;
        }
    }

    void begin_capture(int row, bool add, const NxPad& pad, uint64_t now) {
        capture_row = row;
        capture_add = add;
        capture_started = now;
        capture_base = pad;
        modal = Modal::Capture;
    }

    // Left or right on a choice: no wrap, so the ends stay reachable.
    void change_setting(Setting setting, int direction, bool wrap) {
        const nx::SettingInfo& info = nx::info(setting);
        const int count = static_cast<int>(info.choices.size());
        int value = nx::get(setting) + direction;
        if (wrap) {
            value = (value + count) % count;
        } else {
            value = std::clamp(value, 0, count - 1);
        }
        if (value != nx::get(setting)) {
            nx::set(setting, value);
            settings_changed = true;
        }
    }

    void handle_main(uint32_t pressed, uint32_t actions) {
        if (actions & NX_UP) move_cursor(-1);
        if (actions & NX_DOWN) move_cursor(1);
        if (pressed & NX_B) {
            close_menu();
        } else if (pressed & NX_A) {
            switch (cursor()) {
            case MAIN_CONTINUE: close_menu(); break;
            case MAIN_CONTROLLERS: go_to(Screen::Controllers); break;
            case MAIN_VIDEO: go_to(Screen::Video); break;
            case MAIN_MODS: go_to(Screen::Mods); break;
            case MAIN_RESET_GAME: modal = Modal::ConfirmGameReset; break;
            case MAIN_QUIT: leave_game(false); break;
            }
        }
    }

    void handle_controllers(uint32_t pressed, uint32_t actions, const NxPad& pad, uint64_t now) {
        if (actions & NX_UP) move_cursor(-1);
        if (actions & NX_DOWN) move_cursor(1);
        const int item = cursor();
        const int row = control_row(item);
        if (pressed & NX_B) {
            go_to(Screen::Main);
        } else if (pressed & NX_A) {
            if (row >= 0) begin_capture(row, false, pad, now);
            else if (item == CONTROL_RESET) modal = Modal::ConfirmReset;
            else go_to(Screen::Main);
        } else if ((pressed & NX_Y) && row >= 0) {
            begin_capture(row, true, pad, now);
        } else if ((pressed & NX_X) && row >= 0 && row != nx::controls::HOTKEY_ROW) {
            nx::controls::clear(row);   // the hotkey cannot be cleared: without it the menu could not be opened
        }
    }

    void handle_video(uint32_t pressed, uint32_t actions) {
        if (actions & NX_UP) move_cursor(-1);
        if (actions & NX_DOWN) move_cursor(1);
        const int item = cursor();
        if (item < VIDEO_SETTING_COUNT) {
            if (actions & NX_LEFT) change_setting(VIDEO_SETTINGS[item], -1, false);
            if (actions & NX_RIGHT) change_setting(VIDEO_SETTINGS[item], 1, false);
            if (pressed & NX_A) change_setting(VIDEO_SETTINGS[item], 1, true);
        } else if (item == VIDEO_APPLY && (pressed & NX_A)) {
            if (!nx::restart_pending()) return;
            request_game_reset();
        } else if (item == VIDEO_BACK && (pressed & NX_A)) {
            go_to(Screen::Main);
        }
        if (pressed & NX_B) go_to(Screen::Main);
    }

    void handle_mods(uint32_t pressed, uint32_t actions) {
        if (actions & NX_UP) move_cursor(-1);
        if (actions & NX_DOWN) move_cursor(1);
        const int item = cursor();
        const int mod_count = nx::mods::count();
        if (item < mod_count) {
            // A, or either side of the D-pad, flips the mod.
            if ((pressed & NX_A) || (actions & (NX_LEFT | NX_RIGHT))) nx::mods::toggle(item);
        } else if (item == mod_count && (pressed & NX_A)) {
            if (nx::mods::reset_needed()) modal = Modal::ConfirmGameReset;
        } else if (item == mod_count + 1 && (pressed & NX_A)) {
            go_to(Screen::Main);
        }
        if (pressed & NX_B) go_to(Screen::Main);
    }

    void handle_modal(uint32_t pressed) {
        if (modal == Modal::ConfirmReset) {
            if (pressed & NX_A) {
                nx::controls::reset_defaults();
                modal = Modal::None;
            } else if (pressed & NX_B) {
                modal = Modal::None;
            }
        } else if (modal == Modal::ConfirmGameReset) {
            if (pressed & NX_A) {
                modal = Modal::None;
                request_game_reset();
            } else if (pressed & NX_B) {
                modal = Modal::None;
            }
        } else if (modal == Modal::NoRestart) {
            if (pressed & (NX_A | NX_B)) modal = Modal::None;
        }
    }

    void update_input(uint64_t now) {
        NxPad pad;
        bmhnx_read_pad(&pad);
        const uint32_t held = menu_buttons(pad);
        const uint32_t pressed = held & ~previous_buttons;
        previous_buttons = held;

        // The hotkey toggles on its rising edge, except while a binding waits for a press.
        const bool hotkey_held = nx::controls::hotkey_held(pad);
        const bool hotkey_pressed = hotkey_held && !hotkey_was_held;
        hotkey_was_held = hotkey_held;
        if (hotkey_pressed && !(menu_open.load() && modal == Modal::Capture) && imgui_state != ImGuiState::Failed) {
            if (menu_open.load()) close_menu(); else open_menu();
            repeat_at = now + REPEAT_DELAY_MS;
            return;
        }

        if (!menu_open.load()) {
            // The game sees a neutral pad until the buttons that closed the menu are up.
            if (release_pending && pad.buttons == 0 && std::max(std::fabs(pad.lx), std::fabs(pad.ly)) < STICK_RELEASE) {
                release_pending = false;
                owns_pad = false;
            }
            return;
        }

        if (modal == Modal::Capture) {
            if (now - capture_started > CAPTURE_TIMEOUT_MS) {
                modal = Modal::None;
                needs_release = true;
            } else if (nx::controls::capture(capture_row, pad, &capture_base, capture_add)) {
                modal = Modal::None;
                needs_release = true;  // the caught press must not also act on the menu
            }
            return;
        }
        if (needs_release) {
            if (held == 0 && pad.buttons == 0) needs_release = false;
            return;
        }

        uint32_t actions = pressed;
        if ((pressed & DIRECTIONS) != 0) {
            repeat_at = now + REPEAT_DELAY_MS;
        } else if ((held & DIRECTIONS) != 0 && now >= repeat_at) {
            actions |= held & DIRECTIONS;
            repeat_at = now + REPEAT_INTERVAL_MS;
        }
        if (modal != Modal::None) {
            handle_modal(pressed);
            return;
        }
        switch (screen) {
        case Screen::Main: handle_main(pressed, actions); break;
        case Screen::Controllers: handle_controllers(pressed, actions, pad, now); break;
        case Screen::Video: handle_video(pressed, actions); break;
        case Screen::Mods: handle_mods(pressed, actions); break;
        }
    }

    // ---- Drawing ----

    constexpr float PANEL_PAD = 28.0f;

    // The dimmed screen and the translucent panel; returns the panel's top-left corner.
    ImVec2 draw_panel(float width, float height, const Theme& theme) {
        ImDrawList* back = ImGui::GetBackgroundDrawList();
        back->AddRectFilled({0.0f, 0.0f}, {SCREEN_W, SCREEN_H}, IM_COL32(0, 0, 0, DIM_ALPHA));
        const ImVec2 min{std::floor((SCREEN_W - width) / 2), std::floor((SCREEN_H - height) / 2)};
        ImVec4 color = theme.panel;
        color.w = PANEL_ALPHA;
        back->AddRectFilled(min, {min.x + width, min.y + height}, ImGui::ColorConvertFloat4ToU32(color), 12.0f);
        return min;
    }

    void centered(ImDrawList* draw, float center_x, float y, const char* text, ImU32 color) {
        draw->AddText({std::floor(center_x - ImGui::CalcTextSize(text).x / 2), y}, color, text);
    }

    void draw_title(ImDrawList* draw, ImVec2 origin, float width, const char* title, const char* subtitle,
                    const Theme& theme) {
        centered(draw, origin.x + width / 2, origin.y + 20.0f, title, theme.accent);
        if (subtitle != nullptr) {
            centered(draw, origin.x + width / 2, origin.y + 20.0f + ImGui::GetTextLineHeightWithSpacing(), subtitle,
                     theme.text_dim);
        }
    }

    void draw_footer(ImDrawList* draw, ImVec2 origin, float width, float height, const char* text, const Theme& theme) {
        centered(draw, origin.x + width / 2, origin.y + height - 18.0f - ImGui::GetTextLineHeight(), text, theme.text_dim);
    }

    void draw_main(const Theme& theme) {
        static const char* LABELS[MAIN_COUNT] = {"Continue", "Controllers", "Video Settings", "Mods", "Reset Game", "Quit"};
        constexpr float WIDTH = 520.0f;
        constexpr float ROW = 52.0f;
        constexpr float GAP = 10.0f;
        constexpr float TOP = 96.0f;
        const float height = TOP + static_cast<float>(MAIN_COUNT) * (ROW + GAP) + 50.0f;
        const ImVec2 origin = draw_panel(WIDTH, height, theme);
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        draw_title(draw, origin, WIDTH, BMHNX_DISPLAY_NAME, "Paused", theme);
        for (int i = 0; i < MAIN_COUNT; i++) {
            const float y = origin.y + TOP + i * (ROW + GAP);
            nx::menu::push_button(draw, {origin.x + PANEL_PAD, y}, {origin.x + WIDTH - PANEL_PAD, y + ROW}, LABELS[i],
                                  i == cursor(), theme);
        }
        char hint[96];
        std::snprintf(hint, sizeof(hint), "A  Select      B or %s  Continue", nx::controls::describe(nx::controls::HOTKEY_ROW).c_str());
        draw_footer(draw, origin, WIDTH, height, hint, theme);
    }

    void draw_controllers(const Theme& theme) {
        constexpr float WIDTH = 800.0f;
        constexpr float HEIGHT = 660.0f;
        constexpr float TOP = 92.0f;
        constexpr float ROW = 38.0f;
        constexpr float VALUE_WIDTH = 330.0f;
        const float list_height = HEIGHT - TOP - 64.0f;
        const int visible = static_cast<int>(list_height / ROW);
        controls_top = std::clamp(controls_top, 0, CONTROL_COUNT - visible);
        if (cursor() < controls_top) controls_top = cursor();
        if (cursor() >= controls_top + visible) controls_top = cursor() - visible + 1;

        const ImVec2 origin = draw_panel(WIDTH, HEIGHT, theme);
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        draw_title(draw, origin, WIDTH, "Controllers", "A  Rebind     Y  Add another     X  Unbind", theme);
        for (int i = 0; i < visible; i++) {
            const int item = controls_top + i;
            if (item >= CONTROL_COUNT) break;
            const float y = origin.y + TOP + i * ROW;
            const bool focused = item == cursor();
            const float left = origin.x + PANEL_PAD;
            const float right = origin.x + WIDTH - PANEL_PAD;
            const int row = control_row(item);
            if (row >= 0) {
                draw->AddText({left, y + 5.0f}, theme.text, nx::controls::row_label(row));
                nx::menu::push_button(draw, {right - VALUE_WIDTH, y}, {right, y + ROW - 4.0f},
                                      nx::controls::describe(row).c_str(), focused, theme);
            } else {
                nx::menu::push_button(draw, {left, y}, {right, y + ROW - 4.0f},
                                      item == CONTROL_RESET ? "Reset to Defaults" : "Back", focused, theme);
            }
        }
        // Where the visible window sits in the list.
        if (controls_top > 0) {
            draw->AddText({origin.x + WIDTH - PANEL_PAD - 12.0f, origin.y + TOP - 24.0f}, theme.text_dim, "^");
        }
        if (controls_top + visible < CONTROL_COUNT) {
            draw->AddText({origin.x + WIDTH - PANEL_PAD - 12.0f, origin.y + HEIGHT - 52.0f}, theme.text_dim, "v");
        }
        draw_footer(draw, origin, WIDTH, HEIGHT, "B  Back     Changes are saved as you make them", theme);
    }

    void draw_video(const Theme& theme) {
        constexpr float WIDTH = 860.0f;
        constexpr float HEIGHT = 560.0f;
        constexpr float TOP = 92.0f;
        constexpr float ROW = 52.0f;
        constexpr float GAP = 8.0f;
        constexpr float VALUE_WIDTH = 380.0f;
        const ImVec2 origin = draw_panel(WIDTH, HEIGHT, theme);
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        draw_title(draw, origin, WIDTH, "Video Settings", "Left / Right  Change     * applies after a restart", theme);
        const float left = origin.x + PANEL_PAD;
        const float right = origin.x + WIDTH - PANEL_PAD;
        for (int i = 0; i < VIDEO_COUNT; i++) {
            const float y = origin.y + TOP + i * (ROW + GAP);
            const bool focused = i == cursor();
            if (i < VIDEO_SETTING_COUNT) {
                const nx::SettingInfo& info = nx::info(VIDEO_SETTINGS[i]);
                const int value = nx::get(VIDEO_SETTINGS[i]);
                char label[64];
                std::snprintf(label, sizeof(label), "%s%s", info.label, info.needs_restart ? " *" : "");
                char text[96];
                std::snprintf(text, sizeof(text), "<  %s  >", info.choices.begin()[value]);
                draw->AddText({left, y + 12.0f}, theme.text, label);
                nx::menu::push_button(draw, {right - VALUE_WIDTH, y}, {right, y + ROW}, text, focused, theme);
            } else {
                const char* text = i == VIDEO_APPLY ? (nx::restart_pending() ? "Apply Changes and Restart" : "No Changes to Apply")
                                                    : "Back";
                nx::menu::push_button(draw, {left, y}, {right, y + ROW}, text, focused, theme);
            }
        }
        // What the focused setting does.
        if (cursor() < VIDEO_SETTING_COUNT) {
            const nx::SettingInfo& info = nx::info(VIDEO_SETTINGS[cursor()]);
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {left, origin.y + TOP + VIDEO_COUNT * (ROW + GAP) + 6.0f},
                          theme.text_dim, info.description, nullptr, WIDTH - 2 * PANEL_PAD);
        }
        draw_footer(draw, origin, WIDTH, HEIGHT, "A  Change / Select     B  Back", theme);
    }

    // Text cut to fit a width, with "..." where it was shortened.
    std::string fit_text(const std::string& text, float max_width) {
        if (ImGui::CalcTextSize(text.c_str()).x <= max_width) return text;
        std::string cut = text;
        while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > max_width) cut.pop_back();
        return cut + "...";
    }

    void draw_mods(const Theme& theme) {
        constexpr float WIDTH = 820.0f;
        constexpr float HEIGHT = 640.0f;
        constexpr float TOP = 112.0f;
        constexpr float ROW = 46.0f;
        constexpr float VALUE_WIDTH = 230.0f;
        const int mod_count = nx::mods::count();
        const int item_total = mod_count + 2;
        const float list_height = HEIGHT - TOP - 64.0f;
        const int visible = static_cast<int>(list_height / ROW);
        mods_top = std::clamp(mods_top, 0, std::max(0, item_total - visible));
        if (cursor() < mods_top) mods_top = cursor();
        if (cursor() >= mods_top + visible) mods_top = cursor() - visible + 1;

        const ImVec2 origin = draw_panel(WIDTH, HEIGHT, theme);
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const std::string folder = fit_text(nx::mods::folder(), WIDTH - 2 * PANEL_PAD);
        draw_title(draw, origin, WIDTH, "Mods", folder.c_str(), theme);
        const float left = origin.x + PANEL_PAD;
        const float right = origin.x + WIDTH - PANEL_PAD;
        if (mod_count == 0) {
            // Nothing to list: say where mods go (the Reset and Back rows are still drawn below).
            draw->AddText({left, origin.y + TOP}, theme.text_dim, "No mods found. Put mod files (.nrm) in the folder above,");
            draw->AddText({left, origin.y + TOP + ImGui::GetTextLineHeightWithSpacing()}, theme.text_dim,
                          "then use Reset Game to load them.");
        }
        const float list_top = origin.y + TOP + (mod_count == 0 ? 2 * ImGui::GetTextLineHeightWithSpacing() + 8.0f : 0.0f);
        for (int i = 0; i < visible; i++) {
            const int item = mods_top + i;
            if (item >= item_total) break;
            const float y = list_top + i * ROW;
            const bool focused = item == cursor();
            if (item < mod_count) {
                const std::string name = fit_text(nx::mods::name(item), right - left - VALUE_WIDTH - 24.0f);
                const char* state = nx::mods::chosen(item) ? "On" : nx::mods::needed(item) ? "On (needed)" : "Off";
                draw->AddText({left, y + 8.0f}, theme.text, name.c_str());
                nx::menu::push_button(draw, {right - VALUE_WIDTH, y}, {right, y + ROW - 4.0f}, state, focused, theme);
            } else if (item == mod_count) {
                nx::menu::push_button(draw, {left, y}, {right, y + ROW - 4.0f},
                                      nx::mods::reset_needed() ? "Reset Game to Apply Changes" : "No Changes to Apply",
                                      focused, theme);
            } else {
                nx::menu::push_button(draw, {left, y}, {right, y + ROW - 4.0f}, "Back", focused, theme);
            }
        }
        if (mods_top > 0) draw->AddText({right - 12.0f, origin.y + TOP - 24.0f}, theme.text_dim, "^");
        if (mods_top + visible < item_total) draw->AddText({right - 12.0f, origin.y + HEIGHT - 52.0f}, theme.text_dim, "v");
        draw_footer(draw, origin, WIDTH, HEIGHT, "A  Switch on / off     B  Back     Changes apply after a reset", theme);
    }

    void draw_modal(const Theme& theme) {
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const ImVec2 screen_size{SCREEN_W, SCREEN_H};
        if (modal == Modal::Capture) {
            char line_one[96];
            char line_two[96];
            const uint64_t waited = SDL_GetTicks64() - capture_started;
            const int left = static_cast<int>((CAPTURE_TIMEOUT_MS - std::min(waited, CAPTURE_TIMEOUT_MS) + 999) / 1000);
            std::snprintf(line_one, sizeof(line_one), "%s %s", capture_add ? "Add an input to" : "Bind",
                          nx::controls::row_label(capture_row));
            std::snprintf(line_two, sizeof(line_two), "Press a button or push a stick  (%d s)", left);
            nx::menu::dialog(draw, screen_size, "Waiting for an input", {line_one, line_two}, "Wait to cancel", theme);
        } else if (modal == Modal::ConfirmReset) {
            nx::menu::dialog(draw, screen_size, "Reset controls?",
                             {"Every input and the menu hotkey go back to the defaults."}, "A  Reset     B  Cancel", theme);
        } else if (modal == Modal::ConfirmGameReset) {
            nx::menu::dialog(draw, screen_size, "Reset the game?",
                             {"The game starts over, with your mods as chosen.", "Progress since your last save is lost."},
                             "A  Reset     B  Cancel", theme);
        } else if (modal == Modal::NoRestart) {
            nx::menu::dialog(draw, screen_size, "Restart not available",
                             {"Your changes are saved. Quit and open the game again", "to apply them."}, "A or B  OK", theme);
        }
    }

    void draw_menu() {
        const Theme& theme = look();
        switch (screen) {
        case Screen::Main: draw_main(theme); break;
        case Screen::Controllers: draw_controllers(theme); break;
        case Screen::Video: draw_video(theme); break;
        case Screen::Mods: draw_mods(theme); break;
        }
        draw_modal(theme);
    }

    // GLideN64 keeps its own GL state cache: the framebuffer binding is put back here, and the ImGui backend restores
    // the rest, so the next emulated frame finds GL as it left it.
    void render(int width, int height, uint64_t now) {
        GLint draw_framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        ImGuiIO& io = ImGui::GetIO();
        // The layout is in 1280x720 units; a 1080p window scales it up.
        io.DisplaySize = {SCREEN_W, SCREEN_H};
        io.DisplayFramebufferScale = {static_cast<float>(width) / SCREEN_W, static_cast<float>(height) / SCREEN_H};
        io.DeltaTime = std::max(0.001f, (now - last_frame_ms) / 1000.0f);
        last_frame_ms = now;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        draw_menu();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_framebuffer));
    }
}

// True while the menu owns the pad: the game then sees a neutral pad (input_switch.cpp).
bool bmhnx_menu_owns_pad() {
    return owns_pad.load();
}

// The menu is open: the renderer presents every refresh so it stays drawn (render_gliden64.cpp).
extern "C" bool bmhnx_menu_is_open() {
    return menu_open.load();
}

// Game thread, at its controller poll: stops here while the menu is open (and lets go when the app is quitting).
extern "C" void bmhnx_menu_pause_point() {
    while (menu_open.load() && !bmhnx_is_quitting()) {
        svcSleepThread(2000000ULL);
    }
}

// Graphics thread, right before the swap (declared inside core_switch.cpp's extern "C" block).
extern "C" void bmhnx_menu_on_swap(int width, int height) {
    const uint64_t now = SDL_GetTicks64();
    update_input(now);
    if (menu_open.load() && imgui_state == ImGuiState::Ready) {
        render(width, height, now);
    }
}
