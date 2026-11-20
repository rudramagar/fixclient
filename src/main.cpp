
#include "application.h"
#include <cstdio>
#include <getopt.h>
#include <cstring>
#include <cstdlib>
#include <climits>

static void usage(const char* program_name) {
    std::printf(
            "Usage:\n"
            " %s -u <session> [options]\n\n"
            "Options:\n"
            " -u <session>          session name (f01)\n"
            " -c <config>           config file (default: config/config.yaml)\n"
            " -s <scenario>         scenario file or directory (default: scenarios)\n"
            " -m, --mode test       validates expected scenarios\n"
            " -l, --live            keep session live\n"
            " -S, --store           store sent messages for resend scenarios\n"
            " -r, --recover         enable Drop Copy recovery\n"
            " -b, --begin-seq <seq> starting sequence (default: 1)\n"
            " -n, --count <count>   number of sequences (default: all)\n"
            " -h, --help            show help\n",
            program_name
    );
}

int main(int argc, char** argv) {
    AppArgs args;

    static const struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"mode", required_argument, 0, 'm'},
        {"live", no_argument, 0, 'l'},
        {"store", no_argument, 0, 'S'},
        {"recover", no_argument, 0, 'r'},
        {"begin-seq", required_argument, 0, 'b'},
        {"count", required_argument, 0, 'n'},
        {0,0,0,0}
    };

    int option = 0;
    int long_index = 0;

    while ((option = getopt_long(argc, argv, "u:c:s:b:n:m:hlSr", long_options, &long_index)) != -1) {
        switch (option) {
            case 'u':
                args.session_name = optarg;
                break;

            case 'c':
                args.config_path = optarg;
                break;

            case 's':
                args.scenario_path = optarg;
                break;

            case 'm':
                if (std::strcmp(optarg, "test") == 0) {
                    args.is_test_mode = true;
                } else {
                    std::printf("Error: (-m|--mode) only supports: test\n");
                    return 1;
                }
                break;

            case 'l':
                args.live = true;
                break;

            case 'S':
                args.store = true;
                break;

            case 'r':
                args.recover = true;
                break;

            case 'b':
                args.recovery_start_seq = std::atoi(optarg);
                if (args.recovery_start_seq <= 0) return 1;
                break;

            case 'n':
                args.recovery_count = std::atoi(optarg);
                if (args.recovery_count <= 0) return 1;
                break;

            case 'h':
                usage(argv[0]);
                return 0;

            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (args.recover && args.recovery_count > 0 &&
        args.recovery_start_seq > INT_MAX - (args.recovery_count - 1)) {
        std::printf("Error: recovery sequence range exceeds INT_MAX\n");
        return 1;
    }

    if (optind < argc) {
        std::printf("Error: Unknown argument: %s\n", argv[optind]);
        usage(argv[0]);
        return 1;
    }

    if (args.session_name.empty()) {
        std::printf("Error: -u <session> is required\n");
        usage(argv[0]);
        return 1;
    }

    Application app;
    return app.run(args);
}
