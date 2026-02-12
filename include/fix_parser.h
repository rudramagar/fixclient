#ifndef FIX_PARSER_H
#define FIX_PARSER_H

#include <string>

class FixParser {
public:
    FixParser();

    // Appends raw TCP bytes
    // to the internal buffer
    void append_bytes(const char* data, size_t size);

    // It reads ONE complete FIX message from
    // the internal buffer instead of network.
    // Returns True if complete FIX message found
    // and copied into message.
    // Returns False if the buffer does not have a
    // full message yet.
    bool read_next_message(std::string& message);

    // Clears/resets internal buffer.
    void reset();

private:
    std::string buffer;

    // Lookup for BeginString
    // in internal buffer
    bool find_begin_string(size_t& start_pos) const;

    // Reads BodyLength(9) from buffer
    // return True if tag 9 is found and valid.
    bool parse_body_length(size_t start_pos, int& body_length, size_t& end_body_len_field) const;
};

#endif
