#include <iostream>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

struct Message {
    int status;
    char data[256];
};

const char* FILE_NAME = "ipc.bin";
std::atomic<bool> running{true};

// Handler for graceful shutdown
void signalHandler(int signum) {
    std::cout << "\nClient: Shutting down...\n";
    running = false;
}

bool readMessage(int fd, Message& msg) {
    if (lseek(fd, 0, SEEK_SET) == -1) {
        std::cerr << "Client: lseek error: " << strerror(errno) << std::endl;
        return false;
    }
    
    int r = read(fd, &msg, sizeof(Message));
    if (r == -1) {
        std::cerr << "Client: read error: " << strerror(errno) << std::endl;
        return false;
    }
    
    if (r == 0) {
        std::memset(&msg, 0, sizeof(Message));
        return true;
    }
    
    return (r == sizeof(Message));
}

bool writeMessage(int fd, const Message& msg) {
    if (lseek(fd, 0, SEEK_SET) == -1) {
        std::cerr << "Client: lseek error: " << strerror(errno) << std::endl;
        return false;
    }
    
    int w = write(fd, &msg, sizeof(Message));
    if (w == -1) {
        std::cerr << "Client: write error: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Synchronization on disk
    if (fsync(fd) == -1) {
        std::cerr << "Client: fsync error: " << strerror(errno) << std::endl;
    }
    
    return (w == sizeof(Message));
}

bool isValidResponse(const char* text) {
    return text != nullptr && text[0] != '\0';
}

std::string getInputFromUser() {
    std::string input;
    while (true) {
        std::cout << "\nEnter your request (or 'exit' to quit, 'status' to check): ";
        std::getline(std::cin, input);
        
        // Проверка на пустой ввод
        if (input.empty()) {
            std::cout << "Error: Empty input is not allowed. Please enter a valid request.\n";
            continue;
        }
        
        // Проверка на слишком длинный ввод
        if (input.length() >= 255) {
            std::cout << "Error: Input is too long (max 254 characters). Please try again.\n";
            continue;
        }
        
        // Специальные команды
        if (input == "exit") {
            running = false;
            return "";
        }
        
        if (input == "status") {
            std::cout << "Client is running. Use 'exit' to quit.\n";
            continue;
        }
        
        // Проверка на наличие непечатаемых символов
        bool hasInvalidChars = false;
        for (char c : input) {
            if (c < 32 && c != '\t' && c != '\n' && c != '\r') {
                hasInvalidChars = true;
                break;
            }
        }
        
        if (hasInvalidChars) {
            std::cout << "Error: Input contains invalid characters. Please use only printable characters.\n";
            continue;
        }
        
        return input;
    }
}
int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    int fd = open(FILE_NAME, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        std::cerr << "Client: Failed to open IPC file: " << strerror(errno) << std::endl;
        return 1;
    }
    
    std::cout << "Client started.\n";
    
    int requestCounter = 0;
    
    while (running) {
        // Step 1: Waiting, until server is free (status == 0)
        Message msg{};
        int waitAttempts = 0;
        const int MAX_WAIT_ATTEMPTS = 50; // 50 * 100ms = 5 seconds
        
        while (running) {
            if (!readMessage(fd, msg)) {
                std::cerr << "Client: Failed to read message\n";
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            
            if (msg.status == 0) {
                break;
            }
            
            if (++waitAttempts > MAX_WAIT_ATTEMPTS) {
                std::cerr << "Client: Server is busy for too long. Please try again later.\n";
                
                // Ask user for action
                std::cout << "Press Enter to retry or type 'exit' to quit: ";
                std::string response;
                std::getline(std::cin, response);
                
                if (response == "exit") {
                    running = false;
                    break;
                }
                
                waitAttempts = 0;
                continue;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (!running) break;
        
        // Step 2: Get input from user
        std::string userInput = getInputFromUser();
        if (!running) break;
        
        // Step 3: Prepare and send message
        requestCounter++;
        std::string fullRequest = "[" + std::to_string(requestCounter) + "] " + userInput;
        
        std::memset(&msg, 0, sizeof(Message));
        std::strncpy(msg.data, fullRequest.c_str(), sizeof(msg.data) - 1);
        msg.data[sizeof(msg.data) - 1] = '\0';
        msg.status = 1;
        
        std::cout << "Client: Sending request #" << requestCounter << ": \"" << userInput << "\"" << std::endl;
        
        if (!writeMessage(fd, msg)) {
            std::cerr << "Client: Failed to send request. Server might be unavailable.\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        
        // Step 4: Wait for servers answer
        std::cout << "Client: Waiting for server response...\n";
        
        bool timeout = false;
        bool gotResponse = false;
        auto start = std::chrono::steady_clock::now();
        const int TIMEOUT_MS = 10000; // 10 seconds
        
        while (running && !timeout && !gotResponse) {
            if (!readMessage(fd, msg)) {
                std::cerr << "Client: Error reading response\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            if (msg.status == 2) {
                gotResponse = true;
                break;
            }
            
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > TIMEOUT_MS) {
                timeout = true;
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (timeout) {
            std::cerr << "Client: Timeout waiting for server response\n";
            msg.status = 0;
            std::memset(msg.data, 0, sizeof(msg.data));
            writeMessage(fd, msg);
            continue;
        }
        
        if (!gotResponse) {
            // Client was stopped while waiting
            continue;
        }
        
        // Step 5: Checking and printing answer
        if (!isValidResponse(msg.data)) {
            std::cerr << "Client: Received empty or invalid response from server\n";
        } else {
            std::cout << "Client: Received response: \"" << msg.data << "\"" << std::endl;
        }
        
        // Step 6: Free server
        msg.status = 0;
        std::memset(msg.data, 0, sizeof(msg.data));
        if (!writeMessage(fd, msg)) {
            std::cerr << "Client: Failed to free server\n";
        }
        
        std::cout << "Client: Request completed successfully\n";
    }
    
    // Graceful shutdown
    std::cout << "\nClient: Cleaning up...\n";
    
    // Free server before exiting
    Message resetMsg{};
    resetMsg.status = 0;
    std::memset(resetMsg.data, 0, sizeof(resetMsg.data));
    writeMessage(fd, resetMsg);
    
    close(fd);
    std::cout << "Client: Stopped gracefully\n";
    
    return 0;
}
