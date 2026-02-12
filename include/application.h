#ifndef APPLICATION_H
#define APPLICATION_H

#include <string>
#include <socket.h>

struct AppArgs {
    std::string session_name;
    std::string config_path = "config/config.yaml";
    std::string scenario_path = "scenarios";
    bool is_test_mode = false;
};

class Application {
public:
    int run(const AppArgs& args);

private:
    TcpSocket socket;
};

#endif
