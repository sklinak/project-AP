#ifdef _WIN32
#define PLATFORM_WINDOWS 1
#else
#define PLATFORM_WINDOWS 0
#endif

#if PLATFORM_WINDOWS
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#define open _open
#define read _read
#define write _write
#define close _close
#define lseek _lseek
#define fsync _commit
#define unlink _unlink
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <ctime>
#include <algorithm>
#include <vector>
#include <set>
#include <mutex>

struct Message {
    int status;
    int client_id;
    char data[256];
};

const char* SERVER_FILE_PREFIX = "ipc_server_";
std::atomic<bool> running{true};

// Global variables for the server
std::string currentFileName;
int serverInstanceNumber = 1;
std::atomic<int> clientCounter{0};
int nextClientId = 1;

std::mutex clientMutex;
std::set<int> connectedClients;

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
        return false;
    }
    
    int r = read(fd, &msg, sizeof(Message));
    if (r == -1) {
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
        return false;
    }
    
    int w = write(fd, &msg, sizeof(Message));
    if (w == -1) {
        return false;
    }
    
    fsync(fd);
    return (w == sizeof(Message));
}

bool isValidPingRequest(const char* text) {
    if (text == nullptr) return false;
    
    std::string request(text);
    
    // Remove leading and trailing whitespace
    size_t start = request.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return false;
    
    size_t end = request.find_last_not_of(" \t\n\r");
    request = request.substr(start, end - start + 1);
    
    std::string lowerRequest = request;
    std::transform(lowerRequest.begin(), lowerRequest.end(), lowerRequest.begin(),
                  [](unsigned char c){ return std::tolower(c); });
    
    return lowerRequest == "ping";
}

// Function to find the maximum number of an existing server
int findMaxServerNumber() {
    int maxNumber = 0;
    
#if !PLATFORM_WINDOWS
    DIR* dir = opendir(".");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            if (filename.find(SERVER_FILE_PREFIX) == 0) {
                // Extract the number from the filename
                std::string numStr = filename.substr(strlen(SERVER_FILE_PREFIX));
                size_t dotPos = numStr.find('.');
                if (dotPos != std::string::npos) {
                    numStr = numStr.substr(0, dotPos);
                    try {
                        int num = std::stoi(numStr);
                        if (num > maxNumber) {
                            maxNumber = num;
                        }
                    } catch (...) {
                        // Ignore files with incorrect names
                    }
                }
            }
        }
        closedir(dir);
    }
#else
    // For Windows
    WIN32_FIND_DATA findFileData;
    HANDLE hFind = FindFirstFile("*", &findFileData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::string filename = findFileData.cFileName;
            if (filename.find(SERVER_FILE_PREFIX) == 0) {
                std::string numStr = filename.substr(strlen(SERVER_FILE_PREFIX));
                size_t dotPos = numStr.find('.');
                if (dotPos != std::string::npos) {
                    numStr = numStr.substr(0, dotPos);
                    try {
                        int num = std::stoi(numStr);
                        if (num > maxNumber) {
                            maxNumber = num;
                        }
                    } catch (...) {
                        // Ignore files with incorrect names
                    }
                }
            }
        } while (FindNextFile(hFind, &findFileData) != 0);
        FindClose(hFind);
    }
#endif
    
    return maxNumber;
}

// Function to remove the server file on exit
void cleanupServerFile() {
    if (unlink(currentFileName.c_str()) == 0) {
        logEvent("Server: Removed IPC file: " + currentFileName);
    }
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Find the maximum number of an existing server
    int maxServerNumber = findMaxServerNumber();
    serverInstanceNumber = maxServerNumber + 1;
    
    // Create a filename with a sequential number
    currentFileName = std::string(SERVER_FILE_PREFIX) + std::to_string(serverInstanceNumber) + ".bin";
    logEvent("Server: Starting server #" + std::to_string(serverInstanceNumber) + 
            " with file: " + currentFileName);
    
    int fd = open(currentFileName.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        // If the file already exists, try to open it
        fd = open(currentFileName.c_str(), O_RDWR);
        if (fd == -1) {
            std::cerr << "Server: Failed to open IPC file: " << strerror(errno) << std::endl;
            return 1;
        }
        logEvent("Server: Using existing IPC file");
    }
    
    // Initialize the file
    long size = lseek(fd, 0, SEEK_END);
    if (size < (long)sizeof(Message)) {
        Message init{};
        init.status = 0;
        init.client_id = 0;
        std::memset(init.data, 0, sizeof(init.data));
        if (!writeMessage(fd, init)) {
            std::cerr << "Server: Failed to initialize IPC file" << std::endl;
            close(fd);
            return 1;
        }
    }
    
    logEvent("Server started.");
    
    while (running) {
        Message msg{};
        
        // Wait for a request from a client
        while (running) {
            if (!readMessage(fd, msg)) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            
            if (msg.status == 1) {
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (!running) break;
        
        // Check if the message is "ping"
        if (!isValidPingRequest(msg.data)) {
            std::string receivedMsg(msg.data);
            
            logEvent("Server: Invalid message from client #" + 
                    std::to_string(msg.client_id) + ": \"" + receivedMsg + "\"");
            
            msg.status = 2;
            msg.client_id = msg.client_id; // Preserve the client ID
            std::strncpy(msg.data, "ERROR: Only 'ping' is accepted", sizeof(msg.data) - 1);
            msg.data[sizeof(msg.data) - 1] = '\0';
            
            writeMessage(fd, msg);
            continue;
        }
        
        // Assign an ID to the client if it's a new connection
        bool isNewClient = false;
        {
            std::lock_guard<std::mutex> lock(clientMutex);
            
            // Check if this is a new client (client_id == 0)
            if (msg.client_id == 0) {
                // Assign a new ID
                msg.client_id = nextClientId++;
                connectedClients.insert(msg.client_id);
                clientCounter = connectedClients.size();
                isNewClient = true;
            }
            // If client already has an ID but not in our set, add it
            else if (msg.client_id > 0 && connectedClients.find(msg.client_id) == connectedClients.end()) {
                connectedClients.insert(msg.client_id);
                clientCounter = connectedClients.size();
                isNewClient = true;
            }
        }
        
        if (isNewClient) {
            logEvent("Server: Client #" + std::to_string(msg.client_id) + 
                    " connected. Total connected clients: " + std::to_string(clientCounter.load()));
        }
        
        // Process the ping request
        logEvent("Server: Received 'ping' from client #" + std::to_string(msg.client_id));
        
        // Form a response
        std::string response = "pong from server #" + std::to_string(serverInstanceNumber) +
                              " to client #" + std::to_string(msg.client_id);
        
        msg.status = 2;
        std::strncpy(msg.data, response.c_str(), sizeof(msg.data) - 1);
        msg.data[sizeof(msg.data) - 1] = '\0';
        
        if (!writeMessage(fd, msg)) {
            continue;
        }
        
        logEvent("Server: Sent 'pong' to client #" + std::to_string(msg.client_id));
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Shutdown
    logEvent("Server: Shutting down...");
    
    Message shutdownMsg{};
    shutdownMsg.status = 0;
    shutdownMsg.client_id = 0;
    std::strncpy(shutdownMsg.data, "SERVER_SHUTDOWN", sizeof(shutdownMsg.data) - 1);
    shutdownMsg.data[sizeof(shutdownMsg.data) - 1] = '\0';
    writeMessage(fd, shutdownMsg);
    
    close(fd);
    cleanupServerFile();
    
    logEvent("Server #" + std::to_string(serverInstanceNumber) + " stopped");
    logEvent("Total unique clients served: " + std::to_string(clientCounter.load()));
    
    return 0;
}
