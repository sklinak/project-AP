Sure — here is a clean, polished **README.md in English** for your project.

---

# Simple File-Based IPC (Client–Server)

This project implements a minimal yet reliable **inter-process communication (IPC)** system using a **shared binary file**.
The system consists of two applications:

* **Server** — reads client requests, processes them, and sends responses.
* **Client** — sends requests, waits for the server’s reply, and displays the result.

The communication is done through a single file `ipc.bin`, which stores a fixed-size `Message` structure.

---

## 📌 Features

* IPC using a regular file (no sockets, no message queues, no pipes).
* Full request–response lifecycle with proper synchronization.
* Robust validation:

  * prevention of invalid or empty input,
  * protection from server hangs,
  * controlled timeouts,
  * safe shutdown for both sides.
* Timestamped logging on the server side.
* Debug commands to simulate errors: `error`, `invalid`, `timeout`, `crash`.

---

## 🧱 Communication Protocol

The shared file stores the following structure:

```cpp
struct Message {
    int status;     // 0 = free, 1 = request, 2 = response
    char data[256]; // payload
};
```

### Status values

| Status | Meaning                           |
| ------ | --------------------------------- |
| `0`    | Server is free, no active message |
| `1`    | Client has written a request      |
| `2`    | Server has written a response     |

### Client workflow

1. Waits for the server to be free (`status = 0`).
2. Writes a request (`status = 1`).
3. Waits for server response (`status = 2`).
4. Reads the response.
5. Frees the server (`status = 0`).

### Server workflow

1. Waits for a client request.
2. Validates the request format.
3. Processes it.
4. Writes a response back to the file.

---

## 🚀 How to Run

### 🔧 Compile

```bash
g++ -std=c++17 server.cpp -o server
g++ -std=c++17 client.cpp -o client
```

### ▶️ Start the server

```bash
./server
```

### ▶️ Start the client (in another terminal)

```bash
./client
```

---

## 💬 Client Commands

The client accepts arbitrary text requests.
Additionally, it supports special commands:

| Command  | Description        |
| -------- | ------------------ |
| `exit`   | Exit client        |
| `status` | Show client status |

---

## 🧪 Testing & Error Simulation

These test inputs trigger different behaviors on the server:

| Input          | Server behavior                              |
| -------------- | -------------------------------------------- |
| `error`        | Returns a simulated processing error         |
| `timeout`      | Delays the response by 3 seconds             |
| `crash`        | Simulates a server freeze (client times out) |
| `invalid`      | Returns an empty response                    |
| Any other text | Responds with `"OK"`                         |

---

## 📄 Example Exchange

**Client → Server:**

```
[3] hello
```

**Server → Client:**

```
OK
```

---

## 📚 Server Logic Overview

The server:

* validates request format (`[N] text`),
* logs events with timestamps,
* processes input in `processRequest()`,
* simulates errors when requested,
* performs graceful shutdown on SIGINT/SIGTERM.

---

## 🧹 Graceful Shutdown Behavior

On shutdown:

* The server writes:
  `status = 0`, `data = "SERVER_SHUTDOWN"`
* The client resets the file and closes the descriptor.

This prevents the system from staying in a locked/busy state after unexpected exits.

---

## 📦 Project Structure

```
/project
 ├── client.cpp
 ├── server.cpp
 ├── README.md
 └── ipc.bin (generated automatically)
```




