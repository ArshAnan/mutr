#include "server.h"

#include <charconv>
#include <iostream>
#include <string>
#include <system_error>

namespace {

void usage(std::ostream& out) {
    out << "Usage: mutr [--port N] [--verbose]\n"
        << "  --port N    TCP port (default 6379, 0 lets the kernel pick)\n"
        << "  --verbose   log accepts and closes\n";
}

}  // namespace

int main(int argc, char** argv) {
    mutr::Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--verbose") {
            config.verbose = true;
            continue;
        }
        if (arg == "--port") {
            if (i + 1 >= argc) {
                usage(std::cerr);
                return 2;
            }
            ++i;
            const std::string value = argv[i];
            int port = 0;
            const auto res = std::from_chars(value.data(), value.data() + value.size(), port);
            if (res.ec != std::errc() || res.ptr != value.data() + value.size() || port < 0 || port > 65535) {
                std::cerr << "invalid port\n";
                return 2;
            }
            config.port = port;
            continue;
        }
        usage(std::cerr);
        return 2;
    }
    return mutr::runServer(config);
}
