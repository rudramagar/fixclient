#ifndef FIX_TEMPLATE
#define FIX_TEMPLATE

#include "fix_message.h"
#include <string>
#include <stdint.h>

struct FixTemplateMessage {
    std::string msg_type;
    FixMessage::FieldList fields;
}

struct FixTemplateRuntime {
    std::string begin_string;
    std::string sender_comp_id;
    std::string target_comp_id;
    int msg_seq_num;
    std::string sending_time_utc;
};

bool fix_template_load(
        const std::string& file_path, 
        FixTemplateMessage& template_message,
);

bool fix_template_appy(
        const FixTemplateRuntime& runtime,
        FixTemplateMessage& template_message,
);
