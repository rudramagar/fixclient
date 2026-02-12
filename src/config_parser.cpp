#include "config_parser.h"
#include "utils.h"

#include <fstream>
#include <stdexcept>
#include <cstdlib>
#include <vector>

static int count_indent(const std::string& line) {
    int count = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == ' ' || line[i] == '\t') {
            count++;
        } else {
            break;
        }
    }
    return count;
}

static bool parse_key_value(const std::string& trimmed,
                            std::string& key,
                            std::string& value) {
    const size_t colon = trimmed.find(':');
    if (colon == std::string::npos) return false;

    key = utils::trim(trimmed.substr(0, colon));
    value = utils::trim(trimmed.substr(colon + 1));

    if (key.empty()) return false;
    return true;
}

void ConfigParser::apply_field(SessionConfig& config,
                               const std::string& key,
                               const std::string& value) {
    if (key == "host") config.host = value;
    else if (key == "port") config.port = std::atoi(value.c_str());
    else if (key == "begin_string") config.begin_string = value;
    else if (key == "sender_comp_id") config.sender_comp_id = value;
    else if (key == "target_comp_id") config.target_comp_id = value;
    else if (key == "heartbeat_interval") config.heartbeat_interval = std::atoi(value.c_str());
    else if (key == "reset_on_logon") config.reset_on_logon = (value == "true");
    else if (key == "username") config.username = value;
    else if (key == "password") config.password = value;
}

void ConfigParser::load(const std::string& path) {
    std::ifstream file(path.c_str());
    if (!file.is_open()) {
        throw std::runtime_error("Error: Cannot open file: " + path);
    }

    sessions.clear();

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }

    // Track indent levels by depth
    // depth 0 = top level (fix:, dropcopy:)
    // depth 1 = default:, sessions:
    // depth 2 = fields under default or list items
    // depth 3 = fields under list items
    int indent_level[4] = {0, 0, 0, 0};
    int depth_count = 0;

    std::string section;
    std::string block;
    SessionConfig section_defaults;
    std::string current_session_key;
    bool in_session_item = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& raw = lines[i];
        const std::string trimmed = utils::trim(raw);

        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        const int indent = count_indent(raw);

        // Strip leading "- " for list items
        std::string item_trimmed = trimmed;
        bool is_list_item = false;
        if (trimmed.size() >= 2 && trimmed[0] == '-' && trimmed[1] == ' ') {
            item_trimmed = utils::trim(trimmed.substr(2));
            is_list_item = true;
        }

        std::string key, value;
        if (!parse_key_value(item_trimmed, key, value)) {
            continue;
        }

        // Determine depth by comparing indent to known levels
        int depth = 0;
        if (indent == 0) {
            depth = 0;
            depth_count = 1;
            indent_level[0] = 0;
        } else if (depth_count >= 1 && indent > indent_level[depth_count - 1]) {
            // Deeper than current deepest
            if (depth_count < 4) {
                indent_level[depth_count] = indent;
                depth = depth_count;
                depth_count++;
            } else {
                depth = 3;
            }
        } else {
            // Find matching or closest level
            for (int d = depth_count - 1; d >= 0; --d) {
                if (indent <= indent_level[d]) {
                    depth = d;
                    depth_count = d + 1;
                } else {
                    break;
                }
            }
            // Check if it's a new deeper level
            if (indent > indent_level[depth_count - 1]) {
                if (depth_count < 4) {
                    indent_level[depth_count] = indent;
                    depth = depth_count;
                    depth_count++;
                }
            }
        }

        // Top level: fix: or dropcopy:
        if (depth == 0) {
            section = key;
            block.clear();
            section_defaults = SessionConfig();
            current_session_key.clear();
            in_session_item = false;
            continue;
        }

        // Level 1: default: or sessions:
        if (depth == 1 && !is_list_item) {
            if (key == "default" || key == "sessions") {
                block = key;
                current_session_key.clear();
                in_session_item = false;
                continue;
            }
        }

        // Fields under default
        if (block == "default" && !is_list_item) {
            apply_field(section_defaults, key, value);
            continue;
        }

        // Session list items and their fields
        if (block == "sessions") {
            if (is_list_item && key == "key") {
                current_session_key = value;
                in_session_item = true;

                SessionConfig& session = sessions[current_session_key];
                session = section_defaults;
                session.name = current_session_key;
                continue;
            }

            if (in_session_item && !current_session_key.empty()) {
                SessionConfig& session = sessions[current_session_key];
                apply_field(session, key, value);
                continue;
            }
        }
    }
}

SessionConfig ConfigParser::get_session(const std::string& session_name) const {
    std::map<std::string, SessionConfig>::const_iterator found = sessions.find(session_name);
    if (found == sessions.end()) {
        throw std::runtime_error("Error: Session not found: " + session_name);
    }
    return found->second;
}
