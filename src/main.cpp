#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

// Making sure the port is listening to the client
int setupServer(int port) {
    // Create a socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0); // Asks the kernel to create a socket with IPv4 and TCP
    // The kernel simply returns a file descriptor (an integer) to the socket, that we will use to interact with the socket.
    if (server_fd < 0) {
        std::cerr << "Failed to create a socket" << "\n";
        exit(1);
    }
    int opt = 1; // 1 means true, 0 means false. We use to this to "enable this option".
    // setsockopt takes the pointer to the option, that is why we use &opt.

    // setsockopt is used to allow the socket to be reused immediately after the program is closed. Without this, the socket will be in a TIME_WAIT state for 2 minutes after the program is closed.
    // When we try to start the server again, the socket will be in a TIME_WAIT state, and the new server will not be able to bind to the same port. Will give an error like "Address already in use".

    // The function setsockopt takes the following arguments:
    // 1. The socket file descriptor (server_fd)
    // 2. The level of the socket (SOL_SOCKET)
    // 3. The option to set (SO_REUSEADDR)
    // 4. The pointer to the option (opt)
    // 5. The size of the option (sizeof(opt))
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set socket options" << "\n";
        close(server_fd);
        exit(1);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address)); // Zero out the structure.
    address.sin_family = AF_INET; // Set the family to IPv4.
    address.sin_port = htons(port); // The port number. The network uses big-endian format, but most CPUs use little-endian format. So, we need to convert it to little-endian format using htons(port). "htons" stands for "host to network short".
    address.sin_addr.s_addr = INADDR_ANY; // The IP address of the server. This is a 32-bit integer in network byte order. The use of "INADDR_ANY" means that the server will listen on all IP addresses.

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) { // Bind the socket to the address and port.
        std::cerr << "Failed to bind the socket" << "\n"; // If the binding fails, exit the program.
        close(server_fd);
        exit(1);
    }

    if (listen(server_fd, 128) < 0) { // Listen for incoming connections. The 128 is the backlog, which is the number of connections that can be queued up.
        std::cerr << "Failed to listen to the socket" << "\n"; // If the listening fails, exit the program.
        close(server_fd);
        exit(1);
    }

    std::cout << "Server is listening on port " << port << "\n"; // Print a message to the console to indicate that the server is listening on the port.

    return server_fd;
}

int main() {
    int serverFileDescriptor = setupServer(6379);
    return 0;
}