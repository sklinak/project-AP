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
#include <ctime>

struct Message {
    int status;
    char data[256];
};

const char* FILE_NAME = "ipc.bin";
std::atomic<bool> running{true};

void logEvent(const std::string& event) {
    std::time_t now = std::time(nullptr);
    char timeStr[100];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    
    std::cout << "[" << timeStr << "] " << event << std::endl;
}

void signalHandler(int signum) {
    logEvent("Server: Shutting down...");    
    running = false;
}

bool readMessage(int fd, Message& msg) {
    if (lseek(fd, 0, SEEK_SET) == -1) {
        std::cerr << "Server: lseek error: " << strerror(errno) << std::endl;
        return false;
    }
    
    int r = read(fd, &msg, sizeof(Message));
    if (r == -1) {
        std::cerr << "Server: read error: " << strerror(errno) << std::endl;
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
        std::cerr << "Server: lseek error: " << strerror(errno) << std::endl;
        return false;
    }
    
    int w = write(fd, &msg, sizeof(Message));
    if (w == -1) {
        std::cerr << "Server: write error: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Synchonization on disk
    if (fsync(fd) == -1) {
        std::cerr << "Server: fsync error: " << strerror(errno) << std::endl;
    }
    
    return (w == sizeof(Message));
}

bool isValidRequest(const char* text) {
    if (text == nullptr) return false;
    
    if (text[0] == '\0') return false;
    
    // Chech for minimum length (after deleting prefix)
    if (strlen(text) < 4) return false;
    
    return true;
}

// Function for getting number of request from message
int extractRequestNumber(const char* request) {
    if (request == nullptr ||  request[0] != '[') return -1;
    
    try {
        return std::stoi(request + 1);
    } catch (...) {
        return -1;
    }
}

// Function for getting text out of request (without number)
std::string extractRequestText(const char* request) {
    if (request == nullptr) return "";
    
    const char* bracket = strchr(request, ']');
    if (bracket == nullptr || bracket[1] != ' ') return "";
    
    return std::string(bracket + 2);
}
// Proccesing request with possible errors
std::string processRequest(const char* request, bool& error) {
    error = false;
    
    int reqNumber = extractRequestNumber(request);
    std::string reqText = extractRequestText(request);
    
    if (reqNumber == -1 || reqText.empty()) {
        error = true;
        return "ERROR: Invalid request format";
    }
    
    // Imitation of different types of responses
    std::string response;
    
    // Special commands for testing errors
    if (reqText == "error") {
        error = true;
        return "ERROR: Simulated processing error";
    }
    
    if (reqText == "timeout") {
        // Imitation of long answer
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return "Response after timeout simulation";
    }
    
    if (reqText == "crash") {
        // Imitation of crash of server
        std::cerr << "Server: Simulating crash...\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // Do not give response - client will receive timeout
        std::this_thread::sleep_for(std::chrono::seconds(20));
        return "This should not be reached";
    }
    
    if (reqText == "invalid") {
        return "";
    }
    
    std::time_t now = std::time(nullptr);
    char timeStr[100];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    
    response = "OK";

    if (reqText.length() > 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    return response;
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    int fd = open(FILE_NAME, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        std::cerr << "Server: Failed to open IPC file: " << strerror(errno) << std::endl;
        return 1;
    }
    
    // Check and initialisation of file
    long size = lseek(fd, 0, SEEK_END);
    if (size < (long)sizeof(Message)) {
        logEvent("Server: Initializing IPC file...");
        Message init{};
        init.status = 0;
        std::memset(init.data, 0, sizeof(init.data));
        if (!writeMessage(fd, init)) {
            std::cerr << "Server: Failed to initialize IPC file\n";
            close(fd);
            return 1;
        }
    }
    
    logEvent("Server started.");    
    
    int processedRequests = 0;
    int errorCount = 0;
    
    while (running) {
        Message msg{};
        
        // Waiting for request from client
        while (running) {
            if (!readMessage(fd, msg)) {
                std::cerr << "Server: Error reading from IPC file\n";
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            
            if (msg.status == 1) {
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (!running) break;
        
        processedRequests++;
        
        // Check if request is valid
        if (!isValidRequest(msg.data)) {
            errorCount++;
            logEvent("Server: Received invalid request: \"" + std::string(msg.data) + "\"");
            
            msg.status = 2;
            std::strncpy(msg.data, "ERROR: Invalid request format", sizeof(msg.data) - 1);
            msg.data[sizeof(msg.data) - 1] = '\0';
            
            if (!writeMessage(fd, msg)) {
                std::cerr << "Server: Failed to send error response\n";
            }
            
            continue;
        }
        
        std::string requestStr(msg.data);
        logEvent("Server: Received request #" + std::to_string(processedRequests) + 
                ": \"" + extractRequestText(msg.data) + "\"");
        
        // Proccesing request
        bool processingError = false;
        std::string response = processRequest(msg.data, processingError);
        
        if (processingError) {
            errorCount++;
            logEvent("Server: Error processing request #" + std::to_string(processedRequests));
        }
   
        msg.status = 2;
        std::strncpy(msg.data, response.c_str(), sizeof(msg.data) - 1);
        msg.data[sizeof(msg.data) - 1] = '\0';
        
        if (!writeMessage(fd, msg)) {
            std::cerr << "Server: Failed to send response\n";
            errorCount++;
            continue;
        }
        
        logEvent("Server: Sent response for request #" + std::to_string(processedRequests));
    }
    
    // Graceful shutdown
    logEvent("Server: Statistics - Total processed: " + std::to_string(processedRequests) + 
            ", Total errors: " + std::to_string(errorCount));
    
    // Setting "unavaible" status before shutting down
    Message shutdownMsg{};
    shutdownMsg.status = 0;
    std::strncpy(shutdownMsg.data, "SERVER_SHUTDOWN", sizeof(shutdownMsg.data) - 1);
shutdownMsg.data[sizeof(shutdownMsg.data) - 1] = '\0';
    writeMessage(fd, shutdownMsg);
    
    close(fd);
    logEvent("Server: Stopped gracefully");
    
    return 0;
}
