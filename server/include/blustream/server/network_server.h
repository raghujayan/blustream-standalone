#pragma once

#include <string>
#include <atomic>

namespace blustream {
namespace server {

/**
 * @brief TCP network server for accepting client connections
 */
class NetworkServer {
public:
    NetworkServer();
    ~NetworkServer();
    
    // Start server on specified port
    bool start(int port);
    
    // Stop server
    void stop();
    
    // Accept a client connection (blocking)
    // Returns client socket fd, or -1 on error
    // Sets client_address to "IP:port" string
    int accept_client(std::string& client_address);
    
    // Check if server is running
    bool is_running() const { return is_running_; }
    
    // Get server port
    int get_port() const { return port_; }
    
private:
    int server_socket_;
    std::atomic<bool> is_running_;
    int port_;
    
    // Helper to set socket to non-blocking mode
    bool set_non_blocking(int socket);
};

} // namespace server
} // namespace blustream