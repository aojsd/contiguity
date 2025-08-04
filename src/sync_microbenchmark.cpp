#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <queue>

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
const long long READ_TARGET = 1000000;
const long long WRITE_TARGET = 1000000;
const std::string BENCHMARK_KEY = "microbench_key";
const size_t DEFAULT_BUFFER_SIZE = 1;
const size_t DEFAULT_VALUE_SIZE_KB = 1;

// --- Shared State ---
long long successful_reads = 0;
long long successful_writes = 0;
volatile bool stop_flag = false;

// Represents a single in-flight request. Can be empty for this use case.
struct InFlightMarker {};

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
 */
void reader_task(size_t buffer_size) {
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

    // Loop while we haven't hit the send target OR while there are still requests to get responses for.
    while ((!stop_flag && reads_sent < READ_TARGET) || !in_flight_queue.empty()) {
        struct epoll_event events[1];
        // If we are just draining, don't wait forever. A short timeout is fine.
        int timeout = (!stop_flag && reads_sent < READ_TARGET) ? -1 : 100;
        int n_events = epoll_wait(epoll_fd, events, 1, timeout);
        
        if (n_events < 0) {
            perror("reader epoll_wait");
            break;
        }
        if (n_events == 0) { // Timeout occurred while draining
            if(in_flight_queue.empty()) break;
            continue;
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
            while ((pos = receive_buffer.find("END\r\n")) != std::string::npos) {
                successful_reads++;
                in_flight_queue.pop();
                receive_buffer.erase(0, pos + 5);
            }
        }
        
        if (events[0].events & EPOLLOUT) {
            is_writable = true;
        }

        // This loop now controls sending based on the number of requests SENT.
        while (is_writable && !stop_flag && reads_sent < READ_TARGET && in_flight_queue.size() < buffer_size) {
            if (send(sock_fd, get_command.c_str(), get_command.length(), 0) > 0) {
                in_flight_queue.push({});
                reads_sent++;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    is_writable = false;
                } else {
                    perror("reader send");
                    goto reader_cleanup;
                }
            }
        }
        
        // If this thread has finished sending all its requests, set the stop flag.
        if (reads_sent >= READ_TARGET) {
            stop_flag = true;
        }
    }

reader_cleanup:
    stop_flag = true; // Ensure flag is set on error exit
    close(sock_fd);
    close(epoll_fd);
}

/**
 * @brief The task for the writer thread. Uses epoll and an in-flight buffer.
 * @param buffer_size The maximum number of requests to have in-flight.
 * @param value_size The size of the value to write.
 */
void writer_task(size_t buffer_size, size_t value_size) {
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

    // Loop while we haven't hit the send target OR while there are still requests to get responses for.
    while ((!stop_flag && writes_sent < WRITE_TARGET) || !in_flight_queue.empty()) {
        struct epoll_event events[1];
        int timeout = (!stop_flag && writes_sent < WRITE_TARGET) ? -1 : 100;
        int n_events = epoll_wait(epoll_fd, events, 1, timeout);

        if (n_events < 0) {
            perror("writer epoll_wait");
            break;
        }
        if (n_events == 0) {
            if(in_flight_queue.empty()) break;
            continue;
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
                successful_writes++;
                in_flight_queue.pop();
                receive_buffer.erase(0, pos + 8);
            }
        }

        if (events[0].events & EPOLLOUT) {
            is_writable = true;
        }

        while (is_writable && !stop_flag && writes_sent < WRITE_TARGET && in_flight_queue.size() < buffer_size) {
            update_value[0]++;
            std::string replace_command = "replace " + BENCHMARK_KEY + " 0 0 " + std::to_string(update_value.length()) + "\r\n" + update_value + "\r\n";
            if (send(sock_fd, replace_command.c_str(), replace_command.length(), 0) > 0) {
                in_flight_queue.push({});
                writes_sent++;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    is_writable = false;
                } else {
                    perror("writer send");
                    goto writer_cleanup;
                }
            }
        }
        
        if (writes_sent >= WRITE_TARGET) {
            stop_flag = true;
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
              << "  --buffer_size <N>    Set the in-flight buffer size for each thread (default: " << DEFAULT_BUFFER_SIZE << ").\n"
              << "  --value_size_kb <N>  Set the size of the memcached value in KB (default: " << DEFAULT_VALUE_SIZE_KB << ").\n"
              << "  -h, --help           Display this help message.\n";
}

int main(int argc, char* argv[]) {
    size_t buffer_size = DEFAULT_BUFFER_SIZE;
    size_t value_size_kb = DEFAULT_VALUE_SIZE_KB;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
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
        } else if (arg == "--value_size_kb") {
            if (i + 1 < argc) {
                try {
                    value_size_kb = std::stoul(argv[++i]);
                } catch(const std::exception& e) {
                    std::cerr << "Error: Invalid number for --value_size_kb." << std::endl;
                    print_usage(argv[0]);
                    return 1;
                }
            } else {
                std::cerr << "Error: --value_size_kb requires an argument." << std::endl;
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
    std::cout << "Using in-flight buffer size: " << buffer_size << std::endl;
    std::cout << "Using value size: " << value_size_kb << " KB (" << value_size_bytes << " bytes)" << std::endl;

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

    // --- BENCHMARK EXECUTION ---
    std::cout << "Starting benchmark. Running until " << READ_TARGET << " reads or " << WRITE_TARGET << " writes occur..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();

    std::thread reader_thread(reader_task, buffer_size);
    std::thread writer_thread(writer_task, buffer_size, value_size_bytes);

    reader_thread.join();
    writer_thread.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end_time - start_time;

    // --- RESULTS ---
    std::cout << "\n--- Benchmark Finished ---\n";
    std::cout << "Total duration: " << duration.count() << " seconds" << std::endl;
    
    std::cout << "Successful reads:  " << successful_reads << std::endl;
    std::cout << "Successful writes: " << successful_writes << std::endl;
    
    long long difference = successful_reads - successful_writes;
    std::cout << "Difference (#Reads - #Writes): " << difference << std::endl;
    
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
