#include "fix_regression.h"
#include "token_handler.h"
#include "utils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <dirent.h>

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
                               uint64_t& logout_start_ms,
                               int timeout_ms,
                               std::string& out_message);

const int timeout_test_ms = 3000;
const int timeout_discard_ms = 500;
const int max_clr = 50;

static void print_details(const std::string& fix) {
    for (size_t i = 0; i < fix.size(); ++i) {
        char ch = fix[i];
        if (ch == '\x01') ch = '|';
        std::putchar(ch);
    }
}

static void build_clr_id(std::string clr_values[max_clr + 1], int scenario_index) {
    for (int i = 0; i <= max_clr; ++i) {
        clr_values[i].clear();
    }

    const unsigned long long base =
        static_cast<unsigned long long>(utils::get_monotonic_millis()) * 1000ULL +
        static_cast<unsigned long long>(scenario_index % 1000);

    for (int i = 1; i <= max_clr; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%llu_%d", base, i);
        clr_values[i] = buf;
    }
}

static void parse_fields(const std::string& payload,
                         FixMessage::FieldList& fields,
                         std::string* msg_type_out) {
    fields.clear();
    fields.reserve(64);
    if (msg_type_out) msg_type_out->clear();

    size_t pos = 0;
    while (pos < payload.size()) {
        size_t end = payload.find('|', pos);
        if (end == std::string::npos) end = payload.size();

        std::string token = utils::trim(payload.substr(pos, end - pos));
        pos = (end < payload.size()) ? (end + 1) : end;

        if (token.empty()) continue;

        const size_t eq = token.find('=');
        if (eq == std::string::npos || eq == 0) continue;

        const int tag = std::atoi(token.substr(0, eq).c_str());
        if (tag <= 0) continue;

        const std::string val = token.substr(eq + 1);
        fields.push_back(std::make_pair(tag, val));

        if (msg_type_out && tag == 35 && msg_type_out->empty()) {
            *msg_type_out = val;
        }
    }
}

static bool run_file(const std::string& file_path,
                     TcpSocket& socket,
                     FixParser& fix_parser,
                     FixMessage& fix,
                     int& outbound_seq,
                     uint64_t& last_send_ms,
                     const std::string& token_path,
                     bool& logon_accepted,
                     bool& scenarios_sent,
                     bool& scenario_response_started,
                     uint64_t& last_scenario_response_ms,
                     bool& logout_initiated,
                     uint64_t& logout_start_ms,
                     int& total_run,
                     int& total_passed,
                     int& total_failed,
                     std::vector<std::string>& failed_names) {
    std::ifstream in(file_path.c_str());
    if (!in.is_open()) {
        std::printf("ERROR: Cannot open regression file: %s\n", file_path.c_str());
        return false;
    }

    int scenario_index = 0;
    int step = 0;

    bool in_scenario = false;
    bool scenario_ok = true;

    std::string scenario_name;
    std::string clr_values[max_clr + 1];

    std::string line;
    while (std::getline(in, line)) {
        line = utils::trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/') continue;

        const size_t first_bar = line.find('|');
        if (first_bar == std::string::npos) {
            continue; 
        }

        std::string cmd = utils::trim(line.substr(0, first_bar));
        for (size_t i = 0; i < cmd.size(); ++i) {
            const char c = cmd[i];
            if (c >= 'a' && c <= 'z') cmd[i] = static_cast<char>(c - ('a' - 'A'));
        }

        if (cmd != "BGN" && cmd != "SND" && cmd != "TST" && cmd != "RCV" && cmd != "END") {
            continue;
        }

        const size_t second_bar = line.find('|', first_bar + 1);
        const std::string payload = (second_bar == std::string::npos)
            ? utils::trim(line.substr(first_bar + 1))
            : utils::trim(line.substr(second_bar + 1));

        if (cmd == "BGN") {
            scenario_index++;
            build_clr_id(clr_values, scenario_index);

            scenario_name = payload;
            in_scenario = true;
            scenario_ok = true;
            step = 0;

            std::printf("\nBEGIN %s\n", scenario_name.c_str());
            continue;
        }

        if (cmd == "END") {
            if (!in_scenario) continue;

            total_run++;
            if (scenario_ok) {
                total_passed++;
                std::printf("END %s  RESULT=PASS\n", scenario_name.c_str());
            } else {
                total_failed++;
                failed_names.push_back(scenario_name);
                std::printf("END %s  RESULT=FAIL\n", scenario_name.c_str());
            }

            in_scenario = false;
            continue;
        }

        if (!in_scenario) continue;

        if (cmd == "RCV") {
            std::string msg;
            bool stop_requested = false;

            if (!read_next_business_message(socket, fix_parser, fix,
                                            outbound_seq, last_send_ms, token_path,
                                            logon_accepted, stop_requested,
                                            scenarios_sent,
                                            scenario_response_started, last_scenario_response_ms,
                                            logout_initiated, logout_start_ms,
                                            timeout_discard_ms, msg)) {
                return false;
            }

            step++;
            std::printf("  %d  RCV\n", step);
            continue;
        }

        if (cmd == "SND") {
            FixMessage::FieldList raw;
            std::string msg_type;
            parse_fields(payload, raw, &msg_type);

            FixMessage::FieldList body;
            body.reserve(raw.size());

            const std::string sending_time = utils::get_utc_timestamp();

            for (size_t i = 0; i < raw.size(); ++i) {
                const int tag = raw[i].first;
                std::string value = raw[i].second;

                if (tag == 35) continue;

                // ignore header/trailer in file
                if (tag == 8 || tag == 9 || tag == 10 || tag == 34 || tag == 49 || tag == 52 || tag == 56) {
                    continue;
                }

                if (value.size() >= 4 && value[0] == 'c' && value[1] == 'l' && value[2] == 'r') {
                    int n = 0;
                    bool ok = true;
                    for (size_t j = 3; j < value.size(); ++j) {
                        const char ch = value[j];
                        if (ch < '0' || ch > '9') { ok = false; break; }
                        n = (n * 10) + (ch - '0');
                    }
                    if (ok && n >= 1 && n <= max_clr) {
                        value = clr_values[n];
                    }
                }

                if (tag == 60 && value.empty()) {
                    value = sending_time;
                }

                body.push_back(std::make_pair(tag, value));
            }

            const std::string msg = fix.build_message(msg_type, outbound_seq, sending_time, body);
            if (msg.empty()) {
                std::printf("Error: failed to build SND message\n");
                return false;
            }

            if (!socket.send_bytes(msg)) {
                return false;
            }

            last_send_ms = utils::get_monotonic_millis();
            outbound_seq++;
            save_token(token_path, outbound_seq);
            scenarios_sent = true;

            step++;
            std::printf("  %d  SEND: %s\n", step, payload.c_str());
            continue;
        }

        if (cmd == "TST") {
            FixMessage::FieldList expected;
            parse_fields(payload, expected, 0);

            step++;
            std::printf("  %d  TEST: %s\n", step, payload.c_str());

            std::string msg;
            bool stop_requested = false;

            if (!read_next_business_message(socket, fix_parser, fix,
                                            outbound_seq, last_send_ms, token_path,
                                            logon_accepted, stop_requested,
                                            scenarios_sent,
                                            scenario_response_started, last_scenario_response_ms,
                                            logout_initiated, logout_start_ms,
                                            timeout_test_ms, msg)) {
                return false;
            }

            step++;
            if (msg.empty()) {
                std::printf("  %d  RECV: (TIMEOUT)\n", step);
                std::printf("     STATE,  EXPECTED,  RECEIVED\n");
                scenario_ok = false;
                continue;
            }

            std::printf("  %d  RECV: ", step);
            print_details(msg);
            std::printf("\n");

            std::printf("     STATE,  EXPECTED,  RECEIVED\n");

            for (size_t i = 0; i < expected.size(); ++i) {
                const int tag = expected[i].first;
                const std::string& exp_text = expected[i].second;

                std::string exp_val = exp_text;
                if (exp_text.size() >= 4 && exp_text[0] == 'c' && exp_text[1] == 'l' && exp_text[2] == 'r') {
                    int n = 0;
                    bool ok = true;
                    for (size_t j = 3; j < exp_text.size(); ++j) {
                        const char ch = exp_text[j];
                        if (ch < '0' || ch > '9') { ok = false; break; }
                        n = (n * 10) + (ch - '0');
                    }
                    if (ok && n >= 1 && n <= max_clr) {
                        exp_val = clr_values[n];
                    }
                }

                char prefix[16];
                std::snprintf(prefix, sizeof(prefix), "%d=", tag);

                std::string act_val;
                const bool has = utils::find_tag_value(msg, prefix, act_val);

                bool match = false;
                if (exp_text == "IGNORE") {
                    match = true;
                } else if (exp_text == "NONE") {
                    match = (!has || act_val.empty() || act_val == "NONE");
                } else {
                    match = has && (act_val == exp_val);
                }

                std::printf("     %s   %d=%s,   ", match ? "OK" : "FAIL", tag, exp_text.c_str());
                if (!has) {
                    std::printf("%d=MISSING\n", tag);
                } else {
                    std::printf("%d=%s\n", tag, act_val.c_str());
                }

                if (!match) scenario_ok = false;
            }

            continue;
        }
    }

    return true;
}

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
                        uint64_t& logout_start_ms) {
    int total_run = 0;
    int total_passed = 0;
    int total_failed = 0;
    std::vector<std::string> failed_names;

    std::vector<std::string> files;
    DIR* dir = ::opendir(scenarios_path.c_str());
    if (dir) {
        dirent* entry = 0;
        while ((entry = ::readdir(dir)) != 0) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            if (!name.empty() && name[0] == '.') continue;
            files.push_back(scenarios_path + "/" + name);
        }
        ::closedir(dir);
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(scenarios_path);
    }

    bool ok = true;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!run_file(files[i], socket, fix_parser, fix,
                      outbound_seq, last_send_ms, token_path,
                      logon_accepted, scenarios_sent,
                      scenario_response_started, last_scenario_response_ms,
                      logout_initiated, logout_start_ms,
                      total_run, total_passed, total_failed, failed_names)) {
            ok = false;
            break;
        }
    }

    std::printf("\n# OVER ALL SUMMARY\n");
    std::printf("RUN:          %d\n", total_run);
    std::printf("PASSED:       %d\n", total_passed);

    if (total_failed == 0) {
        std::printf("FAILD:        0.\n");
    } else {
        std::printf("FAILED:       %d (", total_failed);
        for (size_t i = 0; i < failed_names.size(); ++i) {
            if (i) std::printf(", ");
            std::printf("%s", failed_names[i].c_str());
        }
        std::printf(")\n");
    }

    return ok && (total_failed == 0);
}
