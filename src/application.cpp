#include "application.h"
#include "config_parser.h"
#include "fix_parser.h"
#include "fix_message.h"
#include "utils.h"

#include <cstdio>
#include <string>
#include <sys/select.h>
#include <unistd.h>

int Application::run(const AppArgs& args) {
    ConfigParser config_parser;
    config_parser.load(args.config_path);

    SessionConfig config = config_parser.get_session(args.session_name);

    if (!socket.connect(config.host, config.port)) {
        std::printf("Error: Connection failed\n");
        return 1;
    }

    std::printf("Info: Connected to %s:%d\n", config.host.c_str(), config.port);

    FixMessage fix;
    fix.set_begin_string(config.begin_string);
    fix.set_sender_comp_id(config.sender_comp_id);
    fix.set_target_comp_id(config.target_comp_id);

    FixParser fix_parser;

    int outbound_seq = 1;

    // Send Logon
    const std::string logon = fix.build_logon(outbound_seq,
                                              utils::get_utc_timestamp(),
                                              config.heartbeat_interval,
                                              config.reset_on_logon);

    if (logon.empty()) {
        socket.close();
        return 1;
    }

    std::printf(">> %s\n", utils::to_pipe_delimited(logon).c_str());

    if (!socket.send_bytes(logon)) {
        socket.close();
        return 1;
    }

    outbound_seq++;

    bool logon_accepted = false;
    char recv_buf[4096];
    const int peer_closed = 0;

    while (true) {
        const int bytes_received = socket.receive_bytes(recv_buf, sizeof(recv_buf));

        if (bytes_received == peer_closed) {
            std::printf("Info: peer closed\n");
            break;
        }

        if (bytes_received < peer_closed) {
            std::printf("Error: receive failed\n");
            break;
        }

        fix_parser.append_bytes(recv_buf, static_cast<size_t>(bytes_received));
        std::string inbound_msg;
        while (fix_parser.read_next_message(inbound_msg)) {
            std::printf("<< %s\n", utils::to_pipe_delimited(inbound_msg).c_str());

            // Read MessageType 
            std::string msg_type;
            if (!utils::find_tag_value(inbound_msg, "35=", msg_type)) {
                continue;
            }

            // Logon Ack
            if (!logon_accepted && msg_type == "A") {
                logon_accepted = true;
                std::printf("Info: Logon accepted\n");
                continue;
            }

            // TestRequest (35=1)
            // reply with HeartBeat (35=0)
            // and same 112
            if (msg_type == "1") {
                std::string test_req_id;
                utils::find_tag_value(inbound_msg, "112=", test_req_id);
                const std::string heartbeat = fix.build_heartbeat(outbound_seq,
                                                                  utils::get_utc_timestamp(),
                                                                  test_req_id);

                std::printf(">> %s\n", utils::to_pipe_delimited(heartbeat).c_str());
                socket.send_bytes(heartbeat);
                outbound_seq++;
                continue;
            }

            // Logout
            if (msg_type == "5") {
                std::printf("Info: received Logout\n");

                const std::string logout = fix.build_logout(outbound_seq,
                                                            utils::get_utc_timestamp(),
                                                            "");

                if (!logout.empty()) {
                    std::printf(">> %s\n", utils::to_pipe_delimited(logout).c_str());
                    socket.send_bytes(logout);
                    outbound_seq++;
                }

                socket.close();
                return 0;
            }

            if (!logon_accepted) {
                continue;
            }
        }
    }

    socket.close();
    return 0;
}
