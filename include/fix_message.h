#ifndef FIX_MESSAGE_H
#define FIX_MESSAGE_H
#include <string>
#include <vector>
#include <utility>

class FixMessage {
public:
    typedef std::pair<int, std::string> Field;
    typedef std::vector<Field> FieldList;

    FixMessage();

    void set_begin_string(const std::string& begin_string);
    void set_sender_comp_id(const std::string& sender_comp_id);
    void set_target_comp_id(const std::string& target_comp_id);

    std::string build_message(const std::string& msg_type,
                              int msg_seq_num,
                              const std::string& sending_time,
                              const FieldList& body_fields) const;

    std::string build_logon(int msg_seq_num,
                            const std::string& sending_time,
                            int heart_bt_int, bool reset_seq_num) const;
    
    // 35=0 HeartBeat
    std::string build_heartbeat(int msg_seq_num,
                                const std::string& sending_time,
                                const std::string& test_req_id) const;
    // 35=1 TestRequest
    std::string build_test_request(int msg_seq_num,
                                   const std::string& sending_time,
                                   const std::string& test_req_id) const;

    // 35=2 ResendRequest
    std::string build_resend_request(int msg_seq_num,
                                     const std::string& sending_time,
                                     int begin_seq_no, int end_seq_no) const;

    // 35=4 SequenceReset
    // 36 is required
    // set gap_fill=true to add
    // 123=Y
    std::string build_sequence_reset(int msg_seq_num,
                                     const std::string& sending_time,
                                     int new_seq_no, bool gap_fill) const;

    // 35=5 Logout
    std::string build_logout(int msg_seq_num,
                             const std::string& sending_time,
                             const std::string& test) const;

    static std::string to_pipe_delimited(const std::string& fix);

private:
    std::string begin_string;
    std::string sender_comp_id;
    std::string target_comp_id;

    static char soh();

    static void append_field(std::string& buffer, int tag, const std::string& tag_value);
    static void append_field_int(std::string& buffer, int tag, int tag_value);

    static int calculate_checksum(const std::string& data);
};

#endif
