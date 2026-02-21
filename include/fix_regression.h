#ifndef FIX_REGRESSION_H
#define FIX_REGRESSION_H

#include "socket.h"
#include "fix_message.h"
#include "fix_parser.h"

#include <string>
#include <stdint.h>

bool run_fix_regression(TcpSocket& socket,
                        FixParser& fix_parser,
                        FixMessage& fix,
                        const std::string& scenarios_path,
                        int& outbound_seq,
                        uint64_t& last_send_ms,
                        const std::string& token_path,
                        bool& logon_accepted,
                        bool& scenarios_sent,
                        bool& scenario_response_started,
                        uint64_t& last_scenario_response_ms,
                        bool& logout_initiated,
                        uint64_t& logout_start_ms);

#endif
