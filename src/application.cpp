#include "application.h"
#include "config_parser.h"
#include "fix_parser.h"
#include "fix_message.h"
#include "fix_template.h"
#include "token_handler.h"
#include "utils.h"
#include "fix_regression.h"
#include <cstdio>
#include <string>
#include <cstdint>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include <vector>
#include <algorithm>
#include <fstream>
#include <ctime>

const int peer_closed = 0;
const size_t receive_buffer_size = 4096;
const int logon_timeout_seconds = 5;
const int receive_timeout_millis = 200;
static bool is_running_regression = false;
static bool store_msgs_enabled = false;
static std::string store_sender_comp_id;
static int g_expected_incoming_seq = 1;
static std::string g_token_path;

static void store_sent_message(const std::string& message) {
    if (!store_msgs_enabled) return;

    std::string seq_str;
    if (!utils::find_tag_value(message, "34=", seq_str)) return;

    const int seq = std::atoi(seq_str.c_str());
    if (seq <= 0) return;

    ::mkdir("store", 0755);

    std::time_t now = std::time(0);
    struct tm local_time;
    localtime_r(&now, &local_time);

    char path[256];
    std::snprintf(path, sizeof(path), "store/%s_%04d%02d%02d.store",
                  store_sender_comp_id.c_str(),
                  local_time.tm_year + 1900,
                  local_time.tm_mon + 1,
                  local_time.tm_mday);

    std::FILE* f = std::fopen(path, "a");
    if (!f) return;

    std::fprintf(f, "SEQ=%d|MSG=", seq);
    std::fwrite(message.data(), 1, message.size(), f);
    std::fprintf(f, "\n");
    std::fclose(f);
}

// Load stored message for re-send
static std::string load_stored_message(int seq) {
    if (store_sender_comp_id.empty()) return std::string();

    // Find the store file for today or scan all store files
    DIR* dir = ::opendir("store");
    if (!dir) return std::string();

    char prefix[128];
    std::snprintf(prefix, sizeof(prefix), "%s_", store_sender_comp_id.c_str());
    const size_t prefix_len = std::strlen(prefix);

    // Collect matching store files
    std::vector<std::string> store_files;
    dirent* entry = 0;
    while ((entry = ::readdir(dir)) != 0) {
        const std::string name(entry->d_name);
        if (name.size() > prefix_len &&
            name.compare(0, prefix_len, prefix) == 0 &&
            name.size() > 6 &&
            name.compare(name.size() - 6, 6, ".store") == 0) {
            store_files.push_back("store/" + name);
        }
    }
    ::closedir(dir);

    // Search newest files first
    std::sort(store_files.begin(), store_files.end());

    char seq_prefix[32];
    std::snprintf(seq_prefix, sizeof(seq_prefix), "SEQ=%d|MSG=", seq);
    const size_t seq_prefix_len = std::strlen(seq_prefix);

    // Search from newest to oldest
    for (int i = static_cast<int>(store_files.size()) - 1; i >= 0; --i) {
        std::FILE* f = std::fopen(store_files[i].c_str(), "r");
        if (!f) continue;

        char line[8192];
        std::string found;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, seq_prefix, seq_prefix_len) == 0) {
                // Strip trailing newline
                size_t len = std::strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                    len--;
                }
                found = std::string(line + seq_prefix_len, len - seq_prefix_len);
                break;
            }
        }
        std::fclose(f);

        if (!found.empty()) return found;
    }

    return std::string();
}

static bool set_socket_recv_timeout(int sock_fd, int timeout_millis) {
    timeval tv;
    tv.tv_sec = timeout_millis / 1000;
    tv.tv_usec = (timeout_millis % 1000) * 1000;

    const int rc = ::setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return rc == 0;
}

static bool recv_timed_out() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static bool send_fix_message(TcpSocket& socket,
                             const std::string& message,
                             uint64_t& last_send_ms) {
    if (message.empty()) {
        return false;
    }

    if (!is_running_regression) {
        std::printf(">> %s\n", utils::to_pipe_delimited(message).c_str());
    }

    if (!socket.send_bytes(message)) {
        return false;
    }

    last_send_ms = utils::get_monotonic_millis();
    store_sent_message(message);
    return true;
}

static bool process_inbound_message(TcpSocket& socket,
                                    FixMessage& fix,
                                    int& outbound_seq,
                                    uint64_t& last_send_ms,
                                    const std::string& inbound_message,
                                    bool& logon_accepted,
                                    bool& stop_requested,
                                    bool scenarios_sent,
                                    bool& scenario_response_started,
                                    uint64_t& last_scenario_response_ms,
                                    bool& logout_initiated,
                                    const std::string& token_path) {

    if (!is_running_regression) {
        std::printf("<< %s\n", utils::to_pipe_delimited(inbound_message).c_str());
    }

    std::string msg_type;
    if (!utils::find_tag_value(inbound_message, "35=", msg_type)) {
        // Not a valid FIX message (or missing 35). Ignore it.
        return true;
    }

    if (!logon_accepted && msg_type == "A") {
        logon_accepted = true;
        return true;
    }

    // Initiate Logout Handsake
    // after scenario finished
    if (scenarios_sent && !logout_initiated) {
        const bool is_admin_msg =
            (msg_type == "0" || msg_type == "1" || msg_type == "2" ||
             msg_type == "4" || msg_type == "A" || msg_type == "5");

        if (scenarios_sent && !is_admin_msg) {
            scenario_response_started = true;
            last_scenario_response_ms = utils::get_monotonic_millis();
        }
    }

    // TestRequest (35=1) -> Heartbeat (35=0) with same 112 (if present)
    if (msg_type == "1") {
        std::string test_req_id;
        utils::find_tag_value(inbound_message, "112=", test_req_id);

        const std::string heartbeat = fix.build_heartbeat(outbound_seq,
                                                          utils::get_utc_timestamp(),
                                                          test_req_id);

        if (!send_fix_message(socket, heartbeat, last_send_ms)) {
            return false;
        }

        outbound_seq++;
        save_token(token_path, outbound_seq, g_expected_incoming_seq);
        return true;
    }

    // ResendRequest (35=2) -> Resend stored messages with PossDupFlag
    if (msg_type == "2") {
        std::string begin_str, end_str;
        utils::find_tag_value(inbound_message, "7=", begin_str);
        utils::find_tag_value(inbound_message, "16=", end_str);

        const int begin_seq = std::atoi(begin_str.c_str());
        const int end_seq = std::atoi(end_str.c_str());
        const int actual_end = (end_seq == 0) ? (outbound_seq - 1) : end_seq;

        int gap_start = -1;

        for (int seq = begin_seq; seq <= actual_end; ++seq) {
            const std::string stored = load_stored_message(seq);
            bool is_resendable = false;

            if (!stored.empty()) {
                std::string orig_msg_type;
                utils::find_tag_value(stored, "35=", orig_msg_type);

                // Admin messages get gap-filled, not resent
                if (orig_msg_type != "A" && orig_msg_type != "0" &&
                    orig_msg_type != "1" && orig_msg_type != "2" &&
                    orig_msg_type != "4" && orig_msg_type != "5") {
                    is_resendable = true;
                }
            }

            if (!is_resendable) {
                // Accumulate into gap range
                if (gap_start < 0) {
                    gap_start = seq;
                }
                continue;
            }

            // Flush pending gap before resending
            if (gap_start >= 0) {
                const std::string gap_fill = fix.build_sequence_reset(
                    gap_start, utils::get_utc_timestamp(), seq, true);

                if (!send_fix_message(socket, gap_fill, last_send_ms)) {
                    return false;
                }
                gap_start = -1;
            }

            // Resend the business message
            std::string orig_sending_time;
            utils::find_tag_value(stored, "52=", orig_sending_time);

            std::string orig_msg_type;
            utils::find_tag_value(stored, "35=", orig_msg_type);

            FixMessage::FieldList body_fields;
            body_fields.push_back(FixMessage::Field(43, "Y"));
            body_fields.push_back(FixMessage::Field(122, orig_sending_time));

            size_t pos = 0;
            while (pos < stored.size()) {
                size_t soh = stored.find('\x01', pos);
                if (soh == std::string::npos) soh = stored.size();

                const std::string field = stored.substr(pos, soh - pos);
                pos = soh + 1;

                const size_t eq = field.find('=');
                if (eq == std::string::npos) continue;

                const int tag = std::atoi(field.substr(0, eq).c_str());
                const std::string val = field.substr(eq + 1);

                if (tag == 8 || tag == 9 || tag == 10 || tag == 34 ||
                    tag == 35 || tag == 49 || tag == 56 || tag == 52 ||
                    tag == 43 || tag == 122) {
                    continue;
                }

                body_fields.push_back(FixMessage::Field(tag, val));
            }

            const std::string resend = fix.build_message(
                orig_msg_type, seq, utils::get_utc_timestamp(), body_fields);

            if (!send_fix_message(socket, resend, last_send_ms)) {
                return false;
            }
        }

        // Flush any trailing gap
        if (gap_start >= 0) {
            const std::string gap_fill = fix.build_sequence_reset(
                gap_start, utils::get_utc_timestamp(), actual_end + 1, true);

            if (!send_fix_message(socket, gap_fill, last_send_ms)) {
                return false;
            }
        }

        return true;
    }

    // Logout (35=5) -> reply Logout and stop
    if (msg_type == "5") {
        if (!logout_initiated) {
            const std::string logout = fix.build_logout(outbound_seq,
                                                    utils::get_utc_timestamp(),
                                                    "");

            send_fix_message(socket, logout, last_send_ms);
            outbound_seq++;
            save_token(token_path, outbound_seq, g_expected_incoming_seq);
        }

        stop_requested = true;
        return true;
    }

    return true;
}

// load custom
// RAW FIX messages
// from template file
static bool run_scenarios(TcpSocket& socket, FixMessage& fix,
                          const SessionConfig& config,
                          const std::string& scenario_path, int& outbound_seq,
                          uint64_t& last_send_ms, bool& scenarios_sent,
                          const std::string& token_path) {

    scenarios_sent = false;
    std::vector<std::string> files;

    DIR* dir = ::opendir(scenario_path.c_str());
    if (dir) {
        dirent* entry = 0;
        while ((entry = ::readdir(dir)) != 0) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") {
                continue;
            }

            // Ignore hidden files and folder
            if (!name.empty() && name[0] == '.') {
                continue;
            }

            files.push_back(scenario_path + "/" + name);
        }

        ::closedir(dir);
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(scenario_path);
    }

    for (size_t i = 0; i < files.size(); i++) {
        const std::string& file_path = files[i];

        std::ifstream in(file_path.c_str());
        if (!in.is_open()) {
            continue;
        }

        FixTemplateRuntime runtime;
        runtime.begin_string = config.begin_string;
        runtime.sender_comp_id = config.sender_comp_id;
        runtime.target_comp_id = config.target_comp_id;
        runtime.msg_seq_num = 0;
        runtime.sending_time_utc.clear();
        runtime.state.org_clord_id.clear();

        std::string line;
        while (std::getline(in, line)) {
            line = utils::trim(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }

            FixTemplateMessage template_message;
            template_message.msg_type.clear();
            template_message.fields.clear();

            // Parse RAW messages
            size_t pos = 0;
            while (pos < line.size()) {
                size_t end = line.find('|', pos);
                if (end == std::string::npos) {
                    end = line.size();
                }

                const std::string field_text = line.substr(pos, end - pos);
                pos = (end < line.size()) ? (end + 1) : end;

                if (field_text.empty()) {
                    continue;
                }

                const size_t eq = field_text.find('=');
                if (eq == std::string::npos) {
                    continue;
                }

                const std::string tag_text = field_text.substr(0, eq);
                const std::string value_text = field_text.substr(eq + 1);

                const int tag_value = std::atoi(tag_text.c_str());
                if (tag_value <= 0) {
                    continue;
                }

                template_message.fields.push_back(std::make_pair(tag_value, value_text));
                if (tag_value == 35 && template_message.msg_type.empty()) {
                    template_message.msg_type = value_text;
                }
            }

            if (template_message.fields.empty()) {
                continue;
            }

            runtime.msg_seq_num = outbound_seq;
            runtime.sending_time_utc = utils::get_utc_timestamp();

            fix_template_apply(runtime, template_message);
            const std::string raw_fix = fix.build_from_fields(template_message.fields);

            if (!send_fix_message(socket, raw_fix, last_send_ms)) {
                return false;
            }

            scenarios_sent = true;
            outbound_seq++;
            save_token(token_path, outbound_seq, g_expected_incoming_seq);
        }
    }

    return true;
}

bool read_next_business_message(TcpSocket& socket,
                               FixParser& fix_parser,
                               FixMessage& fix,
                               int& outbound_seq,
                               uint64_t& last_send_ms,
                               const std::string& token_path,
                               bool& logon_accepted,
                               bool& stop_requested,
                               bool scenarios_sent,
                               bool& scenario_response_started,
                               uint64_t& last_scenario_response_ms,
                               bool& logout_initiated,
                               int timeout_ms,
                               std::string& out_message) {
    out_message.clear();

    const uint64_t start_ms = utils::get_monotonic_millis();
    char receive_buffer[receive_buffer_size];

    while (utils::get_monotonic_millis() - start_ms < static_cast<uint64_t>(timeout_ms)) {

        // Drain already-buffered messages
        std::string inbound_message;
        while (fix_parser.read_next_message(inbound_message)) {
            if (!process_inbound_message(socket, fix, outbound_seq, last_send_ms,
                                         inbound_message, logon_accepted, stop_requested,
                                         scenarios_sent, scenario_response_started,
                                         last_scenario_response_ms, logout_initiated,
                                         token_path)) {
                return false;
            }

            if (stop_requested) {
                out_message = inbound_message;
                return true;
            }

            std::string msg_type;
            if (!utils::find_tag_value(inbound_message, "35=", msg_type)) {
                continue;
            }

            const bool is_admin_msg =
                (msg_type == "0" || msg_type == "1" || msg_type == "2" ||
                 msg_type == "4" || msg_type == "A" || msg_type == "5");

            if (!is_admin_msg) {
                out_message = inbound_message;
                return true;
            }
        }

        // No buffered messages -> read more bytes from socket
        const int bytes_received = socket.receive_bytes(receive_buffer, sizeof(receive_buffer));

        if (bytes_received == peer_closed) {
            return false;
        }

        if (bytes_received < peer_closed) {
            if (recv_timed_out()) {
                continue;
            }
            return false;
        }

        fix_parser.append_bytes(receive_buffer, static_cast<size_t>(bytes_received));
    }

    return true;
}

int Application::run(const AppArgs& args) {
    ConfigParser config_parser;
    config_parser.load(args.config_path);

    SessionConfig config = config_parser.get_session(args.session_name);

    if (config.heartbeat_interval <= 0) {
        std::printf("Error: heartbeat_interval must be > 0 in config\n");
        return 1;
    }

    if (!socket.connect(config.host, config.port)) {
        std::printf("Error: Connection failed\n");
        return 1;
    }

    std::printf("Info: Connected to %s:%d\n", config.host.c_str(), config.port);

    const int sock_fd = socket.get_fd();
    if (sock_fd < 0) {
        std::printf("Error: invalid socket\n");
        socket.close();
        return 1;
    }

    if (!set_socket_recv_timeout(sock_fd, receive_timeout_millis)) {
        std::printf("Error: failed to set SO_RCVTIMEO\n");
        socket.close();
        return 1;
    }

    FixMessage fix;
    fix.set_begin_string(config.begin_string);
    fix.set_sender_comp_id(config.sender_comp_id);
    fix.set_target_comp_id(config.target_comp_id);

    store_msgs_enabled = args.store;
    store_sender_comp_id = config.sender_comp_id;

    bool is_live = args.live;
    if (!config.username.empty()) {
        is_live = true;
    }

    FixParser fix_parser;

    const uint64_t heartbeat_interval_ms =
        static_cast<uint64_t>(config.heartbeat_interval) * 1000ULL;

    uint64_t last_send_ms = utils::get_monotonic_millis();
    uint64_t last_recv_ms = last_send_ms;

    // 0 = no TestRequest pending, non-zero = sent and waiting for inbound
    uint64_t test_request_sent_ms = 0;
    int test_request_counter = 1;

    int outbound_seq = 1;
    int expected_incoming_seq = 1;

    // Read Token(Sequence)
    // form file
    const std::string now_utc = utils::get_utc_timestamp();
    std::string token_path;

    if (!read_token("tokens",
                    config.sender_comp_id,
                    now_utc,
                    config.reset_on_logon,
                    outbound_seq,
                    expected_incoming_seq,
                    token_path)) {
        
        std::printf("ERROR: Token read failed\n");
        socket.close();
        return 1;
    }

    g_expected_incoming_seq = expected_incoming_seq;
    g_token_path = token_path;

    if (args.recover) {
        g_expected_incoming_seq = 1;
    }

    //scenario logout state
    bool scenarios_sent = false;
    bool logout_initiated = false;
    uint64_t logout_start_ms = 0;

    // Wait to send logout
    // before receiving
    // all response
    bool scenario_response_started = false;
    uint64_t last_scenario_response_ms = 0;
    const uint64_t scenario_quiet_ms = 300ULL;

    // Timeout on heartbaet
    uint64_t scenario_sent_ms = 0;
    const uint64_t scenario_first_response_timeout_ms = 5000ULL;

    char receive_buffer[receive_buffer_size];

    // Send Logon
    const std::string logon = fix.build_logon(outbound_seq,
                                              utils::get_utc_timestamp(),
                                              config.heartbeat_interval,
                                              config.reset_on_logon,
                                              config.username,
                                              config.password);

    if (!send_fix_message(socket, logon, last_send_ms)) {
        socket.close();
        return 1;
    }

    outbound_seq++;
    save_token(token_path, outbound_seq, g_expected_incoming_seq);

    // Wait for Logon Ack (35=A)
    const uint64_t logon_start_ms = utils::get_monotonic_millis();
    const uint64_t logon_timeout_ms =
        static_cast<uint64_t>(logon_timeout_seconds) * 1000ULL;

    bool logon_accepted = false;

    while (!logon_accepted) {
        const uint64_t now_ms = utils::get_monotonic_millis();
        if (now_ms - logon_start_ms >= logon_timeout_ms) {
            std::printf("Error: logon timeout (no 35=A)\n");
            socket.close();
            return 1;
        }

        const int bytes_received = socket.receive_bytes(receive_buffer, sizeof(receive_buffer));

        if (bytes_received == peer_closed) {
            std::printf("Info: peer closed\n");
            socket.close();
            return 1;
        }

        if (bytes_received < peer_closed) {
            if (recv_timed_out()) {
                continue;
            }
            std::printf("Error: receive failed\n");
            socket.close();
            return 1;
        }

        last_recv_ms = utils::get_monotonic_millis();
        test_request_sent_ms = 0;

        fix_parser.append_bytes(receive_buffer, static_cast<size_t>(bytes_received));

        std::string inbound_message;
        while (fix_parser.read_next_message(inbound_message)) {
            bool stop_requested = false;

            if (!process_inbound_message(socket, fix, outbound_seq, last_send_ms,
                                         inbound_message, logon_accepted, stop_requested,
                                         scenarios_sent, scenario_response_started, last_scenario_response_ms,
                                         logout_initiated, token_path)) {
                socket.close();
                return 1;
            }

            // Detect incoming sequence gap after logon accepted
            if (logon_accepted) {
                std::string logon_seq_str;
                if (utils::find_tag_value(inbound_message, "34=", logon_seq_str)) {
                    const int logon_seq = std::atoi(logon_seq_str.c_str());

                    // Incoming gap: request missing messages
                    if (logon_seq > g_expected_incoming_seq) {
                        const std::string resend_req = fix.build_resend_request(
                            outbound_seq, utils::get_utc_timestamp(),
                            g_expected_incoming_seq, 0);

                        if (!send_fix_message(socket, resend_req, last_send_ms)) {
                            socket.close();
                            return 1;
                        }

                        outbound_seq++;
                        save_token(token_path, outbound_seq, g_expected_incoming_seq);
                    }

                    // Sequence error: server sent lower seq than expected
                    // disconnect to prevent data integrity issues
                    if (logon_seq < g_expected_incoming_seq) {
                        std::printf("Error: sequence too low, expected=%d received=%d\n",
                                    g_expected_incoming_seq, logon_seq);

                        const std::string logout = fix.build_logout(outbound_seq,
                            utils::get_utc_timestamp(),
                            "MsgSeqNum too low");

                        send_fix_message(socket, logout, last_send_ms);
                        socket.close();
                        return 1;
                    }
                }
            }

            // Track incoming sequence number
            std::string recv_seq_str;
            if (utils::find_tag_value(inbound_message, "34=", recv_seq_str)) {
                const int recv_seq = std::atoi(recv_seq_str.c_str());
                if (recv_seq >= g_expected_incoming_seq) {
                    g_expected_incoming_seq = recv_seq + 1;
                    save_token(token_path, outbound_seq, g_expected_incoming_seq);
                }
            }

            if (stop_requested) {
                socket.close();
                return 0;
            }

            if (logon_accepted) {
                break;
            }
        }
    }

    // Send Scenarios/regression test
    // after logon is accepted
    if (args.is_test_mode) {
        is_running_regression = true;

        if (!run_fix_regression(socket, fix_parser, fix,
                                args.scenario_path,
                                outbound_seq, last_send_ms, token_path,
                                logon_accepted, scenarios_sent,
                                scenario_response_started, last_scenario_response_ms,
                                logout_initiated)) {

            is_running_regression = false;
            socket.close();
            return 1;
        }

        is_running_regression = false;
    }
    else { 
        if (config.username.empty()) {
            if (!run_scenarios(socket, fix, config,
                               args.scenario_path, outbound_seq,
                               last_send_ms, scenarios_sent, token_path)) {
                socket.close();
                return 1;
            }
        }
    }

    scenario_sent_ms = utils::get_monotonic_millis();

    // Main loop: keepalive + admin message handling
    logon_accepted = true;
    while (true) {
        const uint64_t now_ms = utils::get_monotonic_millis();

        // If no business response at all 
        // after sending scenarios
        // logout
        if (!logout_initiated && scenarios_sent && !scenario_response_started) {
            if (now_ms - scenario_sent_ms >= scenario_first_response_timeout_ms) {
                if (!is_live) {
                    const std::string logout = fix.build_logout(outbound_seq, utils::get_utc_timestamp(), "");
                    if (!send_fix_message(socket, logout, last_send_ms)) {
                        break;
                    }

                    outbound_seq++;
                    save_token(token_path, outbound_seq, g_expected_incoming_seq);
                    logout_initiated = true;
                    logout_start_ms = now_ms;
                }
            }
        }

        // Check if there is no response
        // initate logout
        if (!logout_initiated && scenarios_sent && scenario_response_started) {
            if (now_ms - last_scenario_response_ms >= scenario_quiet_ms) {
                if (!is_live) {
                    const std::string logout = fix.build_logout(outbound_seq, utils::get_utc_timestamp(), "");

                    if (!send_fix_message(socket, logout, last_send_ms)) {
                        break;
                    }

                    outbound_seq++;
                    save_token(token_path, outbound_seq, g_expected_incoming_seq);
                    logout_initiated = true;
                    logout_start_ms = now_ms;
                }
            }
        }

        if (logout_initiated) {
            if (now_ms - logout_start_ms >= 2000ULL) {
                std::printf("Info: logout wait timeout, closing\n");
                break;
            }
        }
        else {
            // TestRequest timeout check
            if (test_request_sent_ms != 0) {
                if (now_ms - test_request_sent_ms >= heartbeat_interval_ms) {
                    std::printf("Error: TestRequest timeout\n");
                    break;
                }
            } else {
                // No inbound for interval -> send TestRequest
                if (now_ms - last_recv_ms >= heartbeat_interval_ms) {
                    char test_req_id_buf[32];
                    std::snprintf(test_req_id_buf, sizeof(test_req_id_buf), "TR%d", test_request_counter++);
                    const std::string test_req_id(test_req_id_buf);

                    const std::string test_request = fix.build_test_request(outbound_seq,
                                                                            utils::get_utc_timestamp(),
                                                                            test_req_id);

                    if (!send_fix_message(socket, test_request, last_send_ms)) {
                        break;
                    }

                    outbound_seq++;
                    save_token(token_path, outbound_seq, g_expected_incoming_seq);
                    test_request_sent_ms = utils::get_monotonic_millis();
                }
            }

            // No outbound for interval -> send Heartbeat
            if (now_ms - last_send_ms >= heartbeat_interval_ms) {
                const std::string heartbeat = fix.build_heartbeat(outbound_seq,
                                                                  utils::get_utc_timestamp(),
                                                                  "");

                if (!send_fix_message(socket, heartbeat, last_send_ms)) {
                    break;
                }

                outbound_seq++;
                save_token(token_path, outbound_seq, g_expected_incoming_seq);
            }
        }

        const int bytes_received = socket.receive_bytes(receive_buffer, sizeof(receive_buffer));

        if (bytes_received == peer_closed) {
            std::printf("Info: peer closed\n");
            break;
        }

        if (bytes_received < peer_closed) {
            if (recv_timed_out()) {
                continue;
            }
            if (errno == ECONNRESET || errno == EPIPE) {
                std::printf("Info: server disconnected\n");
                break;
            }

            std::printf("Error: receive failed\n");
            break;
        }

        last_recv_ms = utils::get_monotonic_millis();
        test_request_sent_ms = 0;

        fix_parser.append_bytes(receive_buffer, static_cast<size_t>(bytes_received));

        std::string inbound_message;
        while (fix_parser.read_next_message(inbound_message)) {
            bool stop_requested = false;

            if (!process_inbound_message(socket, fix, outbound_seq, last_send_ms,
                                         inbound_message, logon_accepted, stop_requested,
                                         scenarios_sent, scenario_response_started, last_scenario_response_ms,
                                         logout_initiated, token_path)) {
                socket.close();
                return 1;
            }

            // Track incoming sequence number
            std::string recv_seq_str;
            if (utils::find_tag_value(inbound_message, "34=", recv_seq_str)) {
                const int recv_seq = std::atoi(recv_seq_str.c_str());
                if (recv_seq >= g_expected_incoming_seq) {
                    g_expected_incoming_seq = recv_seq + 1;
                    save_token(token_path, outbound_seq, g_expected_incoming_seq);
                }
            }

            if (stop_requested) {
                socket.close();
                return 0;
            }
        }
    }

    socket.close();
    return 0;
}
