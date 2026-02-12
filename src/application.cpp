#include "application.h"

#include "config_parser.h"
#include <cstdio>

int Application::run(const AppArgs& args) {
    ConfigParser parser;
    parser.load(args.config_path);

    SessionConfig config = parser.get_session(args.session_name);

    if (!socket.connect(config.host, config.port)) {
        std::printf("Error: Connection failed\n");
        return 1;
    }

    std::printf("Info: Connected to &=%s:%d\n", config.host.c_str(), config.port);

    socket.close();
    return 0;
}
