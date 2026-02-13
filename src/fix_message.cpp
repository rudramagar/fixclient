#include "fix_message.h"
#include "utils.h"
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

// Logon
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

// HeartBeat
std::string FixMessage::build_heartbeat(int msg_seq_num,
                                        const std::string& sending_time,
                                        const std::string& test_req_id) const {

    FieldList fields;
    fields.reserve(2);

    // TestReqID
    // only when replying
    // to TestRequest
    if (!test_req_id.empty()) {
        fields.push_back(Field(112, test_req_id));
    }

    return build_message("0", msg_seq_num, sending_time, fields);
}

// TestRequest
std::string FixMessage::build_test_request(int msg_seq_num,
                                           const std::string& sending_time,
                                           const std::string& test_req_id) const {

    FieldList fields;
    fields.reserve(2);

    // TestReqID
    fields.push_back(Field(112, test_req_id));

    return build_message("1", msg_seq_num, sending_time, fields);
}

// ResendRequest
std::string FixMessage::build_resend_request(int msg_seq_num,
                                             const std::string& sending_time,
                                             int begin_seq_no, int end_seq_no) const {

    FieldList fields;
    fields.reserve(2);

    // BeginSeqNo (7)
    // EndSeqNo (16)
    char begin_seq_buf[32];
    std::snprintf(begin_seq_buf, sizeof(begin_seq_buf), "%d", begin_seq_no);
    fields.push_back(Field(7, begin_seq_buf));

    char end_seq_buf[32];
    std::snprintf(end_seq_buf, sizeof(end_seq_buf), "%d", end_seq_no);
    fields.push_back(Field(16, end_seq_buf));

    return build_message("2", msg_seq_num, sending_time, fields);
}

// Sequence Reset
std::string FixMessage::build_sequence_reset(int msg_seq_num,
                                             const std::string& sending_time,
                                             int new_seq_no, bool gap_fill) const {

    FieldList fields;
    fields.reserve(4);

    // NewSeqNo (36)
    char new_seq_buf[32];
    std::snprintf(new_seq_buf, sizeof(new_seq_buf), "%d", new_seq_no);
    fields.push_back(Field(36, new_seq_buf));

    // Gapfill
    if (gap_fill) {
        fields.push_back(Field(123, "Y"));
    }

    return build_message("4", msg_seq_num, sending_time, fields);
}

// Logout
std::string FixMessage::build_logout(int msg_seq_num,
                                     const std::string& sending_time,
                                     const std::string& text) const {

    FieldList fields;
    fields.reserve(2);

    // Text (58)
    if (!text.empty()) {
        fields.push_back(Field(58, text));
    }

    return build_message("5", msg_seq_num, sending_time, fields);
}

std::string FixMessage::to_pipe_delimited(const std::string& fix) {
    return utils::to_pipe_delimited(fix);
}
