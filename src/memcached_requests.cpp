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

// Networking includes
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>

// --- Configuration ---
const char* HOST = "127.0.0.1";
const int PORT = 11211;
const int MAX_IN_FLIGHT = 64; // Maximum number of concurrent requests
const int BUFFER_SIZE = 16384; // Increased buffer for high throughput

// --- Data Structures ---

// Represents a single request sent to memcached
struct Request {
    std::string command_type;
    std::chrono::high_resolution_clock::time_point send_time;
    std::string full_command; // The full command string sent
};

// Statistics for each command type
struct Stats {
    long long count = 0;
    double total_latency_ms = 0.0;
    double max_latency_ms = 0.0;
    
    // For calculating variance using Welford's online algorithm
    double M2 = 0.0;
    double mean = 0.0;

    void update(double latency_ms) {
        count++;
        total_latency_ms += latency_ms;
        if (latency_ms > max_latency_ms) {
            max_latency_ms = latency_ms;
        }

        // Welford's algorithm for running variance
        double delta = latency_ms - mean;
        mean += delta / count;
        double delta2 = latency_ms - mean;
        M2 += delta * delta2;
    }

    double get_average() const {
        return (count > 0) ? (total_latency_ms / count) : 0.0;
    }

    double get_variance() const {
        return (count > 1) ? (M2 / (count - 1)) : 0.0;
    }
    
    double get_std_dev() const {
        return std::sqrt(get_variance());
    }
};

// --- Helper Functions ---

/**
 * @brief Sets a file descriptor to non-blocking mode.
 * @param fd The file descriptor.
 * @return True on success, false on failure.
 */
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

/**
 * @brief Prints the final statistics report.
 * @param stats_map A map containing the statistics for each command type.
 * @param errors_map A map containing counts of different server responses.
 */
void print_stats(const std::map<std::string, Stats>& stats_map, const std::map<std::string, long long>& errors_map) {
    std::cout << "\n--- Trace Replay Finished ---\n";
    std::cout << "\n--- Performance Statistics ---\n";
    for (const auto& pair : stats_map) {
        const std::string& cmd_type = pair.first;
        const Stats& stats = pair.second;
        std::cout << "--------------------------------\n";
        std::cout << "Command Type: " << cmd_type << "\n";
        std::cout << "  - Succeeded Requests: " << stats.count << "\n";
        if (stats.count > 0) {
            std::cout << "  - Average Latency:    " << std::fixed << stats.get_average() << " ms\n";
            std::cout << "  - Maximum Latency:    " << std::fixed << stats.max_latency_ms << " ms\n";
            // std::cout << "  - Latency Variance:   " << std::fixed << stats.get_variance() << " ms^2\n";
            std::cout << "  - Latency Std Dev:    " << std::fixed << stats.get_std_dev() << " ms\n";
        }
    }
    std::cout << "--------------------------------\n";

    if (!errors_map.empty()) {
        std::cout << "\n--- Server Response Counts ---\n";
        for (const auto& pair : errors_map) {
            std::cout << "  - " << pair.first << ": " << pair.second << "\n";
        }
        std::cout << "--------------------------------\n";
    }
}


/**
 * @brief Processes the receive buffer to parse complete memcached responses.
 * @param buffer The string buffer containing raw data from the socket.
 * @param requests The queue of in-flight requests.
 * @param stats The statistics map to update on success.
 * @param errors The errors map to update on failure or non-success responses.
 * @return True if a response was processed, false if more data is needed.
 */
bool process_responses(
    std::string& buffer, 
    std::queue<Request>& requests, 
    std::map<std::string, Stats>& stats, 
    std::map<std::string, long long>& errors) 
{
    if (requests.empty()) {
        return false;
    }

    const auto& current_request = requests.front();
    const std::string& cmd_type = current_request.command_type;

    size_t end_of_line_pos = buffer.find("\r\n");
    if (end_of_line_pos == std::string::npos) {
        return false; // Incomplete response line, need more data.
    }

    std::string response_line = buffer.substr(0, end_of_line_pos);
    
    bool request_finished = false;
    bool is_success = false;
    std::string response_key;

    if (cmd_type == "add" || cmd_type == "replace" || cmd_type == "set") {
        if (response_line == "STORED") {
            request_finished = true;
            is_success = true;
            response_key = "STORED";
        } else if (response_line == "NOT_STORED") {
            request_finished = true;
            response_key = "NOT_STORED";
        } else if (response_line.rfind("SERVER_ERROR", 0) == 0 || response_line.rfind("CLIENT_ERROR", 0) == 0) {
            request_finished = true;
            response_key = "SERVER/CLIENT_ERROR";
        }
        
        if(request_finished) {
            buffer.erase(0, end_of_line_pos + 2);
        }

    } else if (cmd_type == "get") {
        if (response_line == "END") { // Key not found
            request_finished = true;
            response_key = "NOT_FOUND (END)";
            buffer.erase(0, end_of_line_pos + 2);
        } else if (response_line.rfind("VALUE ", 0) == 0) {
            // Format: VALUE <key> <flags> <bytes>\r\n<data>\r\nEND\r\n
            std::stringstream ss(response_line);
            std::string token;
            long long data_len = -1;
            ss >> token >> token >> token >> data_len; // VALUE, key, flags, bytes

            if (data_len >= 0) {
                size_t data_start = end_of_line_pos + 2;
                size_t expected_end_marker_pos = data_start + data_len + 2; // +2 for the data's \r\n
                // Check if the entire block is in the buffer: data + \r\n + END + \r\n
                if (buffer.length() >= expected_end_marker_pos + 5 && buffer.substr(expected_end_marker_pos, 5) == "END\r\n") {
                    request_finished = true;
                    is_success = true;
                    response_key = "FOUND (VALUE)";
                    buffer.erase(0, expected_end_marker_pos + 5);
                }
            }
        } else if (response_line.rfind("SERVER_ERROR", 0) == 0 || response_line.rfind("CLIENT_ERROR", 0) == 0) {
            request_finished = true;
            response_key = "SERVER/CLIENT_ERROR";
            buffer.erase(0, end_of_line_pos + 2);
        }
    }

    if (request_finished) {
        if (is_success) {
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> latency = end_time - current_request.send_time;
            stats[cmd_type].update(latency.count());
        }
        errors[response_key]++;
        requests.pop();
        return true; // A response was processed, try to process the next one in the buffer.
    }

    return false; // Not enough data for a full response yet.
}


// --- Main Logic ---

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <trace_file> [--live]" << std::endl;
        return 1;
    }
    const char* trace_filename = argv[1];
    bool live_updates_enabled = false;

    // --- Parse command line options ---
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--live") {
            live_updates_enabled = true;
        }
    }

    std::ifstream trace_file(trace_filename);
    if (!trace_file.is_open()) {
        std::cerr << "Error: Could not open trace file '" << trace_filename << "'" << std::endl;
        return 1;
    }

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, HOST, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock_fd);
        return 1;
    }

    if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return 1;
    }

    std::cout << "Connected to memcached at " << HOST << ":" << PORT << std::endl;
    
    if (!make_socket_non_blocking(sock_fd)) {
        close(sock_fd);
        return 1;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        close(sock_fd);
        return 1;
    }

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    event.data.fd = sock_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event) == -1) {
        perror("epoll_ctl");
        close(sock_fd);
        close(epoll_fd);
        return 1;
    }

    // --- Initialize State ---
    std::queue<Request> in_flight_requests;
    std::map<std::string, Stats> statistics;
    std::map<std::string, long long> response_counts;
    std::string receive_buffer;
    char read_buffer[BUFFER_SIZE];
    bool trace_file_done = false;
    long total_requests_sent = 0;

    std::cout << "Starting trace replay with MAX_IN_FLIGHT = " << MAX_IN_FLIGHT << std::endl;
    if (!live_updates_enabled) {
        std::cout << "Live updates are disabled. Use the --live flag to enable real-time progress." << std::endl;
    }

    // --- Main Event Loop ---
    while (!trace_file_done || !in_flight_requests.empty()) {
        struct epoll_event events[1];
        int timeout = (in_flight_requests.size() < MAX_IN_FLIGHT && !trace_file_done) ? 0 : -1;
        int n_events = epoll_wait(epoll_fd, events, 1, timeout);

        if (n_events == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n_events; ++i) {
            // --- Handle Readable Socket (Responses) ---
            if (events[i].events & EPOLLIN) {
                while (true) {
                    ssize_t count = read(sock_fd, read_buffer, BUFFER_SIZE);
                    if (count == -1) {
                        if (errno != EAGAIN) perror("read");
                        break; 
                    } else if (count == 0) {
                        std::cerr << "Server closed connection." << std::endl;
                        goto cleanup;
                    }
                    receive_buffer.append(read_buffer, count);
                }
                while(process_responses(receive_buffer, in_flight_requests, statistics, response_counts));
            }

            // --- Handle Writable Socket (Sending Requests) ---
            if (events[i].events & EPOLLOUT) {
                while (in_flight_requests.size() < MAX_IN_FLIGHT && !trace_file_done) {
                    std::streampos before_read_pos = trace_file.tellg();
                    
                    std::string line1, line2;
                    if (!std::getline(trace_file, line1)) {
                        trace_file_done = true;
                        break;
                    }

                    if (!line1.empty() && line1.back() == '\r') {
                        line1.pop_back();
                    }

                    Request new_request;
                    if (line1.rfind("add ", 0) == 0 || line1.rfind("replace ", 0) == 0 || line1.rfind("set ", 0) == 0) {
                        if (!std::getline(trace_file, line2)) {
                             std::cerr << "Warning: Incomplete request at end of file for command: " << line1 << std::endl;
                             trace_file_done = true;
                             break;
                        }
                        if (!line2.empty() && line2.back() == '\r') {
                            line2.pop_back();
                        }
                        new_request.full_command = line1 + "\r\n" + line2 + "\r\n";
                        new_request.command_type = line1.substr(0, line1.find(' '));
                    } else if (line1.rfind("get ", 0) == 0) {
                        new_request.full_command = line1 + "\r\n";
                        new_request.command_type = "get";
                    } else {
                        if(!line1.empty()) std::cerr << "Warning: Skipping malformed line: " << line1 << std::endl;
                        continue;
                    }

                    ssize_t bytes_sent = write(sock_fd, new_request.full_command.c_str(), new_request.full_command.length());

                    if (bytes_sent == -1) {
                        if (errno == EAGAIN) {
                             trace_file.clear(); 
                             trace_file.seekg(before_read_pos);
                             break; 
                        }
                        perror("write");
                        goto cleanup;
                    }
                    new_request.send_time = std::chrono::high_resolution_clock::now();
                    in_flight_requests.push(new_request);
                    total_requests_sent++;
                    if (live_updates_enabled && total_requests_sent % 5000 == 0) {
                        std::cout << "Sent: " << total_requests_sent << " | In-Flight: " << in_flight_requests.size() << " | Resp Buffered: " << receive_buffer.length() << "  \r" << std::flush;
                    }
                }
            }
        }
        while(process_responses(receive_buffer, in_flight_requests, statistics, response_counts));
    }

cleanup:
    std::cout << "\nTrace file processed. Waiting for " << in_flight_requests.size() << " final responses..." << std::endl;
    
    // Final loop to drain remaining responses
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(!in_flight_requests.empty() && std::chrono::steady_clock::now() < deadline){
         struct epoll_event final_events[1];
         int n_events = epoll_wait(epoll_fd, final_events, 1, 1000); // 1s timeout
         if (n_events > 0 && (final_events[0].events & EPOLLIN)) {
             while (true) {
                ssize_t count = read(sock_fd, read_buffer, BUFFER_SIZE);
                 if (count > 0) {
                     receive_buffer.append(read_buffer, count);
                 } else if (count == 0) {
                     std::cerr << "Server disconnected while waiting for final responses." << std::endl;
                     goto end_of_program;
                 } else { 
                     if(errno != EAGAIN) perror("read (final)");
                     break;
                 }
             }
         } else if (n_events == 0) {
             if (live_updates_enabled) {
                std::cout << "Waiting for " << in_flight_requests.size() << " responses... \r" << std::flush;
             }
         }
         while(process_responses(receive_buffer, in_flight_requests, statistics, response_counts));
    }
    if(!in_flight_requests.empty()){
        std::cerr << "\nWarning: Timed out waiting for final responses. " << in_flight_requests.size() << " requests may have been lost." << std::endl;
    }


end_of_program:
    print_stats(statistics, response_counts);

    close(sock_fd);
    close(epoll_fd);
    trace_file.close();

    return 0;
}
