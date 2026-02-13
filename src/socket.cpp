#include "socket.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

TcpSocket::TcpSocket() : sock_fd(-1) {}

TcpSocket::~TcpSocket() {
    close();
}

void TcpSocket::close() {
    if (sock_fd >= 0) {
        ::close(sock_fd);
        sock_fd = -1;
    }
}

bool TcpSocket::connect(const std::string& host, int port) {
    close();

    sock_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        sock_fd = -1;
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // Convert IP string (e.g., "127.0.0.1") to binary
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close();
        return false;
    }
    
    if (::connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close();
        return false;
    }

    return true;
}

bool TcpSocket::send_bytes(const std::string& data) {
    if (sock_fd < 0) return false;

    const char* ptr = data.data();
    size_t remaining = data.size();

    // Send
    // can be send less bytes than requested;
    // loop until all bytes are sent.
    while (remaining > 0) {
        ssize_t bytes_sent = ::send(sock_fd, ptr, remaining, 0);
        if (bytes_sent > 0) {
            ptr += bytes_sent;
            remaining -= static_cast<size_t>(bytes_sent);
            continue;
        }
        
        // Treat as connection problem 
        // on unusual for send()
        if (bytes_sent == 0) {
            return false;
        }
        
        // Interrupted by a 
        // signal, retry
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

int TcpSocket::receive_bytes(char* buf, size_t max_bytes) {
    if (sock_fd < 0) {
        return -1;
    }

    if (!buf || max_bytes == 0) {
        return -1;
    }

    ssize_t bytes_read = ::recv(sock_fd, buf, max_bytes, 0);

    if (bytes_read > 0) {
        return static_cast<int>(bytes_read);
    }

    // peer close connection
    if (bytes_read == 0) {
        return 0;
    }

    // error
    return -1;
}
