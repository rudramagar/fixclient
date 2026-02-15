#include "token_handler.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

bool save_token(const std::string& token_path, int next_seq) {
    if (token_path.empty()) {
        return false;
    }

    if (next_seq <= 0) {
        return false;
    }

    std::FILE* file = std::fopen(token_path.c_str(), "w");
    if (!file) {
        return false;
    }

    std::fprintf(file, "%d\n", next_seq);
    std::fclose(file);
    return true;
}

bool read_token(const std::string& token_dir,
                const std::string& sender_comp_id,
                const std::string& utc_timestamp,
                bool reset_on_logon,
                int& next_seq,
                std::string& token_path_out) {

    next_seq = 1;
    token_path_out.clear();

    if (sender_comp_id.empty()) {
        return false;
    }

    std::string dir = token_dir;
    if (dir.empty()) {
        dir = "tokens";
    }

    std::string day = "00000000";
    if (utc_timestamp.size() >= 8) {
        day = utc_timestamp.substr(0, 8);
    }

    struct stat st;
    if (::stat(dir.c_str(), &st) != 0) {
        if (::mkdir(dir.c_str(), 0755) != 0) {
            if (errno != EEXIST) {
                return false;
            }
        }
    }
    else {
        if (!S_ISDIR(st.st_mode)) {
            return false;
        }
    }

    token_path_out = dir + "/" + sender_comp_id + "_" + day + ".token";

    if (reset_on_logon) {
        next_seq = 1;
        save_token(token_path_out, next_seq);
        return true;
    }

    std::FILE* file = std::fopen(token_path_out.c_str(), "r");
    if (!file) {
        next_seq = 1;
        save_token(token_path_out, next_seq);
        return true;
    }

    char buf[32];
    const size_t bytes_read = std::fread(buf, 1, sizeof(buf) - 1, file);
    std::fclose(file);

    buf[bytes_read] = '\0';

    const long value = std::strtol(buf, 0, 10);
    if (value > 0 && value <= 2000000000L) {
        next_seq = static_cast<int>(value);
        return true;
    }

    next_seq = 1;
    save_token(token_path_out, next_seq);
    return true;
}

