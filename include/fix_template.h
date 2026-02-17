#ifndef FIX_TEMPLATE_H
#define FIX_TEMPLATE_H

#include "fix_message.h"
#include <string>
#include <stdint.h>

struct FixTemplateMessage {
    std::string msg_type;
    FixMessage::FieldList fields;
};


// Shared
// same state across multiple
// messages
// e.g., 11=$(ORG_ClOrdID) and 41=$(ORG_ClOrdID)
struct FixTemplateState {
    std::string org_clord_id;
};

struct FixTemplateRuntime {
    std::string begin_string;
    std::string sender_comp_id;
    std::string target_comp_id;
    int msg_seq_num;
    std::string sending_time_utc;

    FixTemplateState state;
};

bool fix_template_load(
        const std::string& file_path, 
        FixTemplateMessage& template_message
);

bool fix_template_apply(
        FixTemplateRuntime& runtime,
        FixTemplateMessage& template_message
);

#endif
