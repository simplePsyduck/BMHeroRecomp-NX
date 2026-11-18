#include "nx_config.h"

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <string>

#include "nx_controls.h"
#include "nx_settings.h"

#ifndef NX_CONFIG_PATH
#define NX_CONFIG_PATH "sdmc:/switch/bmherorecomp/config.ini"
#endif
#ifndef NX_LEGACY_SETTINGS_PATH
#define NX_LEGACY_SETTINGS_PATH "sdmc:/switch/bmherorecomp/nx_options.txt"
#endif
#ifndef NX_LEGACY_CONTROLS_PATH
#define NX_LEGACY_CONTROLS_PATH "sdmc:/switch/bmherorecomp/controls.ini"
#endif

namespace {
    constexpr const char* CONFIG_PATH = NX_CONFIG_PATH;
    std::mutex write_mutex;  // the menu thread saves; keep two saves from interleaving

    bool read_file(const char* path, std::string* text) {
        FILE* file = std::fopen(path, "rb");
        if (file == nullptr) return false;
        char buffer[1024];
        size_t n;
        text->clear();
        while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text->append(buffer, n);
        std::fclose(file);
        return true;
    }

    // The body of "[name]": its lines up to the next section header (empty when the section is missing).
    std::string section(const std::string& text, const std::string& name) {
        const std::string header = "[" + name + "]";
        size_t pos = 0;
        bool inside = false;
        std::string body;
        while (pos <= text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos && line[first] == '[') {
                inside = line.compare(first, header.size(), header) == 0;
                continue;
            }
            if (inside) body += line + "\n";
        }
        return body;
    }

    std::string compose() {
        std::string text =
            "# Bomberman Hero: Recompiled - Switch configuration\n"
            "#\n"
            "# Everything the pause menu can change lives in this one file, and the menu rewrites it whenever you change\n"
            "# something. To edit it by hand, do so while the game is closed. Lines starting with # are ignored; delete\n"
            "# this file to get the defaults back.\n"
            "\n"
            "[settings]\n";
        text += nx::settings_text();
        text += "\n[controls]\n";
        text += nx::controls::help_text();
        text += nx::controls::text();
        return text;
    }

    // Written next to the file, then renamed over it, so a full card or a power cut never leaves it truncated.
    void write_file(const std::string& text) {
        const std::string temporary = std::string(CONFIG_PATH) + ".tmp";
        FILE* file = std::fopen(temporary.c_str(), "wb");
        bool written = file != nullptr && std::fwrite(text.data(), 1, text.size(), file) == text.size();
        if (file != nullptr && std::fclose(file) != 0) written = false;
        // The SD card's rename does not replace an existing file.
        if (!written || (std::remove(CONFIG_PATH) != 0 && errno != ENOENT) ||
            std::rename(temporary.c_str(), CONFIG_PATH) != 0) {
            std::printf("[BMHNX] cannot write %s\n", CONFIG_PATH);
            std::remove(temporary.c_str());
        }
    }
}

void nx::config::load() {
    std::string text;
    if (read_file(CONFIG_PATH, &text)) {
        std::printf("[BMHNX] config: reading %s\n", CONFIG_PATH);
        nx::apply_text(section(text, "settings"));
        if (nx::controls::apply_text(section(text, "controls"))) {
            std::printf("[BMHNX] config: controls converted to the printed button labels, rewriting %s\n", CONFIG_PATH);
            save();
        }
        return;
    }
    // First run with a single config file: carry over the older two files, when they are there.
    std::string legacy_settings;
    std::string legacy_controls;
    const bool had_settings = read_file(NX_LEGACY_SETTINGS_PATH, &legacy_settings);
    const bool had_controls = read_file(NX_LEGACY_CONTROLS_PATH, &legacy_controls);
    nx::apply_text(legacy_settings);
    nx::controls::apply_text(legacy_controls);  // converted when old; the file is written below either way
    std::printf("[BMHNX] config: %s not found, %s\n", CONFIG_PATH,
                had_settings || had_controls ? "carried over nx_options.txt / controls.ini" : "writing the defaults");
    save();
}

void nx::config::save() {
    std::lock_guard<std::mutex> lock(write_mutex);
    write_file(compose());
}

void nx::save() {
    nx::config::save();
}
