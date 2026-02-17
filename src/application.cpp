#include "application.h"
#include "config_parser.h"
#include "fix_parser.h"
#include "fix_message.h"
#include "fix_template.h"
#include "token_handler.h"
#include "utils.h"
#include <cstdio>
#include <string>
#include <cstdint>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <fstream>

const int peer_closed = 0;
const size_t receive_buffer_size = 4096;

const int logon_timeout_seconds = 5;
const int receive_timeout_millis = 200;

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

    std::printf(">> %s\n", utils::to_pipe_delimited(message).c_str());

    if (!socket.send_bytes(message)) {
        return false;
    }

    last_send_ms = utils::get_monotonic_millis();
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
                                    uint64_t& logout_start_ms,
                                    const std::string& token_path) {

    std::printf("<< %s\n", utils::to_pipe_delimited(inbound_message).c_str());

    std::string msg_type;
    if (!utils::find_tag_value(inbound_message, "35=", msg_type)) {
        // Not a valid FIX message (or missing 35). Ignore it.
        return true;
    }

    if (!logon_accepted && msg_type == "A") {
        logon_accepted = true;
        std::printf("Info: Logon accepted\n");
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
        save_token(token_path, outbound_seq);
        return true;
    }

    // Logout (35=5) -> reply Logout and stop
    if (msg_type == "5") {
        std::printf("Info: received logout\n");

        if (!logout_initiated) {
            const std::string logout = fix.build_logout(outbound_seq,
                                                    utils::get_utc_timestamp(),
                                                    "");

            send_fix_message(socket, logout, last_send_ms);
            outbound_seq++;
            save_token(token_path, outbound_seq);
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
            save_token(token_path, outbound_seq);
        }
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

    FixParser fix_parser;

    const uint64_t heartbeat_interval_ms =
        static_cast<uint64_t>(config.heartbeat_interval) * 1000ULL;

    uint64_t last_send_ms = utils::get_monotonic_millis();
    uint64_t last_recv_ms = last_send_ms;

    // 0 = no TestRequest pending, non-zero = sent and waiting for inbound
    uint64_t test_request_sent_ms = 0;
    int test_request_counter = 1;

    int outbound_seq = 1;

    // Read Token(Sequence)
    // form file
    const std::string now_utc = utils::get_utc_timestamp();
    std::string token_path;

    if (!read_token("tokens",
                    config.sender_comp_id,
                    now_utc,
                    config.reset_on_logon,
                    outbound_seq,
                    token_path)) {
        
        std::printf("ERROR: Token read failed\n");
        socket.close();
        return 1;
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
                                              config.reset_on_logon);

    if (!send_fix_message(socket, logon, last_send_ms)) {
        socket.close();
        return 1;
    }

    outbound_seq++;
    save_token(token_path, outbound_seq);

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
                                         logout_initiated, logout_start_ms, token_path)) {
                socket.close();
                return 1;
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

    // Send Scenarios after logon is accepted
    if (!run_scenarios(socket, fix, config, args.scenario_path, outbound_seq, last_send_ms, scenarios_sent, token_path)) {
        socket.close();
        return 1;
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
                const std::string logout = fix.build_logout(outbound_seq, utils::get_utc_timestamp(), "");
                if (!send_fix_message(socket, logout, last_send_ms)) {
                    break;
                }

                outbound_seq++;
                save_token(token_path, outbound_seq);
                logout_initiated = true;
                logout_start_ms = now_ms;
            }
        }

        // Check if there is no response
        // initate logout
        if (!logout_initiated && scenarios_sent && scenario_response_started) {
            if (now_ms - last_scenario_response_ms >= scenario_quiet_ms) {
                const std::string logout = fix.build_logout(outbound_seq, utils::get_utc_timestamp(), "");

                if (!send_fix_message(socket, logout, last_send_ms)) {
                    break;
                }

                outbound_seq++;
                save_token(token_path, outbound_seq);
                logout_initiated = true;
                logout_start_ms = now_ms;
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
                    save_token(token_path, outbound_seq);
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
                save_token(token_path, outbound_seq);
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
                                         logout_initiated, logout_start_ms, token_path)) {
                socket.close();
                return 1;
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
