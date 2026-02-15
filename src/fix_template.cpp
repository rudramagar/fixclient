#include "fix_template.h"
#include "utils.h"

#include <fstream>
#include <cstdio>
#include <stdint.h>

static bool is_ignored_line(const std::string& line_text) {
    const std::string trimmed = utils::trim(line_text);
    if (trimmed.empty()) return true;
    if (trimmed[0] == '#') return true;
    if (trimmed[0] == ';') return true;
    if (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/') return true;
    return false;
}

// Check and parse FIX tag number(digit only)
// from raw FIX message return false
// if invalid
static bool parse_fix_tag(const std::string& tag_text, int& tag_value) {
    if (tag_text.empty()) return false;

    int value = 0;
    for (size_t i = 0; i < tag_text.size(); i++) {
        const char c = tag_text[i];
        if (c < '0' || c > '9') return false;
        value = (value * 10) + (c - '0');
    }

    if (value <= 0) return false;
    tag_value = value;
    return true;
}

// For ClordID(11)
// PREF + EPOS_MS + COUNTER (eg. CL1100..)
static std::string make_unique_id(const char* prefix, uint64_t base_ms, int counter) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%llu-%d", prefix,
                  static_cast<unsigned long long>(base_ms), counter);

    return std::string(buf);
}

// Parse RAW FIX
static bool parse_raw_fix_line(const std::string& raw_line,
							   FixMessage::FieldList& field_list) {
	field_list.clear();

    size_t pos = 0;
    while (pos <= raw_line.size()) {
        size_t next = raw_line.find('|', pos);
        if (next == std::string::npos) next = raw_line.size();

        const std::string token_text = utils::trim(raw_line.substr(pos, next - pos));
        if (!token_text.empty()) {
            const size_t eq_pos = token_text.find('=');
            if (eq_pos != std::string::npos) {
                const std::string tag_text = utils::trim(token_text.substr(0, eq_pos));
                const std::string value_text = token_text.substr(eq_pos + 1); // may be empty

                int tag_value = 0;
                if (parse_fix_tag(tag_text, tag_value)) {
                    field_list.push_back(FixMessage::Field(tag_value, value_text));
                }
            }
        }

        if (next == raw_line.size()) break;
        pos = next + 1;
    }

    if (field_list.empty()) {
        return false;
    }

	return true;
}

// Read the RAW FIX message
// from file and parse it into
// template_message.fields.
bool fix_template_load(const std::string& file_path,FixTemplateMessage& template_message) {
	std::ifstream input(file_path.c_str());
    if (!input.is_open()) {
        return false;
    }

    std::string line_text;
    while (std::getline(input, line_text)) {
        if (is_ignored_line(line_text)) continue;

        FixMessage::FieldList field_list;
        if (!parse_raw_fix_line(utils::trim(line_text), field_list)) {
            return false;
        }

        template_message.fields.swap(field_list);
        return true;
    }

    return false;
}

bool fix_template_apply(const FixTemplateRuntime& runtime,
						FixTemplateMessage& template_message) {

	const uint64_t base_ms = utils::get_monotonic_millis();
    int clord_count = 0;
    int cross_count = 0;

    bool is_set_begin_string = false;
    bool is_set_sender_comp_id = false;
    bool is_set_target_comp_id = false;
    bool is_set_msg_seq_num = false;
    bool is_set_sending_time = false;

    for (size_t i = 0; i < template_message.fields.size(); i++) {
        const int tag_value = template_message.fields[i].first;
        std::string& value_text = template_message.fields[i].second;

        // Overwrite first occurrence only (if present)
        if (tag_value == 8 && !is_set_begin_string) {
            value_text = runtime.begin_string;
            is_set_begin_string = true;
            continue;
        }
        if (tag_value == 49 && !is_set_sender_comp_id) {
            value_text = runtime.sender_comp_id;
            is_set_sender_comp_id = true;
            continue;
        }
        if (tag_value == 56 && !is_set_target_comp_id) {
            value_text = runtime.target_comp_id;
            is_set_target_comp_id = true;
            continue;
        }
        if (tag_value == 34 && !is_set_msg_seq_num) {
            value_text = std::to_string(runtime.msg_seq_num);
            is_set_msg_seq_num = true;
            continue;
        }
        if (tag_value == 52 && !is_set_sending_time) {
            value_text = runtime.sending_time_utc;
            is_set_sending_time = true;
            continue;
        }

        // Fill blanks only
        if (tag_value == 60 && value_text.empty()) {
            value_text = runtime.sending_time_utc;
            continue;
        }
        if (tag_value == 11 && value_text.empty()) {
            clord_count++;
            value_text = make_unique_id("CL", base_ms, clord_count);
            continue;
        }
        if (tag_value == 548 && value_text.empty()) {
            cross_count++;
            value_text = make_unique_id("X", base_ms, cross_count);
            continue;
        }
    }

	return true;
}
