#include "fix_regression.h"
#include "token_handler.h"
#include "constants.h"
#include "utils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <dirent.h>
#include <unistd.h>
#include <cstdarg>
#include <ctime>
#include <sys/stat.h>

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
                               std::string& out_message);

const int timeout_test_ms = 3000;
const int timeout_discard_ms = 500;
const int max_clr = 50;
static std::string log_begin_string;
static std::string log_sender_comp_id;

static void print_result_log(const char* format, ...) {
    static FILE* result_log_file = 0;

    if (!result_log_file) {
        ::mkdir("results", 0755);

        std::time_t now = std::time(0);
        std::tm local_time;
        localtime_r(&now, &local_time);

        char log_path[256];
        std::snprintf(log_path, sizeof(log_path), "results/%s_%s_REGRESSION_RESULT_%04d%02d%02d_%02d%02d%02d.log",
                                       log_begin_string.c_str(), log_sender_comp_id.c_str(), local_time.tm_year + 1900,
                                       local_time.tm_mon + 1, local_time.tm_mday + 1, local_time.tm_hour,
                                       local_time.tm_min, local_time.tm_sec);

        result_log_file = std::fopen(log_path, "w");
    }

    char formatted_text[8192];
    va_list args;
    va_start(args, format);
    const int text_len = std::vsnprintf(formatted_text, sizeof(formatted_text), format, args);
    va_end(args);

    if (text_len <= 0) return;

    std::fwrite(formatted_text, 1, static_cast<size_t>(text_len), stdout);

    if (result_log_file) {
        bool inside_ansi_code = false;
        for (int i = 0; i < text_len; ++i) {
            const char ch = formatted_text[i];

            if (ch == '\033') {
                inside_ansi_code = true;
                continue;
            }

            if (inside_ansi_code) {
                if (ch == 'm') inside_ansi_code = false;
                continue;
            }
            std::fputc(ch, result_log_file);
        }

        std::fflush(result_log_file);
    }
}

static void get_status_clr(const char* label, bool is_success) {
    const bool use_color =
        (std::getenv("NO_COLOR") == nullptr) &&
        (::isatty(::fileno(stdout)) == 1);

    if (use_color) {
        print_result_log("%s%-4s%s", is_success ? GREEN : RED, label, RESET);
    } else {
        print_result_log("%-4s", label);
    }
}

static void print_details(const std::string& fix) {
    std::string printable;
    printable.reserve(fix.size());

    for (size_t i = 0; i < fix.size(); ++i) {
        char ch = fix[i];
        if (ch == '\x01') ch = '|';
        printable.push_back(ch);
    }

    print_result_log("%s", printable.c_str());
}

static void make_fix_field_name(std::string& out_line, int tag, const char* value) {
    const char* tag_name = fix_tag_name(tag);
    const char* tag_value = value ? value : "";

    char buf[512];
    if (tag_name) {
        std::snprintf(buf, sizeof(buf), "%s(%d)=%s|", tag_name, tag, tag_value);
    }
    else {
        std::snprintf(buf, sizeof(buf), "%d=%s|", tag, tag_value);
    }
    out_line.append(buf);
}

static void build_clr_id(std::string clr_values[max_clr + 1], int scenario_index) {
    (void)scenario_index;

    for (int i = 0; i <= max_clr; ++i) {
        clr_values[i].clear();
    }

    static unsigned long long uniq = 0;
    uniq++;

    const unsigned long long now_ms =
        static_cast<unsigned long long>(utils::get_monotonic_millis());

    const unsigned long long base = now_ms * 1000000ULL + (uniq % 1000000ULL);

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

    char delim = 0;
    if (payload.find('|') != std::string::npos) delim = '|';
    else if (payload.find(',') != std::string::npos) delim = ',';
    else delim = 0;

    size_t pos = 0;
    while (pos <= payload.size()) {
        size_t end = std::string::npos;
        if (delim) end = payload.find(delim, pos);
        if (end == std::string::npos) end = payload.size();

        std::string token = utils::trim(payload.substr(pos, end - pos));
        pos = (end < payload.size()) ? (end + 1) : (payload.size() + 1);

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
        while (!cmd.empty()) {
            const unsigned char c = static_cast<unsigned char>(cmd[0]);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) break;
            cmd.erase(0, 1);
        }

        while (!cmd.empty()) {
            const unsigned char c = static_cast<unsigned char>(cmd[cmd.size() - 1]);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) break;
            cmd.erase(cmd.size() - 1, 1);
        }

        for (size_t i = 0; i < cmd.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(cmd[i]);
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
            if (scenarios_sent) {
                const int timeout_drain_ms = 5;
                const int drain_max_reads = 50;

                for (int i = 0; i < drain_max_reads; ++i) {
                    std::string pending;
                    bool stop_requested = false;

                    if (!read_next_business_message(socket, fix_parser, fix,
                                                    outbound_seq, last_send_ms, token_path,
                                                    logon_accepted, stop_requested,
                                                    scenarios_sent, scenario_response_started,
                                                    last_scenario_response_ms, logout_initiated,
                                                    timeout_drain_ms, pending)) {
                        return false;
                    }

                    if (pending.empty()) {
                        break;
                    }
                }
            }

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
            } else {
                total_failed++;
                failed_names.push_back(scenario_name);
            }

            std::printf("END %s\n", scenario_name.c_str());
            std::printf("\n");

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
                                            logout_initiated,
                                            timeout_discard_ms, msg)) {
                return false;
            }

            step++;
            std::printf("  %02d  \tRCV\n", step);
            continue;
        }

		if (cmd == "SND") {
		    FixMessage::FieldList raw;
		    std::string msg_type;
		    parse_fields(payload, raw, &msg_type);
		
		    if (msg_type.empty()) {
		        step++;
		        std::printf("  %02d  SEND: (ERROR missing 35)\n", step);
		        scenario_ok = false;
		        continue;
		    }
		
		    const std::string now_utc = utils::get_utc_timestamp();
		
		    // Apply clrN and fill blanks (34/52/60)
		    for (size_t i = 0; i < raw.size(); ++i) {
		        const int tag = raw[i].first;
		        std::string& value = raw[i].second;
		
		        // clrN replacement
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
		
		        // fill blanks like v1
		        if (tag == 34 && value.empty()) {
		            char buf[32];
		            std::snprintf(buf, sizeof(buf), "%d", outbound_seq);
		            value = buf;
		        }
		        if (tag == 52 && value.empty()) value = now_utc;
		        if (tag == 60 && value.empty()) value = now_utc;
		    }
		
		    // Build raw FIX from ordered fields (preserves your scenario order)
		    const std::string msg = fix.build_from_fields(raw);
		    if (msg.empty()) {
		        step++;
		        std::printf("  %02d  SEND: (ERROR build_from_fields failed)\n", step);
		        scenario_ok = false;
		        continue;
		    }
		
		    if (!socket.send_bytes(msg)) {
		        return false;
		    }
		
		    last_send_ms = utils::get_monotonic_millis();
		    outbound_seq++;
		    save_token(token_path, outbound_seq);
		    scenarios_sent = true;
		
		    step++;

            std::string send_line;
            send_line.reserve(raw.size() * 16);
            for (size_t i = 0; i < raw.size(); ++i) {
                make_fix_field_name(send_line, raw[i].first, raw[i].second.c_str());
            }

            print_result_log("  %02d \tSEND: %s\n", step, send_line.c_str());
		    continue;
		}

        if (cmd == "TST") {
            FixMessage::FieldList expected;
            parse_fields(payload, expected, 0);
            const int table_indent = 9;

            step++;

            std::string tst_message;
            tst_message.reserve(expected.size() * 16);
            for (size_t i = 0; i < expected.size(); ++i) {
                make_fix_field_name(tst_message, expected[i].first, expected[i].second.c_str());
            }

            print_result_log("  %02d  \tTEST:  %s\n", step, tst_message.c_str());

            std::string msg;
            bool stop_requested = false;

            if (!read_next_business_message(socket, fix_parser, fix,
                                            outbound_seq, last_send_ms, token_path,
                                            logon_accepted, stop_requested,
                                            scenarios_sent,
                                            scenario_response_started, last_scenario_response_ms,
                                            logout_initiated,
                                            timeout_test_ms, msg)) {
                return false;
            }
    
            step++;
            if (msg.empty()) {
                std::printf("  %02d  \tRECV: (TIMEOUT)\n", step);
                scenario_ok = false;
                continue;
            }

            // Print RAW FIX message received
            // from server
            print_result_log("  %02d\tRECV:  ", step);
            print_details(msg);
            print_result_log("\n");

            print_result_log("%*sRECEIVED:\n", table_indent, "");

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
                act_val.clear();
                const bool has = utils::find_tag_value(msg, prefix, act_val);

                bool match = false;
				if (exp_text.empty()) {
					match = true;
				}	
                else if (exp_text == "IGNORE") {
                    match = true;
                } else if (exp_text == "NONE") {
                    match = (!has || act_val.empty() || act_val == "NONE");
                } else {
                    match = has && (act_val == exp_val);
                }

                const char* show_exp = exp_text.c_str();
                if (exp_text.size() >= 4 && exp_text[0]=='c' && exp_text[1]=='l' && exp_text[2]=='r') {
                    show_exp = exp_val.c_str();
                }

                const char* got_text = has ? act_val.c_str() : "MISSING";

                std::string received_named;
                received_named.reserve(64);
                make_fix_field_name(received_named, tag, got_text);
                if (!received_named.empty() && received_named.back() == '|') received_named.pop_back();

                print_result_log("%*s", table_indent, "");

                if (match) {
                    get_status_clr("OK", true);
                    print_result_log("  %s\n", received_named.c_str());
                }
                else {
                    std::string got_exp_name;
                    got_exp_name.reserve(64);
                    make_fix_field_name(got_exp_name, tag, show_exp);
                    if (!got_exp_name.empty() && got_exp_name.back() == '|') got_exp_name.pop_back();

                    print_result_log("%s%-4s  %s != %s <- (exp)\n", RED, "FAIL",
                                     received_named.c_str(), got_exp_name.c_str(), RESET);
                    scenario_ok = false;
                }
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
                        bool& logout_initiated) {
    int total_run = 0;
    int total_passed = 0;
    int total_failed = 0;
    std::vector<std::string> failed_names;
    log_begin_string = fix.get_begin_string();
    log_sender_comp_id = fix.get_sender_comp_id();


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
                      logout_initiated,
                      total_run, total_passed, total_failed, failed_names)) {
            ok = false;
            break;
        }
    }

    print_result_log("\n# OVER ALL SUMMARY\n");
    print_result_log("Total Scenarios:\t%d\n", total_run);
    print_result_log("Total Passed:\t\t%d\n", total_passed);

    if (total_failed == 0) {
        print_result_log("Total Failed:\t\t0\n");
        print_result_log("All done!\n");
    } else {
        std::printf("Total Failed:\t\t%d (", total_failed);
        for (size_t i = 0; i < failed_names.size(); ++i) {
            if (i) print_result_log(", ");
            print_result_log("%s", failed_names[i].c_str());
        }
        print_result_log(")\n");
    }

    std::printf("\n");

    return ok && (total_failed == 0);
}
