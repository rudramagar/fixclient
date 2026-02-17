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
static std::string make_unique_id(const char* prefix,
                                  const std::string& sending_time_utc,
                                  int msg_seq_num,
                                  int counter) {
    char digits[32];
    size_t digits_len = 0;

    for (size_t i = 0; i < sending_time_utc.size() && digits_len + 1 < sizeof(digits); ++i) {
        char ch = sending_time_utc[i];
        if (ch >= '0' && ch <= '9') {
            digits[digits_len++] = ch;
        }
    }
    digits[digits_len] = '\0';

    const size_t keep = 11;
    const char* time_part = digits;
    if (digits_len > keep) {
        time_part = digits + (digits_len - keep);
    }

    const int seq_part = (msg_seq_num < 0) ? 0 : (msg_seq_num % 10000);
    const int count_part = (counter < 0) ? 0 : (counter % 100);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%s%04d%02d", prefix, time_part, seq_part, count_part);
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

bool fix_template_apply(FixTemplateRuntime& runtime,
                        FixTemplateMessage& template_message) {

    bool is_set_begin_string = false;
    bool is_set_sender = false;
    bool is_set_target = false;
    bool is_set_seq = false;
    bool is_set_time = false;

    // Overwrite header/runtime fields
	// and fill 60 if blank
    for (size_t i = 0; i < template_message.fields.size(); i++) {
        const int tag_value = template_message.fields[i].first;
        std::string& value_text = template_message.fields[i].second;

        if (tag_value == 8 && !is_set_begin_string) {
            value_text = runtime.begin_string;
            is_set_begin_string = true;
            continue;
        }
        if (tag_value == 49 && !is_set_sender) {
            value_text = runtime.sender_comp_id;
            is_set_sender = true;
            continue;
        }
        if (tag_value == 56 && !is_set_target) {
            value_text = runtime.target_comp_id;
            is_set_target = true;
            continue;
        }
        if (tag_value == 34 && !is_set_seq) {
            value_text = std::to_string(runtime.msg_seq_num);
            is_set_seq = true;
            continue;
        }
        if (tag_value == 52 && !is_set_time) {
            value_text = runtime.sending_time_utc;
            is_set_time = true;
            continue;
        }

        if (tag_value == 60 && value_text.empty()) {
            value_text = runtime.sending_time_utc;
            continue;
        }

        // Replace Original ClOrdID
        // placeholder 41:${ORG_CLRID}
        if (tag_value == 41 && value_text == "${ORG_CLRID}") {
            if (runtime.state.org_clord_id.empty()) {
                runtime.state.org_clord_id = make_unique_id("CL", runtime.sending_time_utc, runtime.msg_seq_num, 1);
            }
            value_text = runtime.state.org_clord_id;
            continue;
        }
    }

    // ALWAYS overwrite CrossID, ClordID
	// (ignore template values)
    int clord_counter = 0;
    int cross_counter = 0;

    for (size_t i = 0; i < template_message.fields.size(); i++) {
        const int tag_value = template_message.fields[i].first;
        std::string& value_text = template_message.fields[i].second;

        if (tag_value == 11) {
            if (value_text == "${ORG_CLRID}") {
                if (runtime.state.org_clord_id.empty()) {
                    runtime.state.org_clord_id = make_unique_id("CL", runtime.sending_time_utc, runtime.msg_seq_num, 1);
                }
                value_text = runtime.state.org_clord_id;
            }
            else {
                clord_counter++;
                value_text = make_unique_id("CL", runtime.sending_time_utc, runtime.msg_seq_num, clord_counter);
            }
            continue;
        }

        if (tag_value == 548) {
            cross_counter++;
            value_text = make_unique_id("X", runtime.sending_time_utc, runtime.msg_seq_num, cross_counter);
            continue;
        }
    }

    return true;
}
