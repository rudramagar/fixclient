#ifndef UTILS_H
#define UTILS_H

#include <string>

namespace utils {

// SendingTime format UTC:
// YYYYMMDD-HH:MM:SS.mmm
std::string get_utc_timestamp();

std::string to_pipe_delimited(const std::string& fix);
bool find_tag_value(const std::string& msg, const char* tag_prefix, std::string& value);

}

#endif

