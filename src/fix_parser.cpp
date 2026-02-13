#include "fix_parser.h"
#include <cctype>

static const char soh = '\x01';

FixParser::FixParser() {}

void FixParser::append_bytes(const char* data, size_t size) {
    if (data == 0 || size == 0) {
        return;
    }

    buffer.append(data, size);
}

void FixParser::reset() {
    buffer.clear();
}

bool FixParser::find_begin_string(size_t& start_pos) const {
    start_pos = buffer.find("8=FIX");
    return (start_pos != std::string::npos);
}

bool FixParser::parse_body_length(size_t start_pos, int& body_length, size_t& end_body_len_field) const {
    body_length = -1;
    end_body_len_field = 0;

    size_t body_len_pos = std::string::npos;
    size_t scan_pos = start_pos;

    while (true) {
        size_t found = buffer.find("9=", scan_pos);
        if (found == std::string::npos) {
            return false;
        }

        bool at_field_start = (found == 0) || (buffer[found - 1] == soh);
        if (at_field_start) {
            body_len_pos = found;
            break;
        }

        scan_pos = found + 2;
    }

    size_t body_len_value_pos = body_len_pos + 2;

    size_t body_len_end = buffer.find(soh, body_len_value_pos);
    if (body_len_end == std::string::npos) {
        return false;
    }

    if (body_len_end <= body_len_value_pos) {
        return false;
    }

    const long max_body_len = 1000000; // 1MB
    long body_len_value = 0;

    for (size_t pos = body_len_value_pos; pos < body_len_end; pos++) {
        unsigned char ch = static_cast<unsigned char>(buffer[pos]);
        if (!std::isdigit(ch)) {
            return false;
        }

        body_len_value = body_len_value * 10 + (ch - '0');
        if (body_len_value > max_body_len) {
            return false;
        }
    }

    body_length = static_cast<int>(body_len_value);
    end_body_len_field = body_len_end + 1;
    return true;
}

bool FixParser::read_next_message(std::string& message) {
    message.clear();

    // Find "8=FIX"
    size_t start_pos = 0;
    if (!find_begin_string(start_pos)) {
        if (buffer.size() > 8) {
            buffer.erase(0, buffer.size() - 8);
        }
        return false;
    }

    // Drop garbage
    // before beginstring
    if (start_pos > 0) {
        buffer.erase(0, start_pos);
    }

    // Read BodyLength
    int body_length = -1;
    size_t end_body_len_field = 0;
    if (!parse_body_length(0, body_length, end_body_len_field)) {
        return false;
    }

    // Body Start
    size_t body_start = end_body_len_field;

    // Checksum field
    size_t checksum_start = body_start + static_cast<size_t>(body_length);

    if (buffer.size() < checksum_start + 7) {
        return false;
    }

    // Validate "10=" at the computed pos.
	if (checksum_start + 3 > buffer.size() || buffer.compare(checksum_start, 3, "10=") != 0) {
		const size_t next_start = buffer.find("8=FIX", 1);
        if (next_start != std::string::npos) {
            buffer.erase(0, next_start);
        } else {
            buffer.clear();
        }

        return false;
    }

    // Message end at the SOH after checksum
    size_t end_pos = buffer.find(soh, checksum_start);
    if (end_pos == std::string::npos) {
        return false;
    }

    // Extract complete FIX message
    message.assign(buffer.data(), end_pos + 1);
    buffer.erase(0, end_pos + 1);
    return true;
}
