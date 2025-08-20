// Network server implementation for Phase 4
#include "blustream/server/network_server.h"
#include "blustream/common/logger.h"

#include <cstring>
#include <fcntl.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace blustream {
namespace server {

NetworkServer::NetworkServer() 
    : server_socket_(-1)
    , is_running_(false)
    , port_(0) {
}

NetworkServer::~NetworkServer() {
    stop();
}

bool NetworkServer::start(int port) {
    if (is_running_) {
        BLUSTREAM_LOG_WARN("Network server already running");
        return true;
    }
    
    port_ = port;
    
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        BLUSTREAM_LOG_ERROR("WSAStartup failed");
        return false;
    }
#endif
    
    // Create socket
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        BLUSTREAM_LOG_ERROR("Failed to create socket: " + std::string(strerror(errno)));
        return false;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, 
                   reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0) {
        BLUSTREAM_LOG_ERROR("Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
        close(server_socket_);
        return false;
    }
    
    // Disable Nagle's algorithm for low latency
    if (setsockopt(server_socket_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0) {
        BLUSTREAM_LOG_WARN("Failed to set TCP_NODELAY: " + std::string(strerror(errno)));
    }
    
    // Bind to port
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(server_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        BLUSTREAM_LOG_ERROR("Failed to bind to port " + std::to_string(port) + 
                           ": " + std::string(strerror(errno)));
        close(server_socket_);
        return false;
    }
    
    // Start listening
    if (listen(server_socket_, 10) < 0) {
        BLUSTREAM_LOG_ERROR("Failed to listen on socket: " + std::string(strerror(errno)));
        close(server_socket_);
        return false;
    }
    
    is_running_ = true;
    BLUSTREAM_LOG_INFO("Network server listening on port " + std::to_string(port));
    
    return true;
}

void NetworkServer::stop() {
    if (!is_running_) {
        return;
    }
    
    is_running_ = false;
    
    // Close server socket
    if (server_socket_ >= 0) {
#ifdef _WIN32
        closesocket(server_socket_);
        WSACleanup();
#else
        close(server_socket_);
#endif
        server_socket_ = -1;
    }
    
    BLUSTREAM_LOG_INFO("Network server stopped");
}

int NetworkServer::accept_client(std::string& client_address) {
    if (!is_running_ || server_socket_ < 0) {
        return -1;
    }
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_socket < 0) {
        if (is_running_) {
            BLUSTREAM_LOG_ERROR("Failed to accept client: " + std::string(strerror(errno)));
        }
        return -1;
    }
    
    // Get client address
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
    client_address = std::string(addr_str) + ":" + std::to_string(ntohs(client_addr.sin_port));
    
    // Set socket options for client
    int opt = 1;
    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    
    // Set socket to non-blocking mode
    set_non_blocking(client_socket);
    
    return client_socket;
}

bool NetworkServer::set_non_blocking(int socket) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return fcntl(socket, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

} // namespace server
} // namespace blustream