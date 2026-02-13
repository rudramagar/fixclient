#include "fix_message.h"

#include <cstdio>

FixMessage::FixMessage() {}

void FixMessage::set_begin_string(const std::string& value) {
    begin_string = value;
}

void FixMessage::set_sender_comp_id(const std::string& value) {
    sender_comp_id = value;
}

void FixMessage::set_target_comp_id(const std::string& value) {
    target_comp_id = value;
}

char FixMessage::soh() {
    return '\x01';
}

void FixMessage::append_field(std::string& buffer, int tag, const std::string& tag_value) {
    char tag_buf[32];
    std::snprintf(tag_buf, sizeof(tag_buf), "%d=", tag);
    buffer.append(tag_buf);
    buffer.append(tag_value);
    buffer.push_back(soh());
}

void FixMessage::append_field_int(std::string& buffer, int tag, int tag_value) {
    char field_buf[64];
    std::snprintf(field_buf, sizeof(field_buf), "%d=%d", tag, tag_value);
    buffer.append(field_buf);
    buffer.push_back(soh());
}

int FixMessage::calculate_checksum(const std::string& data) {
    unsigned int sum = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        sum += static_cast<unsigned char>(data[i]);
    }
    return static_cast<int>(sum % 256);
}

std::string FixMessage::build_message(const std::string& msg_type,
                                      int msg_seq_num,
                                      const std::string& sending_time,
                                      const FieldList& body_fields) const {

    // Guard
    if (begin_string.empty() || sender_comp_id.empty() || target_comp_id.empty()) {
        return std::string();
    }

    // Build body
    // everything after
    // "9=..|"
    std::string body;
    body.reserve(256);

    // Standard FIX Header
    append_field(body, 35, msg_type);
    append_field_int(body, 34, msg_seq_num);
    append_field(body, 49, sender_comp_id);
    append_field(body, 56, target_comp_id);
    append_field(body, 52, sending_time);

    // Append extra tags the
    // caller requested
    // and keeps them in same order
    // as given in body_fields
    for (size_t i = 0; i < body_fields.size(); ++i) {
        append_field(body, body_fields[i].first, body_fields[i].second);
    }

    // Build full FIX Message
    // 8 + 9 + body + 10
    std::string msg;
    msg.reserve(64 + body.size() + 16);

    append_field(msg, 8, begin_string);
    append_field_int(msg, 9, static_cast<int>(body.size()));
    msg.append(body);

    const int checksum = calculate_checksum(msg);
    char checksum_buf[16];
    std::snprintf(checksum_buf, sizeof(checksum_buf), "%03d", checksum);
    append_field(msg, 10, checksum_buf);

    return msg;
}

std::string FixMessage::build_logon(int msg_seq_num, const std::string& sending_time,
                                    int heartbeat_interval, bool reset_seq_num) const {

    FieldList fields;
    fields.reserve(4);

    // EncryptMethod=0 (NONE)
    fields.push_back(Field(98, "0"));

    // HeartBtInt=<seconds>
    char hb_buf[32];
    std::snprintf(hb_buf, sizeof(hb_buf), "%d", heartbeat_interval);
    fields.push_back(Field(108, hb_buf));

    // ResetSeeqNumFlag=Y
    if (reset_seq_num) {
        fields.push_back(Field(141, "Y"));
    }

    return build_message("A", msg_seq_num, sending_time, fields);
}

std::string FixMessage::to_pipe_delimited(const std::string& fix) {
    std::string printable = fix;
    for (size_t i = 0; i < printable.size(); ++i) {
        if (printable[i] == soh()) {
            printable[i] = '|';
        }
    }
    return printable;
}
