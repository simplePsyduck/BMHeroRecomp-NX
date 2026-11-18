#include "nx_controls.h"

#include "nx_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {
    constexpr float STICK_AS_BUTTON = 0.5f;
    constexpr float CAPTURE_PRESS = 0.6f;   // a stick counts as pressed when captured
    constexpr float CAPTURE_REST = 0.3f;    // and as released below this

    struct N64Button { const char* name; const char* label; uint16_t bit; };
    constexpr N64Button N64_BUTTONS[] = {
        {"A", "A Button", 0x8000},          {"B", "B Button", 0x4000},        {"Z", "Z Trigger", 0x2000},
        {"START", "Start", 0x1000},         {"DUP", "D-Pad Up", 0x0800},      {"DDOWN", "D-Pad Down", 0x0400},
        {"DLEFT", "D-Pad Left", 0x0200},    {"DRIGHT", "D-Pad Right", 0x0100}, {"L", "L Trigger", 0x0020},
        {"R", "R Trigger", 0x0010},         {"CUP", "C Up", 0x0008},          {"CDOWN", "C Down", 0x0004},
        {"CLEFT", "C Left", 0x0002},        {"CRIGHT", "C Right", 0x0001},
    };
    static_assert(sizeof(N64_BUTTONS) / sizeof(N64_BUTTONS[0]) == nx::controls::INPUT_COUNT);
    constexpr int N64_COUNT = nx::controls::INPUT_COUNT;

    enum class Axis { None, LeftX, LeftY, RightX, RightY };
    // One Switch input: a button, or a stick pushed past the halfway point in one direction.
    struct Source {
        uint32_t button = 0;
        Axis axis = Axis::None;
        bool positive = false;
        bool operator==(const Source& other) const {
            return button == other.button && axis == other.axis && positive == other.positive;
        }
    };

    struct SourceName { const char* name; const char* label; Source source; };
    const SourceName SOURCES[] = {
        {"A", "A", {NX_A}}, {"B", "B", {NX_B}}, {"X", "X", {NX_X}}, {"Y", "Y", {NX_Y}},
        {"L", "L", {NX_L}}, {"R", "R", {NX_R}}, {"ZL", "ZL", {NX_ZL}}, {"ZR", "ZR", {NX_ZR}},
        {"PLUS", "+", {NX_PLUS}}, {"MINUS", "-", {NX_MINUS}},
        {"DPAD_UP", "D-Pad Up", {NX_UP}}, {"DPAD_DOWN", "D-Pad Down", {NX_DOWN}},
        {"DPAD_LEFT", "D-Pad Left", {NX_LEFT}}, {"DPAD_RIGHT", "D-Pad Right", {NX_RIGHT}},
        {"LSTICK", "L Stick Click", {NX_LSTICK}}, {"RSTICK", "R Stick Click", {NX_RSTICK}},
        {"LSTICK_UP", "L Stick Up", {0, Axis::LeftY, true}}, {"LSTICK_DOWN", "L Stick Down", {0, Axis::LeftY, false}},
        {"LSTICK_LEFT", "L Stick Left", {0, Axis::LeftX, false}}, {"LSTICK_RIGHT", "L Stick Right", {0, Axis::LeftX, true}},
        {"RSTICK_UP", "R Stick Up", {0, Axis::RightY, true}}, {"RSTICK_DOWN", "R Stick Down", {0, Axis::RightY, false}},
        {"RSTICK_LEFT", "R Stick Left", {0, Axis::RightX, false}}, {"RSTICK_RIGHT", "R Stick Right", {0, Axis::RightX, true}},
    };

    std::mutex mapping_mutex;  // the menu edits while the game thread may be reading
    std::vector<Source> mapping[N64_COUNT];
    std::vector<Source> menu_hotkey;
    bool stick_from_right = false;

    // The defaults, in the form of the [controls] section.
    constexpr const char* DEFAULT_CONTROLS =
        "MENU = MINUS\n"
        "STICK = LEFT\n"
        "\n"
        "A = A\n"
        "B = B\n"
        "Z = ZL\n"
        "START = PLUS\n"
        "L = L\n"
        "R = R, ZR\n"
        "\n"
        "DUP = DPAD_UP\n"
        "DDOWN = DPAD_DOWN\n"
        "DLEFT = DPAD_LEFT\n"
        "DRIGHT = DPAD_RIGHT\n"
        "\n"
        "CUP = RSTICK_UP\n"
        "CDOWN = RSTICK_DOWN\n"
        "CLEFT = RSTICK_LEFT\n"
        "CRIGHT = RSTICK_RIGHT\n";

    constexpr const char* HELP =
        "# One line per N64 input: <N64 input> = <Switch input>, <Switch input>, ...\n"
        "# Any listed Switch input presses the N64 input. Leave the right side empty to unbind it.\n"
        "#\n"
        "# N64 inputs:    A B Z START L R DUP DDOWN DLEFT DRIGHT CUP CDOWN CLEFT CRIGHT\n"
        "# Switch inputs: A B X Y L R ZL ZR PLUS MINUS DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT LSTICK RSTICK\n"
        "#                LSTICK_UP LSTICK_DOWN LSTICK_LEFT LSTICK_RIGHT RSTICK_UP RSTICK_DOWN RSTICK_LEFT RSTICK_RIGHT\n"
        "#                (a stick counts as a button once pushed past the halfway point)\n"
        "#\n"
        "# MENU is the pause menu's hotkey. STICK picks which Switch stick is the N64 analog stick: LEFT or RIGHT.\n"
        "# LAYOUT = LABELS: A B X Y are the Switch buttons as printed on them. Keep this line.\n";

    std::string upper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
        return s;
    }

    std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
        return s.substr(a, b - a);
    }

    void clear_all() {
        for (int i = 0; i < N64_COUNT; i++) mapping[i].clear();
        menu_hotkey.clear();
        stick_from_right = false;
    }

    std::vector<Source>* bindings(int row) {
        if (row >= 0 && row < N64_COUNT) return &mapping[row];
        if (row == nx::controls::HOTKEY_ROW) return &menu_hotkey;
        return nullptr;
    }

    // Caller holds the mutex.
    void parse(const std::string& text) {
        size_t pos = 0;
        int line_number = 0;
        while (pos <= text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = trim(text.substr(pos, end - pos));
            pos = end + 1;
            line_number++;
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                std::printf("[BMHNX] controls.ini line %d: expected 'name = value'\n", line_number);
                continue;
            }
            const std::string key = upper(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            if (key == "LAYOUT") continue;  // marker, see convert_old_layout
            if (key == "STICK") {
                const std::string side = upper(value);
                if (side == "LEFT" || side == "RIGHT") {
                    stick_from_right = side == "RIGHT";
                } else {
                    std::printf("[BMHNX] controls.ini line %d: STICK must be LEFT or RIGHT\n", line_number);
                }
                continue;
            }
            std::vector<Source>* target = nullptr;
            if (key == "MENU") {
                target = &menu_hotkey;
            } else {
                for (int i = 0; i < N64_COUNT; i++) {
                    if (key == N64_BUTTONS[i].name) target = &mapping[i];
                }
            }
            if (target == nullptr) {
                std::printf("[BMHNX] controls.ini line %d: unknown input '%s'\n", line_number, key.c_str());
                continue;
            }
            target->clear();
            size_t start = 0;
            while (start <= value.size()) {
                size_t comma = value.find(',', start);
                if (comma == std::string::npos) comma = value.size();
                const std::string token = upper(trim(value.substr(start, comma - start)));
                start = comma + 1;
                if (token.empty()) continue;
                bool found = false;
                for (const SourceName& source : SOURCES) {
                    if (token == source.name) {
                        target->push_back(source.source);
                        found = true;
                    }
                }
                if (!found) {
                    std::printf("[BMHNX] controls.ini line %d: unknown Switch input '%s'\n", line_number, token.c_str());
                }
            }
        }
    }

    float axis_value(const NxPad& pad, Axis axis) {
        switch (axis) {
        case Axis::LeftX: return pad.lx;
        case Axis::LeftY: return pad.ly;
        case Axis::RightX: return pad.rx;
        case Axis::RightY: return pad.ry;
        case Axis::None: break;
        }
        return 0.0f;
    }

    bool pressed(const NxPad& pad, const Source& source) {
        if (source.axis == Axis::None) return (pad.buttons & source.button) != 0;
        const float value = axis_value(pad, source.axis);
        return source.positive ? value > STICK_AS_BUTTON : value < -STICK_AS_BUTTON;
    }

    const char* source_name(const Source& source) {
        for (const SourceName& entry : SOURCES) {
            if (entry.source == source) return entry.name;
        }
        return "";
    }

    const char* source_label(const Source& source) {
        for (const SourceName& entry : SOURCES) {
            if (entry.source == source) return entry.label;
        }
        return "?";
    }

    std::string join(const std::vector<Source>& sources, const char* separator, bool labels) {
        std::string text;
        for (const Source& source : sources) {
            if (!text.empty()) text += separator;
            text += labels ? source_label(source) : source_name(source);
        }
        return text;
    }

    // Before the LAYOUT line existed, A B X Y in this section named SDL's positions (A the bottom button), so they
    // were the Switch's B A Y X. A section without the marker is converted once, which keeps every existing binding on
    // the same physical button; saving then writes the marker.
    bool has_layout_marker(const std::string& text) {
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string line = trim(text.substr(pos, end - pos));
            pos = end + 1;
            const size_t eq = line.find('=');
            if (eq != std::string::npos && upper(trim(line.substr(0, eq))) == "LAYOUT") return true;
        }
        return false;
    }

    std::string convert_old_layout(const std::string& text) {
        std::string out;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            const std::string trimmed = trim(line);
            const size_t eq = trimmed.find('=');
            const std::string key = eq == std::string::npos ? "" : upper(trim(trimmed.substr(0, eq)));
            if (trimmed.empty() || trimmed[0] == '#' || eq == std::string::npos || key == "STICK") {
                out += line + "\n";
                continue;
            }
            const std::string value = trimmed.substr(eq + 1);
            std::string converted;
            size_t start = 0;
            while (start <= value.size()) {
                size_t comma = value.find(',', start);
                if (comma == std::string::npos) comma = value.size();
                std::string token = trim(value.substr(start, comma - start));
                start = comma + 1;
                const std::string name = upper(token);
                if (name == "A") token = "B";
                else if (name == "B") token = "A";
                else if (name == "X") token = "Y";
                else if (name == "Y") token = "X";
                if (!converted.empty()) converted += ", ";
                converted += token;
            }
            out += trim(trimmed.substr(0, eq)) + " = " + converted + "\n";
        }
        return out;
    }

    std::string current_text() {
        std::string text = "LAYOUT = LABELS\n";
        text += "MENU = " + join(menu_hotkey, ", ", false) + "\n";
        text += std::string("STICK = ") + (stick_from_right ? "RIGHT" : "LEFT") + "\n\n";
        // The order and grouping of the defaults; -1 is a blank line.
        for (int i : {0, 1, 2, 3, 8, 9, -1, 4, 5, 6, 7, -1, 10, 11, 12, 13}) {
            text += i < 0 ? std::string("\n")
                          : std::string(N64_BUTTONS[i].name) + " = " + join(mapping[i], ", ", false) + "\n";
        }
        return text;
    }
}

bool nx::controls::apply_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(mapping_mutex);
    // Defaults first, so a missing or half-valid section still gives a playable layout.
    clear_all();
    parse(DEFAULT_CONTROLS);
    const bool converted = !trim(text).empty() && !has_layout_marker(text);
    parse(converted ? convert_old_layout(text) : text);
    return converted;
}

std::string nx::controls::text() {
    std::lock_guard<std::mutex> lock(mapping_mutex);
    return current_text();
}

const char* nx::controls::help_text() {
    return HELP;
}

void nx::controls::save() {
    nx::config::save();
}

void nx::controls::reset_defaults() {
    {
        std::lock_guard<std::mutex> lock(mapping_mutex);
        clear_all();
        parse(DEFAULT_CONTROLS);
    }
    save();
}

uint16_t nx::controls::to_n64_buttons(const NxPad& pad) {
    std::lock_guard<std::mutex> lock(mapping_mutex);
    uint16_t buttons = 0;
    for (int i = 0; i < N64_COUNT; i++) {
        for (const Source& source : mapping[i]) {
            if (pressed(pad, source)) {
                buttons |= N64_BUTTONS[i].bit;
                break;
            }
        }
    }
    return buttons;
}

void nx::controls::analog_stick(const NxPad& pad, float* x, float* y) {
    *x = stick_from_right ? pad.rx : pad.lx;
    *y = stick_from_right ? pad.ry : pad.ly;
}

bool nx::controls::hotkey_held(const NxPad& pad) {
    std::lock_guard<std::mutex> lock(mapping_mutex);
    for (const Source& source : menu_hotkey) {
        if (pressed(pad, source)) return true;
    }
    return false;
}

const char* nx::controls::row_label(int row) {
    if (row >= 0 && row < N64_COUNT) return N64_BUTTONS[row].label;
    if (row == HOTKEY_ROW) return "Menu Hotkey";
    return "";
}

std::string nx::controls::describe(int row) {
    std::lock_guard<std::mutex> lock(mapping_mutex);
    const std::vector<Source>* sources = bindings(row);
    if (sources == nullptr || sources->empty()) return "Not bound";
    return join(*sources, " / ", true);
}

void nx::controls::clear(int row) {
    {
        std::lock_guard<std::mutex> lock(mapping_mutex);
        if (std::vector<Source>* sources = bindings(row)) sources->clear();
    }
    save();
}

bool nx::controls::capture(int row, const NxPad& pad, NxPad* base, bool add) {
    // What was held when the wait began only counts again after it was let go.
    base->buttons &= pad.buttons;
    for (float* held : {&base->lx, &base->ly, &base->rx, &base->ry}) {
        const float* now = held == &base->lx ? &pad.lx : held == &base->ly ? &pad.ly : held == &base->rx ? &pad.rx : &pad.ry;
        if (std::fabs(*now) < CAPTURE_REST) *held = 0.0f;
    }
    const Source* caught = nullptr;
    for (const SourceName& entry : SOURCES) {
        const Source& source = entry.source;
        if (source.axis == Axis::None) {
            if ((pad.buttons & source.button) != 0 && (base->buttons & source.button) == 0) {
                caught = &source;
                break;
            }
        } else {
            const float value = axis_value(pad, source.axis);
            const float was = axis_value(*base, source.axis);
            const bool now_pressed = source.positive ? value > CAPTURE_PRESS : value < -CAPTURE_PRESS;
            const bool was_pressed = source.positive ? was > CAPTURE_PRESS : was < -CAPTURE_PRESS;
            if (now_pressed && !was_pressed) {
                caught = &source;
                break;
            }
        }
    }
    if (caught == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(mapping_mutex);
        if (std::vector<Source>* sources = bindings(row)) {
            if (!add) sources->clear();
            if (std::find(sources->begin(), sources->end(), *caught) == sources->end()) sources->push_back(*caught);
        }
    }
    save();
    return true;
}
