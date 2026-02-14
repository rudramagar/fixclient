#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <stdint.h>

namespace utils {

// SendingTime format UTC:
// YYYYMMDD-HH:MM:SS.mmm
std::string get_utc_timestamp();
uint64_t get_monotonic_millis();

std::string to_pipe_delimited(const std::string& fix);
bool find_tag_value(const std::string& msg, const char* tag_prefix, std::string& value);
std::string trim(const std::string& str);

}

#endif

