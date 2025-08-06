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

// Networking includes
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>

// File storing time dilation factor (format: <factor> / 1000)
#define DILATION_KNOB "/sys/kernel/sleep_dilation/dilation_factor"

// Read time dilation factor and convert to double
double read_dilation_factor() {
    std::ifstream dilation_file(DILATION_KNOB);
    if (!dilation_file.is_open()) {
        std::cerr << "Failed to open dilation factor file: " << DILATION_KNOB << std::endl;
        return 1.0; // Default to no dilation
    }
    double factor;
    dilation_file >> factor;
    if (dilation_file.fail()) {
        std::cerr << "Failed to read dilation factor from file: " << DILATION_KNOB << std::endl;
        return 1.0; // Default to no dilation
    }
    return factor / 1000.0; // Convert to double
}


// --- Configuration ---
const char* HOST = "127.0.0.1";
const int PORT = 11211;
const long long DEFAULT_OPS_TARGET = 1000000;
const std::string BENCHMARK_KEY = "microbench_key";
const size_t DEFAULT_BUFFER_SIZE = 1;
const size_t DEFAULT_VALUE_SIZE_KB = 1;

// --- Shared State ---
long long successful_reads = 0;
long long failed_reads = 0; // New counter for failed reads
long long successful_writes = 0;
volatile bool stop_flag = false;

// Latency data will be collected here
std::vector<double> read_latencies;
std::vector<double> write_latencies;


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
 * @brief Establishes a standard blocking TCP connection. Used for simple, synchronous setup.
 * @return The socket file descriptor, or -1 on failure.
 */
int connect_to_memcached_blocking() {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, HOST, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock_fd);
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return -1;
    }
    return sock_fd;
}


/**
 * @brief Establishes a non-blocking TCP connection. Used by worker threads with epoll.
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
 * @brief The task for the reader thread. Uses epoll and an in-flight buffer.
 * @param buffer_size The maximum number of requests to have in-flight.
 * @param value_size The size of the value being read, for delay calculation.
 * @param ops_target The number of operations to complete.
 * @param inject_delays Whether to inject an artificial processing delay.
 */
void reader_task(size_t buffer_size, size_t value_size, long long ops_target, bool inject_delays) {
    int sock_fd = connect_to_memcached_nonblocking();
    if (sock_fd == -1) {
        std::cerr << "Reader thread failed to connect." << std::endl;
        stop_flag = true;
        return;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("reader epoll_create1");
        close(sock_fd);
        return;
    }

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    event.data.fd = sock_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event) == -1) {
        perror("reader epoll_ctl");
        close(sock_fd);
        close(epoll_fd);
        return;
    }

    std::queue<InFlightMarker> in_flight_queue;
    std::string receive_buffer;
    std::string get_command = "get " + BENCHMARK_KEY + "\r\n";
    bool is_writable = true;
    long long reads_sent = 0;
    bool target_sent = false;

    while (!target_sent || !in_flight_queue.empty()) {
        struct epoll_event events[1];
        int timeout = (target_sent) ? 100 : -1;
        int n_events = epoll_wait(epoll_fd, events, 1, timeout);
        
        if (n_events < 0) {
            perror("reader epoll_wait");
            break;
        }
        if (n_events == 0) {
            if (in_flight_queue.empty()) break;
            continue;
        }
        if (stop_flag) {
            target_sent = true; // Stop sending if the other thread finished
        }

        if (events[0].events & EPOLLIN) {
            char read_buf[4096];
            while (true) {
                ssize_t count = read(sock_fd, read_buf, sizeof(read_buf));
                if (count > 0) {
                    receive_buffer.append(read_buf, count);
                } else {
                    break;
                }
            }
            
            // --- Robust Parsing Logic ---
            while (!in_flight_queue.empty()) {
                if (receive_buffer.rfind("VALUE", 0) == 0) {
                    size_t line_end = receive_buffer.find("\r\n");
                    if (line_end == std::string::npos) break; 

                    std::stringstream ss(receive_buffer.substr(0, line_end));
                    std::string v, k, f;
                    size_t bytes;
                    ss >> v >> k >> f >> bytes;

                    // Parse response
                    // header + \r\n + data + \r\n + END\r\n
                    size_t expected_total_size = line_end + 2 + bytes + 2 + 5;
                    if (receive_buffer.length() >= expected_total_size) {
                        auto now = std::chrono::high_resolution_clock::now();
                        auto& marker = in_flight_queue.front();
                        std::chrono::duration<double, std::milli> latency = now - marker.send_time;
                        
                        read_latencies.push_back(latency.count());
                        successful_reads++;

                        if (inject_delays) {
                            // Inject response delay (ns)
                            // double dilation_factor = read_dilation_factor();
                            double delay_factor = 12;
                            double bias = -200;
                            double coef = value_size + bias;
                            double delay_ns = coef * delay_factor; // * dilation_factor;
                            double start = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                            double end = start;
                            
                            while (end - start < delay_ns) {
                                end = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                            }

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
        
        if (events[0].events & EPOLLOUT) {
            is_writable = true;
        }

        while (is_writable && !target_sent && in_flight_queue.size() < buffer_size) {
            auto send_time = std::chrono::high_resolution_clock::now();
            if (send(sock_fd, get_command.c_str(), get_command.length(), 0) > 0) {
                in_flight_queue.push({send_time});
                reads_sent++;
                if (reads_sent >= ops_target) {
                    target_sent = true;
                    stop_flag = true;
                }
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    is_writable = false;
                } else {
                    perror("reader send");
                    goto reader_cleanup;
                }
            }
        }
    }

reader_cleanup:
    stop_flag = true;
    close(sock_fd);
    close(epoll_fd);
}

/**
 * @brief The task for the writer thread. Uses epoll and an in-flight buffer.
 * @param buffer_size The maximum number of requests to have in-flight.
 * @param value_size The size of the value to write.
 * @param ops_target The number of operations to complete.
 */
void writer_task(size_t buffer_size, size_t value_size, long long ops_target) {
    int sock_fd = connect_to_memcached_nonblocking();
    if (sock_fd == -1) {
        std::cerr << "Writer thread failed to connect." << std::endl;
        stop_flag = true;
        return;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("writer epoll_create1");
        close(sock_fd);
        return;
    }

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    event.data.fd = sock_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &event) == -1) {
        perror("writer epoll_ctl");
        close(sock_fd);
        close(epoll_fd);
        return;
    }

    std::queue<InFlightMarker> in_flight_queue;
    std::string receive_buffer;
    std::string update_value(value_size, 'A');
    bool is_writable = true;
    long long writes_sent = 0;
    bool target_sent = false;

    while (!target_sent || !in_flight_queue.empty()) {
        struct epoll_event events[1];
        int timeout = (target_sent) ? 100 : -1;
        int n_events = epoll_wait(epoll_fd, events, 1, timeout);

        if (n_events < 0) {
            perror("writer epoll_wait");
            break;
        }
        if (n_events == 0) {
            if(in_flight_queue.empty()) break;
            continue;
        }
        
        if (stop_flag) {
            target_sent = true; // Stop sending if the other thread finished
        }

        if (events[0].events & EPOLLIN) {
            char read_buf[4096];
            while (true) {
                ssize_t count = read(sock_fd, read_buf, sizeof(read_buf));
                if (count > 0) {
                    receive_buffer.append(read_buf, count);
                } else {
                    break;
                }
            }
            
            size_t pos;
            while ((pos = receive_buffer.find("STORED\r\n")) != std::string::npos) {
                auto now = std::chrono::high_resolution_clock::now();
                auto& marker = in_flight_queue.front();
                std::chrono::duration<double, std::milli> latency = now - marker.send_time;

                write_latencies.push_back(latency.count());
                successful_writes++;
                
                in_flight_queue.pop();
                receive_buffer.erase(0, pos + 8);
            }
        }

        if (events[0].events & EPOLLOUT) {
            is_writable = true;
        }

        while (is_writable && !target_sent && in_flight_queue.size() < buffer_size) {
            update_value[0]++;
            std::string replace_command = "replace " + BENCHMARK_KEY + " 0 0 " + std::to_string(update_value.length()) + "\r\n" + update_value + "\r\n";
            auto send_time = std::chrono::high_resolution_clock::now();
            if (send(sock_fd, replace_command.c_str(), replace_command.length(), 0) > 0) {
                in_flight_queue.push({send_time});
                writes_sent++;
                if (writes_sent >= ops_target) {
                    target_sent = true;
                    stop_flag = true;
                }
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    is_writable = false;
                } else {
                    perror("writer send");
                    goto writer_cleanup;
                }
            }
        }
    }

writer_cleanup:
    stop_flag = true;
    close(sock_fd);
    close(epoll_fd);
}

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " [options]\n\n"
              << "Options:\n"
              << "  --requests <N>       Set the number of operations for the winning thread (default: " << DEFAULT_OPS_TARGET << ").\n"
              << "  --buffer_size <N>    Set the in-flight buffer size for each thread (default: " << DEFAULT_BUFFER_SIZE << ").\n"
              << "  --item_size <N>      Set the size of the memcached value in KB (default: " << DEFAULT_VALUE_SIZE_KB << ").\n"
              << "  --inject_delays      Enable artificial client-side processing delays in the reader thread.\n"
              << "  -h, --help           Display this help message.\n";
}

void print_latency_stats(const std::string& name, std::vector<double>& latencies) {
    if (latencies.empty()) {
        std::cout << "No " << name << " latencies recorded." << std::endl;
        return;
    }

    std::sort(latencies.begin(), latencies.end());
    
    double sum = 0.0;
    for(double val : latencies) {
        sum += val;
    }
    double average = sum / latencies.size();

    double p90 = latencies[static_cast<size_t>(std::ceil(0.90 * latencies.size())) - 1];
    double p99 = latencies[static_cast<size_t>(std::ceil(0.99 * latencies.size())) - 1];
    double p999 = latencies[static_cast<size_t>(std::ceil(0.999 * latencies.size())) - 1];

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
        std::cout << "Artificial reader delays are ENABLED." << std::endl;
    }


    // --- SETUP ---
    // Assuming the server is in a clean state.
    std::cout << "Initializing benchmark key with 'add'..." << std::endl;
    int init_sock = connect_to_memcached_blocking();
    if (init_sock == -1) {
        std::cerr << "Failed to connect for initialization. Aborting." << std::endl;
        return 1;
    }

    std::string initial_value(value_size_bytes, 'A');
    std::string add_command = "add " + BENCHMARK_KEY + " 0 0 " + std::to_string(initial_value.length()) + "\r\n" + initial_value + "\r\n";
    send(init_sock, add_command.c_str(), add_command.length(), 0);
    
    char init_response[32] = {0};
    int init_bytes = recv(init_sock, init_response, sizeof(init_response) - 1, 0);
    close(init_sock);

    if (init_bytes <= 0 || strncmp(init_response, "STORED", 6) != 0) {
        std::cerr << "Failed to 'add' initial key value. Response: " << init_response << std::endl;
        std::cerr << "Please ensure the server is empty or the key does not exist before running." << std::endl;
        return 1;
    }
    std::cout << "Initialization complete." << std::endl;

    // Pre-allocate memory to avoid reallocations during the benchmark
    read_latencies.reserve(ops_target);
    write_latencies.reserve(ops_target);

    // --- BENCHMARK EXECUTION ---
    std::cout << "Starting benchmark. Running until " << ops_target << " reads or " << ops_target << " writes occur..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();

    std::thread reader_thread(reader_task, buffer_size, value_size_bytes, ops_target, inject_delays);
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
    
    print_latency_stats("Read", read_latencies);
    print_latency_stats("Write", write_latencies);

    // --- CLEANUP ---
    std::cout << "\nCleaning up benchmark key..." << std::endl;
    int cleanup_sock = connect_to_memcached_blocking();
    if (cleanup_sock != -1) {
        std::string delete_command = "delete " + BENCHMARK_KEY + "\r\n";
        send(cleanup_sock, delete_command.c_str(), delete_command.length(), 0);
        
        char cleanup_response[32] = {0};
        recv(cleanup_sock, cleanup_response, sizeof(cleanup_response) - 1, 0);
        close(cleanup_sock);

        if (strncmp(cleanup_response, "DELETED", 7) == 0) {
            std::cout << "Key successfully deleted." << std::endl;
        } else if (strncmp(cleanup_response, "NOT_FOUND", 9) == 0) {
            std::cout << "Key was already gone." << std::endl;
        } else {
            std::cout << "Cleanup response: " << cleanup_response << std::endl;
        }
    } else {
        std::cerr << "Failed to connect for cleanup." << std::endl;
    }

    return 0;
}
