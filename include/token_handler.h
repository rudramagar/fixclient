#ifndef TOKEN_HANDLER_H
#define TOKEN_HANDLER_H

#include <string>

bool read_token(const std::string& token_dir, 
                const std::string& sender_comp_id,
                const std::string& utc_timestamp,
                bool reset_on_logon,
                int& next_seq,
                std::string& token_path_out);

bool save_token(const std::string& token_path, int next_seq);

#endif
