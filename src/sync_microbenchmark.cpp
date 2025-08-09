#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <queue>
#include <algorithm> // For std::sort
#include <cmath>     // For std::ceil
#include <sstream>
#include <fstream>
#include <stdexcept> // For std::runtime_error
#include <mutex>
#include <condition_variable>

// Networking includes
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/un.h> // Include for Unix domain sockets

// --- Global Flags & Variables ---
// Delay injection value in nanoseconds
double delay_ns = 0.0;

// Scaling factor for injected delays. Set via command line.
double dilation_scaling_factor = 0.0;

// File storing time dilation factor (format: <factor> / 1000)
#define DILATION_KNOB "/sys/kernel/sleep_dilation/dilation_factor"

// Unix domain socket path
#define UNIX_SOCKET_PATH "/home/michael/ISCA_2025_results/tmp/sync_microbench.sock"

/**
 * @brief Reads the system's time dilation factor from a kernel file.
 * @return The dilation factor as a double. Defaults to 1.0 if file cannot be read.
 */
double read_dilation_factor() {
    std::ifstream dilation_file(DILATION_KNOB);
    if (!dilation_file.is_open()) {
        // This is not a fatal error, as the system may not have the knob.
        // We can just return 1.0 for no dilation.
        return 1.0;
    }
    uint factor;
    dilation_file >> factor;
    if (dilation_file.fail()) {
        std::cerr << "Warning: Failed to read dilation factor from file: " << DILATION_KNOB << std::endl;
        return 1.0; // Default to no dilation
    }

    // This specific calculation might be tuned for a particular environment.
    return (double) factor / 1000.0;
}


// --- Configuration ---
const long long DEFAULT_OPS_TARGET = 1000000;
const std::string BENCHMARK_KEY = "microbench_key";
const size_t DEFAULT_BUFFER_SIZE = 1;
const size_t DEFAULT_VALUE_SIZE_KB = 1;
const size_t READ_BUFFER_SIZE = 65536; // 64KB read buffer for efficiency

// --- Shared State ---
long long successful_reads = 0;
long long failed_reads = 0;
long long successful_writes = 0;
volatile bool stop_flag = false;

// Latency data will be collected here
std::vector<double> read_latencies;
std::vector<double> write_latencies;
std::vector<double> delayed_read_latencies; // For injected delays
std::mutex latencies_mutex; // Mutex to protect latency vectors

// Represents a single in-flight request.
struct InFlightMarker {
    std::chrono::high_resolution_clock::time_point send_time;
};

/**
 * @brief Sets a file descriptor to non-blocking mode.
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
 * @brief Establishes a standard blocking Unix domain socket connection.
 * @return The socket file descriptor, or -1 on failure.
 */
int connect_to_memcached_blocking() {
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket(AF_UNIX)");
        return -1;
    }

    struct sockaddr_un serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sun_family = AF_UNIX;
    strncpy(serv_addr.sun_path, UNIX_SOCKET_PATH, sizeof(serv_addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "connect failed for socket path %s: %s\n", UNIX_SOCKET_PATH, strerror(errno));
        close(sock_fd);
        return -1;
    }
    return sock_fd;
}


/**
 * @brief Establishes a non-blocking connection using a Unix socket.
 * @return The socket file descriptor, or -1 on failure.
 */
int connect_to_memcached_nonblocking() {
    int sock_fd = connect_to_memcached_blocking();
    if (sock_fd == -1) {
        return -1;
    }
    
    if (!make_socket_non_blocking(sock_fd)) {
        close(sock_fd);
        return -1;
    }
    return sock_fd;
}

/**
 * @brief Sends all data in a buffer over a socket using a busy-wait (spin) loop.
 * This function will block, consuming 100% CPU, until all data is sent or an
 * unrecoverable error occurs.
 * @param fd The socket file descriptor.
 * @param data The data to send.
 * @throws std::runtime_error on unrecoverable socket errors.
 */
void send_all(int fd, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.length()) {
        ssize_t sent_now = send(fd, data.c_str() + total_sent, data.length() - total_sent, 0);
        if (sent_now > 0) {
            total_sent += sent_now;
        } else if (sent_now == 0) {
            throw std::runtime_error("send() returned 0, peer has closed the connection.");
        } else { // sent_now < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // This is the busy-wait loop. It will continuously retry the send.
                continue;
            } else {
                // An unrecoverable error occurred.
                throw std::runtime_error(std::string("send() failed: ") + strerror(errno));
            }
        }
    }
}

/**
 * @brief Processes incoming data from the socket, parsing responses and updating stats.
 * @param sock_fd The socket file descriptor.
 * @param receive_buffer The buffer holding received data.
 * @param in_flight_queue The queue of in-flight requests.
 * @param value_size The expected size of the value for latency injection.
 * @param inject_delays Whether to inject artificial delays.
 * @param scaling_factor The factor to scale the delay by.
 */
void process_incoming_reads(int sock_fd, std::string& receive_buffer, std::queue<InFlightMarker>& in_flight_queue, size_t value_size, bool inject_delays, double scaling_factor) {
    char read_buf[READ_BUFFER_SIZE];
    while (true) {
        ssize_t count = read(sock_fd, read_buf, sizeof(read_buf));
        if (count > 0) {
            receive_buffer.append(read_buf, count);
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                throw std::runtime_error(std::string("read() failed: ") + strerror(errno));
            }
            break; // No more data to read for now
        }
    }

    while (!in_flight_queue.empty()) {
        if (receive_buffer.rfind("VALUE", 0) == 0) {
            size_t line_end = receive_buffer.find("\r\n");
            if (line_end == std::string::npos) break;

            std::stringstream ss(receive_buffer.substr(0, line_end));
            std::string v, k, f;
            size_t bytes;
            ss >> v >> k >> f >> bytes;

            size_t expected_total_size = line_end + 2 + bytes + 2 + 5;
            if (receive_buffer.length() >= expected_total_size) {
                auto now = std::chrono::high_resolution_clock::now();
                auto& marker = in_flight_queue.front();
                std::chrono::duration<double, std::milli> latency = now - marker.send_time;
                
                read_latencies.push_back(latency.count());
                successful_reads++;

                if (inject_delays) {
                    // *** MODIFICATION HERE ***
                    // Start with the base delay.
                    double final_delay_ns = delay_ns;

                    // If a command-line scaling factor is provided, apply it AND the system dilation factor.
                    if (scaling_factor > 0.0) {
                        double system_dilation = read_dilation_factor();
                        final_delay_ns *= system_dilation * scaling_factor;
                    }

                    double start = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::high_resolution_clock::now().time_since_epoch()
                                        ).count();
                    double end = start;
                    
                    while (end - start < final_delay_ns) {
                        end = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::high_resolution_clock::now().time_since_epoch()
                                ).count();
                    }
                    delayed_read_latencies.push_back(((end - start) / 1e6) + latency.count()); // Store delay in ms
                }
                in_flight_queue.pop();
                receive_buffer.erase(0, expected_total_size);
            } else {
                break;
            }
        } else if (receive_buffer.rfind("END\r\n", 0) == 0) {
            failed_reads++;
            in_flight_queue.pop();
            receive_buffer.erase(0, 5);
        } else {
            break;
        }
    }
}

/**
 * @brief The task for the reader thread.
 */
void reader_task(size_t buffer_size, size_t value_size, long long ops_target, bool inject_delays, double scaling_factor) {
    try {
        int sock_fd = connect_to_memcached_nonblocking();
        if (sock_fd == -1) throw std::runtime_error("Reader thread failed to connect.");

        int epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) throw std::runtime_error(std::string("reader epoll_create1: ") + strerror(errno));

        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = sock_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event) == -1) {
            close(sock_fd);
            throw std::runtime_error(std::string("reader epoll_ctl: ") + strerror(errno));
        }

        std::queue<InFlightMarker> in_flight_queue;
        std::string receive_buffer;
        long long reads_sent = 0;

        while (reads_sent < ops_target && !stop_flag) {
            // Try to send requests if we have capacity
            while (in_flight_queue.size() < buffer_size && reads_sent < ops_target && !stop_flag) {
                std::string get_command = "get " + BENCHMARK_KEY + "\r\n";
                auto send_time = std::chrono::high_resolution_clock::now();
                send_all(sock_fd, get_command);
                in_flight_queue.push({send_time});
                reads_sent++;
            }

            // Check for and process incoming responses
            struct epoll_event events[1];
            int n_events = epoll_wait(epoll_fd, events, 1, 0); // 0 timeout for non-blocking check
            if (n_events > 0) {
                process_incoming_reads(sock_fd, receive_buffer, in_flight_queue, value_size, inject_delays, scaling_factor);
            }
        }
        
        // After sending all requests, drain the remaining responses
        while (!in_flight_queue.empty() && !stop_flag) {
            struct epoll_event events[1];
            int n_events = epoll_wait(epoll_fd, events, 1, 100); // 100ms timeout
            if (n_events > 0) {
                process_incoming_reads(sock_fd, receive_buffer, in_flight_queue, value_size, inject_delays, scaling_factor);
            }
        }

        close(sock_fd);
        close(epoll_fd);

    } catch (const std::runtime_error& e) {
        std::cerr << "Reader thread exception: " << e.what() << std::endl;
    }
    stop_flag = true;
}

/**
 * @brief Processes incoming "STORED" responses from the socket.
 */
void process_incoming_writes(int sock_fd, std::string& receive_buffer, std::queue<InFlightMarker>& in_flight_queue) {
    char read_buf[READ_BUFFER_SIZE];
    while (true) {
        ssize_t count = read(sock_fd, read_buf, sizeof(read_buf));
        if (count > 0) {
            receive_buffer.append(read_buf, count);
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                throw std::runtime_error(std::string("read() failed: ") + strerror(errno));
            }
            break;
        }
    }

    size_t pos;
    while (!in_flight_queue.empty() && (pos = receive_buffer.find("STORED\r\n")) != std::string::npos) {
        auto& marker = in_flight_queue.front();
        auto now = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> latency = now - marker.send_time;

        write_latencies.push_back(latency.count());
        successful_writes++;
        
        in_flight_queue.pop();
        receive_buffer.erase(0, pos + 8);
    }
}

/**
 * @brief The task for the writer thread.
 */
void writer_task(size_t buffer_size, size_t value_size, long long ops_target) {
    try {
        int sock_fd = connect_to_memcached_nonblocking();
        if (sock_fd == -1) throw std::runtime_error("Writer thread failed to connect.");

        int epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) throw std::runtime_error(std::string("writer epoll_create1: ") + strerror(errno));

        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = sock_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event) == -1) {
            close(sock_fd);
            throw std::runtime_error(std::string("writer epoll_ctl: ") + strerror(errno));
        }

        std::queue<InFlightMarker> in_flight_queue;
        std::string receive_buffer;
        std::string update_value(value_size, 'A');
        long long writes_sent = 0;

        while (writes_sent < ops_target && !stop_flag) {
            while (in_flight_queue.size() < buffer_size && writes_sent < ops_target && !stop_flag) {
                update_value[0]++;
                std::string replace_command = "replace " + BENCHMARK_KEY + " 0 0 " + std::to_string(update_value.length()) + "\r\n" + update_value + "\r\n";
                auto send_time = std::chrono::high_resolution_clock::now();
                send_all(sock_fd, replace_command);
                in_flight_queue.push({send_time});
                writes_sent++;
            }

            struct epoll_event events[1];
            int n_events = epoll_wait(epoll_fd, events, 1, 0);
            if (n_events > 0) {
                process_incoming_writes(sock_fd, receive_buffer, in_flight_queue);
            }
        }

        while (!in_flight_queue.empty() && !stop_flag) {
            struct epoll_event events[1];
            int n_events = epoll_wait(epoll_fd, events, 1, 100);
            if (n_events > 0) {
                process_incoming_writes(sock_fd, receive_buffer, in_flight_queue);
            }
        }

        close(sock_fd);
        close(epoll_fd);

    } catch (const std::runtime_error& e) {
        std::cerr << "Writer thread exception: " << e.what() << std::endl;
    }
    stop_flag = true;
}

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " [options]\n\n"
              << "Options:\n"
              << "  --requests <N>           Set the number of operations for the winning thread (default: " << DEFAULT_OPS_TARGET << ").\n"
              << "  --buffer_size <N>        Set the in-flight buffer size for each thread (default: " << DEFAULT_BUFFER_SIZE << ").\n"
              << "  --item_size <N>          Set the size of the memcached value in KB (default: " << DEFAULT_VALUE_SIZE_KB << ").\n"
              << "  --inject_delays          Enable artificial client-side processing delays in the reader thread.\n"
              << "  --dilation_scaling <S>   Set a scaling factor for injected delays (default: 0.0, no scaling).\n"
              << "  -h, --help               Display this help message.\n";
}

void print_latency_stats(const std::string& name, std::vector<double>& latencies) {
    if (latencies.empty()) {
        std::cout << "No " << name << " latencies recorded." << std::endl;
        return;
    }

    std::vector<double> latencies_copy;
    {
        std::lock_guard<std::mutex> lock(latencies_mutex);
        latencies_copy = latencies;
    }
    std::sort(latencies_copy.begin(), latencies_copy.end());
    
    double sum = 0.0;
    for(double val : latencies_copy) {
        sum += val;
    }
    double average = sum / latencies_copy.size();

    size_t count = latencies_copy.size();
    if (count == 0) return;
    double p90 = latencies_copy[static_cast<size_t>(0.90 * count) -1];
    double p99 = latencies_copy[static_cast<size_t>(0.99 * count) -1];
    double p999 = latencies_copy[static_cast<size_t>(0.999 * count) -1];

    std::cout << "\n--- " << name << " Latency (ms) ---\n";
    std::cout << "Average: " << average << "\n";
    std::cout << "p90:     " << p90 << "\n";
    std::cout << "p99:     " << p99 << "\n";
    std::cout << "p99.9:   " << p999 << std::endl;
}

int main(int argc, char* argv[]) {
    long long ops_target = DEFAULT_OPS_TARGET;
    size_t buffer_size = DEFAULT_BUFFER_SIZE;
    size_t value_size_kb = DEFAULT_VALUE_SIZE_KB;
    bool inject_delays = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--inject_delays") {
            inject_delays = true;
        } else if (arg == "--dilation_scaling") {
            if (i + 1 < argc) {
                try {
                    dilation_scaling_factor = std::stod(argv[++i]);
                } catch(const std::exception& e) {
                    std::cerr << "Error: Invalid number for --dilation_scaling." << std::endl;
                    print_usage(argv[0]);
                    return 1;
                }
            } else {
                std::cerr << "Error: --dilation_scaling requires an argument." << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (arg == "--requests") {
            if (i + 1 < argc) {
                try {
                    ops_target = std::stoll(argv[++i]);
                } catch(const std::exception& e) {
                    std::cerr << "Error: Invalid number for --requests." << std::endl;
                    print_usage(argv[0]);
                    return 1;
                }
            } else {
                std::cerr << "Error: --requests requires an argument." << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (arg == "--buffer_size") {
            if (i + 1 < argc) {
                try {
                    buffer_size = std::stoul(argv[++i]);
                } catch(const std::exception& e) {
                    std::cerr << "Error: Invalid number for --buffer_size." << std::endl;
                    print_usage(argv[0]);
                    return 1;
                }
            } else {
                std::cerr << "Error: --buffer_size requires an argument." << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (arg == "--item_size") {
            if (i + 1 < argc) {
                try {
                    value_size_kb = std::stoul(argv[++i]);
                } catch(const std::exception& e) {
                    std::cerr << "Error: Invalid number for --item_size." << std::endl;
                    print_usage(argv[0]);
                    return 1;
                }
            } else {
                std::cerr << "Error: --item_size requires an argument." << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else {
            std::cerr << "Error: Unknown argument '" << arg << "'" << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }
    size_t value_size_bytes = value_size_kb * 1024;
    std::cout << "Using target operations: " << ops_target << std::endl;
    std::cout << "Using in-flight buffer size: " << buffer_size << std::endl;
    std::cout << "Using value size: " << value_size_kb << " KB (" << value_size_bytes << " bytes)" << std::endl;
    if (inject_delays) {
        // Delay structure:
        //  1. Constant added to account for instrumentation overheads on all GETs
        //  2. Delay proportional to value size
        //  3. Linear factor between 8KB - 16KB to handle change in memcpy() instrumentation
        //      - Does not kick in under 8KB, does not increase after 16KB
        const double const_offset = 5000; // constant delay offset for all sizes
        const double offset_8KB = 100000; // extra delay when reaching 8KB
        const double offset_16KB = 80000; // extra delay per byte between 8KB and 16KB
        const double coef = 0.25; // ns of delay per byte, only apply this before 8KB

        // Calculate delay based on value size
        delay_ns = const_offset;
        if (value_size_bytes < 8192) {
            delay_ns += coef * value_size_bytes;
        }
        if (value_size_bytes >= 8192) {
            delay_ns += offset_8KB;
        }
        if (value_size_bytes >= 16384) {
            delay_ns += offset_16KB;
        }
        double delay_ms = delay_ns / 1e6; // Convert to milliseconds
        std::cout << "Artificial reader delays are ENABLED: " << delay_ms << " ms per read." << std::endl;
        if (dilation_scaling_factor > 0.0) {
            std::cout << "Dilation scaling is ENABLED with factor: " << dilation_scaling_factor << std::endl;
        }
    }


    // --- SETUP ---
    std::cout << "Initializing benchmark key with 'add'..." << std::endl;
    int init_sock = connect_to_memcached_blocking();
    if (init_sock == -1) {
        std::cerr << "Failed to connect for initialization. Aborting." << std::endl;
        return 1;
    }

    std::string initial_value(value_size_bytes, 'A');
    std::string add_command = "add " + BENCHMARK_KEY + " 0 0 " + std::to_string(initial_value.length()) + "\r\n" + initial_value + "\r\n";
    
    try {
        send_all(init_sock, add_command);
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to send initial 'add' command: " << e.what() << std::endl;
        close(init_sock);
        return 1;
    }
    
    char init_response[32] = {0};
    recv(init_sock, init_response, sizeof(init_response) - 1, 0);
    close(init_sock);

    if (strncmp(init_response, "STORED", 6) != 0) {
        std::cerr << "Failed to 'add' initial key value. Response: " << init_response << std::endl;
        std::cerr << "Please ensure the server is empty or the key does not exist before running." << std::endl;
        return 1;
    }
    std::cout << "Initialization complete." << std::endl;

    read_latencies.reserve(ops_target);
    write_latencies.reserve(ops_target);
    delayed_read_latencies.reserve(ops_target); // Reserve space for delay latencies

    // --- BENCHMARK EXECUTION ---
    std::cout << "Starting benchmark. Running until " << ops_target << " reads or " << ops_target << " writes occur..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();

    // Pass the new scaling factor to the reader thread
    std::thread reader_thread(reader_task, buffer_size, value_size_bytes, ops_target, inject_delays, dilation_scaling_factor);
    std::thread writer_thread(writer_task, buffer_size, value_size_bytes, ops_target);

    reader_thread.join();
    writer_thread.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end_time - start_time;

    // --- RESULTS ---
    std::cout << "\n--- Benchmark Finished ---\n";
    std::cout << "Total duration: " << duration.count() << " seconds" << std::endl;
    
    std::cout << "Successful reads:  " << successful_reads << std::endl;
    std::cout << "Failed reads:      " << failed_reads << std::endl;
    std::cout << "Successful writes: " << successful_writes << std::endl;
    
    long long difference = successful_reads - successful_writes;
    std::cout << "Difference (#Reads - #Writes): " << difference << std::endl;
    double ratio = (successful_writes > 0) ?
            static_cast<double>(successful_reads) / successful_writes : 0.0;
    std::cout << "Read/Write Ratio:  " << ratio << std::endl;
    
    if (inject_delays) {
        std::cout << "Injected Delays (ms): " << delay_ns / 1e6 << std::endl;
        print_latency_stats("Read", delayed_read_latencies);
    }
    else print_latency_stats("Read", read_latencies);
    print_latency_stats("Write", write_latencies);

    // --- CLEANUP ---
    std::cout << "\nCleaning up benchmark key..." << std::endl;
    int cleanup_sock = connect_to_memcached_blocking();
    if (cleanup_sock != -1) {
        std::string delete_command = "delete " + BENCHMARK_KEY + "\r\n";
        try {
            send_all(cleanup_sock, delete_command);
            char cleanup_response[32] = {0};
            recv(cleanup_sock, cleanup_response, sizeof(cleanup_response) - 1, 0);
            if (strncmp(cleanup_response, "DELETED", 7) == 0) {
                std::cout << "Key successfully deleted." << std::endl;
            } else if (strncmp(cleanup_response, "NOT_FOUND", 9) == 0) {
                std::cout << "Key was already gone." << std::endl;
            } else {
                std::cout << "Cleanup response: " << cleanup_response << std::endl;
            }
        } catch (const std::runtime_error& e) {
            std::cerr << "Cleanup failed: " << e.what() << std::endl;
        }
        close(cleanup_sock);
    } else {
        std::cerr << "Failed to connect for cleanup." << std::endl;
    }

    return 0;
}
