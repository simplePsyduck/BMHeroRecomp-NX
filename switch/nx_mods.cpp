#include "nx_mods.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "librecomp/mods.hpp"

extern const char* bmhnx_mod_game_id;  // main_switch.cpp

namespace {
    struct Mod {
        std::string id;
        std::string name;
        std::vector<std::string> requires_ids;  // dependencies that are not optional
        bool chosen = false;
    };
    std::vector<Mod> loaded;
    std::map<std::string, bool> on_at_start;
    std::vector<bool> needed_flags;

    // Mods that a chosen mod needs, and the ones those need in turn.
    void compute_needed() {
        needed_flags.assign(loaded.size(), false);
        std::vector<bool> on(loaded.size(), false);
        for (size_t i = 0; i < loaded.size(); i++) on[i] = loaded[i].chosen;
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < loaded.size(); i++) {
                if (!on[i]) continue;
                for (const std::string& dependency : loaded[i].requires_ids) {
                    for (size_t j = 0; j < loaded.size(); j++) {
                        if (loaded[j].id == dependency && !on[j]) {
                            on[j] = true;
                            needed_flags[j] = true;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    std::filesystem::path json_path() {
        return recomp::mods::get_mods_directory().parent_path() / "mods.json";
    }

    std::string escape(const std::string& text) {
        std::string out;
        for (char c : text) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    }

    // Index of the ']' that closes the '[' at `open`, skipping over strings; npos when it never closes.
    size_t matching_bracket(const std::string& text, size_t open) {
        bool in_string = false;
        for (size_t i = open + 1; i < text.size(); i++) {
            const char c = text[i];
            if (in_string) {
                if (c == '\\') i++;
                else if (c == '"') in_string = false;
            } else if (c == '"') {
                in_string = true;
            } else if (c == ']') {
                return i;
            }
        }
        return std::string::npos;
    }

    void write_json() {
        std::vector<std::string> enabled;
        for (const Mod& mod : loaded) {
            if (mod.chosen) enabled.push_back(mod.id);
        }
        const std::filesystem::path path = json_path();
        std::string old;
        if (std::ifstream input{path, std::ios::binary}; input.good()) {
            std::stringstream buffer;
            buffer << input.rdbuf();
            old = buffer.str();
        }
        const std::string text = nx::mods::rewrite_json(old, enabled);
        std::ofstream output{path, std::ios::binary};
        output << text;
        if (!output.good()) std::printf("[BMHNX] cannot write %s\n", path.string().c_str());
    }
}

std::string nx::mods::rewrite_json(const std::string& old_json, const std::vector<std::string>& enabled) {
    std::string array = "[";
    for (size_t i = 0; i < enabled.size(); i++) {
        array += std::string(i > 0 ? ", " : "") + "\"" + escape(enabled[i]) + "\"";
    }
    array += "]";

    const size_t key = old_json.find("\"enabled_mods\"");
    if (key != std::string::npos) {
        const size_t open = old_json.find('[', key);
        const size_t close = open == std::string::npos ? std::string::npos : matching_bracket(old_json, open);
        if (close != std::string::npos) {
            return old_json.substr(0, open) + array + old_json.substr(close + 1);
        }
    }
    // No usable list: add the key to an existing object, or start a new file.
    const size_t brace = old_json.find('{');
    if (brace != std::string::npos && old_json.rfind('}') != std::string::npos && old_json.rfind('}') > brace) {
        size_t next = old_json.find_first_not_of(" \t\r\n", brace + 1);
        const bool empty_object = next != std::string::npos && old_json[next] == '}';
        return old_json.substr(0, brace + 1) + "\n    \"enabled_mods\": " + array + (empty_object ? "" : ",") +
               old_json.substr(brace + 1);
    }
    return "{\n    \"enabled_mods\": " + array + ",\n    \"latest_game_mode\": \"\"\n}\n";
}

void nx::mods::refresh() {
    const std::vector<recomp::mods::ModDetails> details = recomp::mods::get_all_mod_details(bmhnx_mod_game_id);
    std::map<std::string, bool> previous;
    for (const Mod& mod : loaded) previous[mod.id] = mod.chosen;
    loaded.clear();
    for (const recomp::mods::ModDetails& detail : details) {
        Mod mod;
        mod.id = detail.mod_id;
        mod.name = detail.display_name.empty() ? detail.mod_id : detail.display_name;
        for (const recomp::mods::Dependency& dependency : detail.dependencies) {
            if (!dependency.optional) mod.requires_ids.push_back(dependency.mod_id);
        }
        on_at_start.try_emplace(mod.id, recomp::mods::is_mod_enabled(mod.id) || recomp::mods::is_mod_auto_enabled(mod.id));
        const auto before = previous.find(mod.id);
        mod.chosen = before != previous.end() ? before->second : recomp::mods::is_mod_enabled(mod.id);
        loaded.push_back(std::move(mod));
    }
    compute_needed();
}

int nx::mods::count() { return static_cast<int>(loaded.size()); }
const std::string& nx::mods::name(int index) { return loaded[index].name; }
bool nx::mods::chosen(int index) { return loaded[index].chosen; }
bool nx::mods::needed(int index) { return !loaded[index].chosen && needed_flags[index]; }
bool nx::mods::on(int index) { return loaded[index].chosen || needed_flags[index]; }

void nx::mods::toggle(int index) {
    loaded[index].chosen = !loaded[index].chosen;
    compute_needed();
    write_json();
}

bool nx::mods::reset_needed() {
    for (size_t i = 0; i < loaded.size(); i++) {
        if (on(static_cast<int>(i)) != on_at_start[loaded[i].id]) return true;
    }
    return false;
}

std::string nx::mods::folder() {
    return recomp::mods::get_mods_directory().string();
}
