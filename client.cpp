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
#include <vector>
#include <algorithm>
#include <cctype>

struct Message {
    int status;
    int client_id;
    char data[256];
};

const char* SERVER_FILE_PREFIX = "ipc_server_";
std::atomic<bool> running{true};
int currentClientId = 0;

void signalHandler(int signum) {
    std::cout << "\nClient: Shutting down..." << std::endl;
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

bool writeMessage(int fd, Message& msg) {
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

// Function to find all server files
std::vector<std::string> findServerFiles() {
    std::vector<std::string> serverFiles;
    
#if !PLATFORM_WINDOWS
    DIR* dir = opendir(".");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            if (filename.find(SERVER_FILE_PREFIX) == 0 && 
                filename.find(".bin") != std::string::npos) {
                serverFiles.push_back(filename);
            }
        }
        closedir(dir);
    }
#else
    WIN32_FIND_DATA findFileData;
    HANDLE hFind = FindFirstFile("*", &findFileData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::string filename = findFileData.cFileName;
            if (filename.find(SERVER_FILE_PREFIX) == 0 && 
                filename.find(".bin") != std::string::npos) {
                serverFiles.push_back(filename);
            }
        } while (FindNextFile(hFind, &findFileData) != 0);
        FindClose(hFind);
    }
#endif
    
    // Sort by number in filename
    std::sort(serverFiles.begin(), serverFiles.end(), [](const std::string& a, const std::string& b) {
        std::string numStrA = a.substr(strlen(SERVER_FILE_PREFIX));
        std::string numStrB = b.substr(strlen(SERVER_FILE_PREFIX));
        numStrA = numStrA.substr(0, numStrA.find('.'));
        numStrB = numStrB.substr(0, numStrB.find('.'));
        
        try {
            int numA = std::stoi(numStrA);
            int numB = std::stoi(numStrB);
            return numA > numB; // Sort descending (newer first)
        } catch (...) {
            return a < b;
        }
    });
    
    return serverFiles;
}

// Function to check server availability
bool checkServerAvailability(const std::string& filename) {
    int fd = open(filename.c_str(), O_RDWR);
    if (fd == -1) {
        return false;
    }
    
    Message msg{};
    bool available = false;
    
    if (readMessage(fd, msg)) {
        available = (msg.status == 0 || msg.status == 2);
    }
    
    close(fd);
    return available;
}

// Function for automatic connection
std::string autoConnectToServer() {
    std::vector<std::string> serverFiles = findServerFiles();
    std::vector<std::string> availableServers;
    
    for (const auto& file : serverFiles) {
        if (checkServerAvailability(file)) {
            availableServers.push_back(file);
        }
    }
    
    if (availableServers.empty()) {
        std::cout << "No servers available." << std::endl;
        return "";
    }
    
    // If only one server - connect to it
    if (availableServers.size() == 1) {
        return availableServers[0];
    }
    
    // If multiple servers - connect to the latest one (with highest number)
    return availableServers[0]; // Already sorted descending
}

// Function to check connection
bool isConnectedToServer(const std::string& filename) {
    int fd = open(filename.c_str(), O_RDWR);
    if (fd == -1) {
        return false;
    }
    
    Message msg{};
    bool connected = false;
    
    if (readMessage(fd, msg) && msg.status == 0) {
        Message testMsg{};
        testMsg.status = 1;
        testMsg.client_id = currentClientId;
        std::strcpy(testMsg.data, "ping");
        
        if (writeMessage(fd, testMsg)) {
            auto start = std::chrono::steady_clock::now();
            
            while (running) {
                if (!readMessage(fd, msg)) {
                    break;
                }
                
                if (msg.status == 2) {
                    connected = true;
                    break;
                }
                
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > 500) {
                    break;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            
            if (connected) {
                msg.status = 0;
                msg.client_id = currentClientId;
                std::memset(msg.data, 0, sizeof(msg.data));
                writeMessage(fd, msg);
            }
        }
    }
    
    close(fd);
    return connected;
}

// Function to display status
void showConnectionStatus(const std::string& currentFile) {
    if (currentFile.empty()) {
        std::cout << "Not connected to any server." << std::endl;
    } else {
        std::cout << "Connected to: " << currentFile << std::endl;
        std::cout << "Client ID: " << (currentClientId > 0 ? std::to_string(currentClientId) : "not assigned") << std::endl;
        
        if (!isConnectedToServer(currentFile)) {
            std::cout << "NO CONNECTED" << std::endl;
        }
    }
}

std::string getInputFromUser(const std::string& currentFile) {
    std::string input;
    while (true) {
        std::cout << "\nEnter command: ";
        
        std::getline(std::cin, input);
        
        if (input.empty()) {
            std::cout << "Enter not empty command";
            continue;
        }
        
        std::string lowerInput = input;
        std::transform(lowerInput.begin(), lowerInput.end(), lowerInput.begin(),
                      [](unsigned char c){ return std::tolower(c); });
        
        if (lowerInput == "exit") {
            running = false;
            return "";
        }
        
        if (lowerInput == "status") {
            showConnectionStatus(currentFile);
            continue;
        }
        
        if (lowerInput == "connect") {
            return "CONNECT";
        }
        
        if (lowerInput == "disconnect") {
            return "DISCONNECT";
        }
        
        if (lowerInput != "ping") {
            std::cout << "Error: Only 'ping' is accepted." << std::endl;
            continue;
        }
        
        if (currentFile.empty()) {
            std::cout << "Error: Not connected to server." << std::endl;
            continue;
        }
        
        return input;
    }
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    std::string currentFile;
    int fd = -1;
    
    // Automatic connection on startup
    currentFile = autoConnectToServer();
    
    if (!currentFile.empty()) {
        fd = open(currentFile.c_str(), O_RDWR);
        if (fd != -1) {
            std::cout << "Connected to: " << currentFile << std::endl;
        } else {
            std::cout << "Failed to connect." << std::endl;
            currentFile = "";
        }
    }
    
    while (running) {
        std::string command = getInputFromUser(currentFile);
        
        if (!running) break;
        
        if (command == "CONNECT") {
            if (fd != -1) {
                close(fd);
                fd = -1;
            }
            
            std::string newFile = autoConnectToServer();
            if (!newFile.empty()) {
                fd = open(newFile.c_str(), O_RDWR);
                if (fd != -1) {
                    currentFile = newFile;
                    currentClientId = 0;
                    std::cout << "Connected to: " << currentFile << std::endl;
                } else {
                    std::cout << "Failed to connect." << std::endl;
                    currentFile = "";
                    currentClientId = 0;
                }
            }
            continue;
        }
        
        if (command == "DISCONNECT") {
            if (fd != -1) {
                close(fd);
                fd = -1;
                currentClientId = 0;
                std::cout << "Disconnected." << std::endl;
            }
            currentFile = "";
            continue;
        }
        
        if (command.empty()) {
            continue;
        }
        
        // Sending ping
        Message msg{};
        int waitAttempts = 0;
        const int MAX_WAIT_ATTEMPTS = 5;
        
        // Wait until server is free
        while (running && waitAttempts < MAX_WAIT_ATTEMPTS) {
            if (!readMessage(fd, msg)) {
                break;
            }
            
            if (msg.status == 0) {
                break;
            }
            
            waitAttempts++;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (waitAttempts >= MAX_WAIT_ATTEMPTS) {
            std::cout << "Server is busy." << std::endl;
            continue;
        }
        
        // Send message
        std::memset(&msg, 0, sizeof(Message));
        msg.status = 1;
        msg.client_id = currentClientId;
        std::strncpy(msg.data, command.c_str(), sizeof(msg.data) - 1);
        msg.data[sizeof(msg.data) - 1] = '\0';
        
        if (!writeMessage(fd, msg)) {
            std::cout << "Failed to send ping." << std::endl;
            continue;
        }
        
        // Wait for response
        bool timeout = false;
        bool gotResponse = false;
        auto start = std::chrono::steady_clock::now();
        
        while (running && !timeout && !gotResponse) {
            if (!readMessage(fd, msg)) {
                break;
            }
            
            if (msg.status == 2) {
                gotResponse = true;
                
                if (msg.client_id > 0 && currentClientId == 0) {
                    currentClientId = msg.client_id;
                    std::cout << "Server assigned Client ID: " << currentClientId << std::endl;
                }
                break;
            }
            
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > 5000) {
                timeout = true;
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (timeout) {
            std::cout << "Timeout waiting for response." << std::endl;
            
            // Reset status
            msg.status = 0;
            msg.client_id = currentClientId;
            std::memset(msg.data, 0, sizeof(msg.data));
            writeMessage(fd, msg);
            continue;
        }
        
        if (gotResponse && msg.data[0] != '\0') {
            std::cout << "Response: " << msg.data << std::endl;
        }
        
        // Free the server
        msg.status = 0;
        msg.client_id = currentClientId;
        std::memset(msg.data, 0, sizeof(msg.data));
        writeMessage(fd, msg);
    }
    
    
    if (fd != -1) {
        Message resetMsg{};
        resetMsg.status = 0;
        resetMsg.client_id = currentClientId;
        std::memset(resetMsg.data, 0, sizeof(resetMsg.data));
        writeMessage(fd, resetMsg);
        close(fd);
    }
    
    std::cout << "Client stopped." << std::endl;
    
    return 0;
}

