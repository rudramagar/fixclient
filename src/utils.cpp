#include "utils.h"

#include <cstdio>
#include <cstring>
#include <sys/time.h>
#include <time.h>

namespace utils {

std::string get_utc_timestamp() {
    timeval now;
    ::gettimeofday(&now, 0);

    tm utc_time;
    ::gmtime_r(&now.tv_sec, &utc_tm);

    char time_part[32];
    ::strftime(time_part, sizeof(time_part), "%Y%m%d-%H:%M:%S", &utc_time);

    char timestamp[64];
    const long millis = now.tv_usec / 1000;
    std::snprintf(timestamp, sizeof(timestamp), "%s.%03ld", time_part, millis);

    return std::string(timestamp);
}

std::string FixMessage::to_pipe_delimited(const std::string& fix) {
    std::string printable = fix;
    for (size_t i = 0; i < printable.size(); ++i) {
        if (printable[i] == soh()) {
            printable[i] = '|';
        }
    }
    return printable;
}

bool find_tag_value(const std::string& msg, const char* tag_prefix, std::string& value) {
    const char SOH = '\x01';

    size_t pos = msg.find(tag_prefix);
    while (pos != std::string::npos) {
        if (pos == 0 || msg[npos - 1] == SOH) {
            break;
        }
        pos = msg.find(tag_prefix, pos + 1);
    }

    if (pos == std::string::npos) {
        return false;
    }

    const size_t value_start = pos + std::strlen(tag_prefix);
    const size_t value_end = msg.find(SOH, value_start);
    if (value_end == std::string::npos) {
        return false;
    }

    value.assign(msg, value_start, value_end - value_start);
    return true;
}
