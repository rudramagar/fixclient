#include "application.h"
#include "config_parser.h"
#include "fix_parser.h"
#include "fix_message.h"
#include "utils.h"

#include <cstdio>
#include <string>
#include <cstdint>

#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>

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
                                    bool& stop_requested) {
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
        return true;
    }

    // Logout (35=5) -> reply Logout and stop
    if (msg_type == "5") {
        std::printf("Info: received logout\n");

        const std::string logout = fix.build_logout(outbound_seq,
                                                    utils::get_utc_timestamp(),
                                                    "");
        send_fix_message(socket, logout, last_send_ms);
        outbound_seq++;

        stop_requested = true;
        return true;
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
                                         inbound_message, logon_accepted, stop_requested)) {
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

    // Main loop: keepalive + admin message handling
    logon_accepted = true;

    while (true) {
        const uint64_t now_ms = utils::get_monotonic_millis();

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
                                         inbound_message, logon_accepted, stop_requested)) {
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
