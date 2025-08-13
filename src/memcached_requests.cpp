#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <queue>
#include <map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <numeric>
#include <unordered_set> // Include for the hash set

// Networking includes
#include <sys/socket.h>
#include <sys/un.h> // Required for UNIX domain sockets
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>

// --- Configuration ---
// const char* HOST = "127.0.0.1"; // No longer needed
// const int PORT = 11211;         // No longer needed
const char* SOCKET_PATH = "/home/michael/ISCA_2025_results/tmp/sync_microbench.sock"; // Path to the UNIX domain socket
const int MAX_TOTAL_IN_FLIGHT = 1024; // Max requests across ALL connections
const int BUFFER_SIZE = 16384; 
const int DEFAULT_CONNECTIONS = 4;
const long UPDATE_INTERVAL = 10000; // How often to print live updates

// --- Data Structures ---

struct Request {
    std::string command_type;
    std::string key; // Store the key to track dependencies
    std::chrono::high_resolution_clock::time_point send_time;
};

struct Stats {
    long long count = 0;
    double total_latency_ms = 0.0;
    double max_latency_ms = 0.0;
    double M2 = 0.0;
    double mean = 0.0;

    void update(double latency_ms) {
        count++;
        total_latency_ms += latency_ms;
        max_latency_ms = std::max(max_latency_ms, latency_ms);
        double delta = latency_ms - mean;
        mean += delta / count;
        double delta2 = latency_ms - mean;
        M2 += delta * delta2;
    }

    double get_average() const { return (count > 0) ? (total_latency_ms / count) : 0.0; }
    double get_variance() const { return (count > 1) ? (M2 / (count - 1)) : 0.0; }
    double get_std_dev() const { return std::sqrt(get_variance()); }
};

// State for a single connection to the server
struct ConnectionState {
    int fd = -1;
    std::queue<Request> in_flight_requests;
    std::string receive_buffer;
    bool is_writable = true; // Start as writable
};


// --- Helper Functions ---

bool make_socket_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        return false;
    }
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) == -1) {
        perror("fcntl(F_SETFL)");
        return false;
    }
    return true;
}

void print_stats(const std::map<std::string, Stats>& stats_map, const std::map<std::string, long long>& response_map, long long stall_count) {
    std::cout << "\n--- Trace Replay Finished ---\n";
    std::cout << "\n--- Performance Statistics ---\n";
    for (const auto& pair : stats_map) {
        const auto& cmd_type = pair.first;
        const auto& stats = pair.second;
        std::cout << "--------------------------------\n";
        std::cout << "Command Type: " << cmd_type << "\n";
        std::cout << "  - Succeeded Requests: " << stats.count << "\n";
        if (stats.count > 0) {
            std::cout << "  - Average Latency:    " << std::fixed << stats.get_average() << " ms\n";
            std::cout << "  - Maximum Latency:    " << std::fixed << stats.max_latency_ms << " ms\n";
            std::cout << "  - Latency Std Dev:    " << std::fixed << stats.get_std_dev() << " ms\n";
        }
    }
    std::cout << "--------------------------------\n";

    if (!response_map.empty()) {
        std::cout << "\n--- Server Response Counts ---\n";
        for (const auto& pair : response_map) {
            std::cout << "  - " << pair.first << ": " << pair.second << "\n";
        }
        std::cout << "--------------------------------\n";
    }

    std::cout << "\n--- Replay Metrics ---\n";
    std::cout << "  - Stalls on pending keys: " << stall_count << "\n";
    std::cout << "--------------------------------\n";
}

// Now needs pending_add_keys to unlock keys
bool process_responses_for_connection(
    ConnectionState& conn, 
    std::map<std::string, Stats>& stats, 
    std::map<std::string, long long>& responses,
    std::unordered_set<std::string>& pending_add_keys) 
{
    if (conn.in_flight_requests.empty()) return false;

    size_t end_of_line_pos = conn.receive_buffer.find("\r\n");
    if (end_of_line_pos == std::string::npos) return false;

    std::string response_line = conn.receive_buffer.substr(0, end_of_line_pos);
    const auto& current_request = conn.in_flight_requests.front();
    
    bool request_finished = false;
    bool is_success = false;
    std::string response_key;
    size_t consumed_len = 0;

    if (current_request.command_type == "get") {
        if (response_line == "END") {
            request_finished = true;
            response_key = "NOT_FOUND (END)";
            consumed_len = end_of_line_pos + 2;
        } else if (response_line.rfind("VALUE ", 0) == 0) {
            std::stringstream ss(response_line);
            std::string token;
            long long data_len = -1;
            ss >> token >> token >> token >> data_len;
            if (data_len >= 0) {
                size_t data_start = end_of_line_pos + 2;
                size_t expected_end_pos = data_start + data_len + 2 + 5; // data + \r\n + END\r\n
                if (conn.receive_buffer.length() >= expected_end_pos && conn.receive_buffer.substr(data_start + data_len + 2, 5) == "END\r\n") {
                    request_finished = true;
                    is_success = true;
                    response_key = "FOUND (VALUE)";
                    consumed_len = expected_end_pos;
                }
            }
        }
    } else { // add, replace, set
        if (response_line == "STORED") {
            request_finished = true; is_success = true; response_key = "STORED";
        } else if (response_line == "NOT_STORED") {
            request_finished = true; response_key = "NOT_STORED";
        }
        if(request_finished) consumed_len = end_of_line_pos + 2;
    }
    
    if (response_line.rfind("SERVER_ERROR", 0) == 0 || response_line.rfind("CLIENT_ERROR", 0) == 0) {
        request_finished = true;
        response_key = "SERVER/CLIENT_ERROR";
        consumed_len = end_of_line_pos + 2;
    }

    if (request_finished) {
        if (is_success) {
            auto latency = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - current_request.send_time);
            stats[current_request.command_type].update(latency.count());
        }
        responses[response_key]++;

        // *** UNLOCK KEY ***: If this was an 'add', remove its key from the pending set
        if (current_request.command_type == "add") {
            pending_add_keys.erase(current_request.key);
        }

        conn.in_flight_requests.pop();
        conn.receive_buffer.erase(0, consumed_len);
        return true;
    }
    return false;
}

// Function to change epoll monitoring mode
void set_epoll_mode(int epoll_fd, std::vector<ConnectionState>& connections, bool send_enabled) {
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET; // Always listen for input
    if (send_enabled) {
        event.events |= EPOLLOUT;
    }

    for (auto& conn : connections) {
        event.data.ptr = &conn;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &event) == -1) {
            perror("epoll_ctl_mod");
        }
    }
}


// --- Main Logic ---

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <trace_file> [--live] [-c|--connections <N>]" << std::endl;
        return 1;
    }
    const char* trace_filename = argv[1];
    bool live_updates_enabled = false;
    int num_connections = DEFAULT_CONNECTIONS;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--live") { live_updates_enabled = true; } 
        else if (arg == "-c" || arg == "--connections") {
            if (i + 1 < argc) {
                try { num_connections = std::stoi(argv[++i]); } 
                catch (const std::exception& e) { std::cerr << "Invalid number for connections: " << e.what() << std::endl; return 1; }
            }
        }
    }

    std::ifstream trace_file(trace_filename);
    if (!trace_file.is_open()) { std::cerr << "Error: Could not open trace file '" << trace_filename << "'" << std::endl; return 1; }

    std::vector<ConnectionState> connections(num_connections);
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("epoll_create1"); return 1; }

    // *** MODIFIED: Connect to UNIX domain socket ***
    for (int i = 0; i < num_connections; ++i) {
        int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0); // Use AF_UNIX for domain sockets
        if (sock_fd < 0) { perror("socket"); return 1; }
        
        struct sockaddr_un serv_addr; // Use the specific address structure for UNIX sockets
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sun_family = AF_UNIX;
        strncpy(serv_addr.sun_path, SOCKET_PATH, sizeof(serv_addr.sun_path) - 1);

        if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { 
            perror("connect"); 
            return 1; 
        }
        
        make_socket_non_blocking(sock_fd);
        connections[i].fd = sock_fd;
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLOUT | EPOLLET;
        event.data.ptr = &connections[i];
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event) == -1) { perror("epoll_ctl_add"); return 1; }
    }
    std::cout << "Established " << num_connections << " connections to " << SOCKET_PATH << std::endl;

    std::map<std::string, Stats> statistics;
    std::map<std::string, long long> response_counts;
    // NOTE: This assumes the trace does not contain multiple concurrent 'add' requests for the same key.
    // A more robust implementation for arbitrary traces would use a map to count pending adds per key.
    std::unordered_set<std::string> pending_add_keys;
    bool trace_file_done = false;
    long total_requests_sent = 0;
    size_t next_connection_idx = 0;
    size_t total_in_flight = 0;
    long last_update_req_count = 0;
    std::string stalled_on_key = "";
    long long stall_count = 0;

    if (!live_updates_enabled) { std::cout << "Live updates disabled. Use --live to enable." << std::endl; }

    while (!trace_file_done || total_in_flight > 0) {
        struct epoll_event events[num_connections * 2];
        int n_events = epoll_wait(epoll_fd, events, num_connections * 2, -1);

        if (n_events == -1) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        for (int i = 0; i < n_events; ++i) {
            ConnectionState* conn = static_cast<ConnectionState*>(events[i].data.ptr);
            if (events[i].events & EPOLLIN) {
                char read_buffer[BUFFER_SIZE];
                while (true) {
                    ssize_t count = read(conn->fd, read_buffer, BUFFER_SIZE);
                    if (count > 0) conn->receive_buffer.append(read_buffer, count);
                    else { if (errno != EAGAIN) perror("read"); break; }
                }
            }
            if (events[i].events & EPOLLOUT) conn->is_writable = true;
        }
        
        for(auto& conn : connections) {
            while(process_responses_for_connection(conn, statistics, response_counts, pending_add_keys));
        }

        total_in_flight = 0;
        for(const auto& conn : connections) total_in_flight += conn.in_flight_requests.size();

        // Check if we were stalled and if the key is now free
        if (!stalled_on_key.empty() && pending_add_keys.count(stalled_on_key) == 0) {
            stalled_on_key = "";
            set_epoll_mode(epoll_fd, connections, true); // Re-enable sending
        }

        while (stalled_on_key.empty() && total_in_flight < MAX_TOTAL_IN_FLIGHT && !trace_file_done) {
            ConnectionState& conn = connections[next_connection_idx];
            if (!conn.is_writable) break;

            std::streampos before_read_pos = trace_file.tellg();
            std::string line1, line2;
            if (!std::getline(trace_file, line1)) { trace_file_done = true; break; }
            if (!line1.empty() && line1.back() == '\r') line1.pop_back();

            std::string full_command, cmd_type, key;
            std::stringstream ss(line1);
            ss >> cmd_type >> key;

            if (pending_add_keys.count(key)) {
                stalled_on_key = key;
                stall_count++;
                set_epoll_mode(epoll_fd, connections, false); // Disable sending
                trace_file.clear();
                trace_file.seekg(before_read_pos);
                break;
            }

            if (cmd_type == "add" || cmd_type == "replace" || cmd_type == "set") {
                if (!std::getline(trace_file, line2)) { trace_file_done = true; break; }
                if (!line2.empty() && line2.back() == '\r') line2.pop_back();
                full_command = line1 + "\r\n" + line2 + "\r\n";
            } else if (cmd_type == "get") {
                full_command = line1 + "\r\n";
            } else { continue; }

            ssize_t bytes_sent = write(conn.fd, full_command.c_str(), full_command.length());
            if (bytes_sent > 0) {
                if (cmd_type == "add") { pending_add_keys.insert(key); }
                conn.in_flight_requests.push({cmd_type, key, std::chrono::high_resolution_clock::now()});
                total_requests_sent++;
                total_in_flight++;
                next_connection_idx = (next_connection_idx + 1) % num_connections;
            } else if (bytes_sent == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    conn.is_writable = false;
                    trace_file.clear();
                    trace_file.seekg(before_read_pos);
                    break;
                }
                perror("write"); goto cleanup;
            }
        }

        if (live_updates_enabled && (total_requests_sent - last_update_req_count) >= UPDATE_INTERVAL) {
            std::cout << "Sent: " << total_requests_sent << " | In-Flight: " << total_in_flight << " | Pending Adds: " << pending_add_keys.size();
            if(!stalled_on_key.empty()) {
                std::cout << " | Stalled on: " << stalled_on_key;
            }
            else {
                std::cout << "\t\t\t";
            }
            std::cout << "  \r" << std::flush;
            last_update_req_count = total_requests_sent;
        }
    }

cleanup:
    std::cout << "\nTrace file processed. Draining final responses..." << std::endl;
    print_stats(statistics, response_counts, stall_count);

    for(auto& conn : connections) close(conn.fd);
    close(epoll_fd);
    trace_file.close();

    return 0;
}
