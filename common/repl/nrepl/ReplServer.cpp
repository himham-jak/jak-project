// clang-format off
#include "ReplServer.h"

#include "common/cross_sockets/XSocket.h"
#include "common/versions/versions.h"

#include "fmt/format.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#endif
#include "common/log/log.h"
// clang-format on

// TODO - The server also needs to eventually return the result of the evaluation

ReplServer::~ReplServer() {
  // Close all our client sockets!
  for (const int& sock : client_sockets) {
    close_socket(sock);
  }
}

void ReplServer::post_init() {
  // Add the listening socket to our set of sockets
  lg::debug("[nREPL:{}:{}] awaiting connections", tcp_port, listening_socket);
}

void ReplServer::error_response(int socket, const std::string& error) {
  std::string msg = fmt::format("[ERROR]: {}", error);
  auto resp = write_to_socket(socket, msg.c_str(), msg.size());
  if (resp == -1) {
    lg::warn("[nREPL:{}] Client Disconnected: {}", tcp_port, address_to_string(addr),
             ntohs(addr.sin_port), socket);
    close_socket(socket);
    client_sockets.erase(socket);
  }
}

void ReplServer::ping_response(int socket) {
  std::string ping = fmt::format("Connected to OpenGOAL v{}.{} nREPL!",
                                 versions::GOAL_VERSION_MAJOR, versions::GOAL_VERSION_MINOR);
  auto resp = write_to_socket(socket, ping.c_str(), ping.size());
  if (resp == -1) {
    lg::warn("[nREPL:{}] Client Disconnected: {}", tcp_port, address_to_string(addr),
             ntohs(addr.sin_port), socket);
    close_socket(socket);
    client_sockets.erase(socket);
  }
}


// Function to convert decimal
// to hexadecimal
std::string decToHexa(int n)
{
    // char array to store hexadecimal number
    char hexaDeciNum[100];

    // Counter for hexadecimal number array
    int i = 0;
    while (n != 0) {
        // Temporary variable to store remainder
        int temp = 0;

        // Storing remainder in temp variable.
        temp = n % 16;

        // Check if temp < 10
        if (temp < 10) {
            hexaDeciNum[i] = temp + 48;
            i++;
        }
        else {
            hexaDeciNum[i] = temp + 55;
            i++;
        }

        n = n / 16;
    }

    std::string result;

    // Printing hexadecimal number
    // array in reverse order
    for (int j = i - 1; j >= 0; j--)
        result += hexaDeciNum[j];

    return result;
}

std::string str_to_chunk(const std::string& data) {
  //lg::info("Datasize d {}",data.size());
  //lg::info("Datasize h {}",decToHexa(data.size()).c_str());

    // Format chunk: <hex-size>\r\n<data>\r\n
    std::string chunk = decToHexa(data.size()) + "\r\n" + data + "\r\n";
    return chunk;
}

int send_str_chunk(int socket, std::string& str_chunk) {
  std::string chunk = str_to_chunk(str_chunk);
  return write_to_socket(socket, chunk.c_str(), chunk.size());
}

// new float response to get request
void ReplServer::float_response(int socket) {
  //http header
  std::string header =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Transfer-Encoding: chunked\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

  // http footer
  std::string end = "0\r\n\r\n";

  //start http message
  write_to_socket(socket, header.c_str(), header.size());

  // padding to accomodate for deci2 header (32 bytes)
  std::string pad = "................................";

  // float in string
  std::string flt = pad + "1.1\nabcdefg";

  // send float in string
  send_str_chunk(socket,flt);

  //end http message
  write_to_socket(socket, end.c_str(), end.size());
}

// new html response to get request
void ReplServer::html_response(int socket) {
  //http header
  std::string header =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Transfer-Encoding: chunked\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

  // http footer
  std::string end = "0\r\n\r\n";

  // open html file
  lg::info("Opening html file now");
  std::string html_path = "default.html";
  std::ifstream file(html_path);

  // if fail to open
  if (!file.is_open()) {
        // error
        lg::error("Failed to open file");
  }

  //start http message
  write_to_socket(socket, header.c_str(), header.size());

  // Read the file line by line into a string
  std::string line;
  while (getline(file, line)) {
        // send each line as an http chunk
        send_str_chunk(socket,line);
    }

  // Close the file
  lg::info("Closing html file now");
  file.close();

  //end http message
  write_to_socket(socket, end.c_str(), end.size());
}


void print_http_request(const void* buffer, int size) {
    const u8* data = reinterpret_cast<const u8*>(buffer);
    const int chunk_size = 8;  // bytes received per call
    static u8 line_buf[16];
    static int line_len = 0;
    static int total_offset = 0;

    for (int i = 0; i < size; i += chunk_size) {
        int len = std::min(chunk_size, size - i);
        // Copy incoming chunk into line_buf
        memcpy(line_buf + line_len, data + i, len);
        line_len += len;

        // If we've accumulated 16 bytes, print the full line
        if (line_len >= 16) {
            // Print offset
            fprintf(stderr, "%08X  ", total_offset);

            // Print hex for full 16 bytes
            for (int j = 0; j < 16; ++j) {
                fprintf(stderr, "%02X ", line_buf[j]);
            }
            fprintf(stderr, " ");

            // Print ASCII for full 16 bytes
            for (int j = 0; j < 16; ++j) {
                u8 c = line_buf[j];
                fprintf(stderr, "%c", (c >= 32 && c <= 126) ? c : '.');
            }
            fprintf(stderr, "\n");

            // Update offset, reset buffer
            total_offset += 16;
            line_len = 0;
        }
    }
}



std::optional<std::string> ReplServer::get_msg() {
  // Clear the sockets we are listening on
  FD_ZERO(&read_sockets);

  // Add the server's main listening socket (where we accept clients from)
  FD_SET(listening_socket, &read_sockets);
  int max_sd = listening_socket;
  for (const int& sock : client_sockets) {
    if (sock > max_sd) {
      max_sd = sock;
    }
    if (sock > 0) {
      FD_SET(sock, &read_sockets);
    }
  }

  // Wait for activity on _something_, with a timeout so we don't get stuck here on exit.
  struct timeval timeout = {0, 100000};
  auto activity = select(max_sd + 1, &read_sockets, NULL, NULL, &timeout);
  if (activity < 0 && errno != EINTR) {
    lg::error("[nREPL:{}] select error, returned: {}, errno: {}", tcp_port, activity,
              strerror(errno));
    return std::nullopt;
  }

  // If something happened on the master socket - it's a new connection
  if (FD_ISSET(listening_socket, &read_sockets)) {
    socklen_t addr_len = sizeof(addr);
    auto new_socket = accept_socket(listening_socket, (sockaddr*)&addr, &addr_len);
    if (new_socket < 0) {
      if (new_socket != -1) {
        lg::error("[nREPL:{}] accept error, returned: {}, errono: {}", tcp_port, new_socket,
                  strerror(errno));
      }
    } else {
      lg::info("[nREPL:{}]: New socket connection: {}:{}:{}", tcp_port, address_to_string(addr),
               ntohs(addr.sin_port), new_socket);
      // Say hello
      //ping_response(new_socket);
      // Track the new socket
      if ((int)client_sockets.size() < max_clients) {
        client_sockets.insert(new_socket);
      } else {
        // Respond with NO and close the socket
        lg::warn("[nREPL:{}]: Maximum clients reached. Rejecting connection.", tcp_port);
        error_response(new_socket, "Maximum clients reached. Rejecting connection.");
        close_socket(new_socket);
      }
    }
  }

  // Check all clients for activity
  for (auto it = client_sockets.begin(); it != client_sockets.end();) {
    int sock = *it;
    if (FD_ISSET(sock, &read_sockets)) {
      // Attempt to read a header
      auto req_bytes = read_from_socket(sock, header_buffer.data(), header_buffer.size());

      // print the http request
      print_http_request(header_buffer.data(), header_buffer.size());

      // handle http get requests
      if (std::strncmp(header_buffer.data(), "GET", 3) == 0) {
        lg::info("GET REQUEST: {}", header_buffer.data());

        // Find the path after "GET "
        const char* path_start = header_buffer.data() + 4;
        const char* path_end = path_start + 4;

        std::string path(path_start, path_end);
        //lg::info("Path {}", path);

        if (path_end) {

          if (path == "/flo") {
            lg::info("Serving float response");
            float_response(sock);
          } else {
            lg::info("Serving html response");
            html_response(sock);
          }



        // print the http request
        print_http_request(header_buffer.data(), header_buffer.size());
        } else {
          lg::error("Malformed GET request (no path found)");
        }
      }



      if (req_bytes <= 0) {
        // TODO - add a queue of messages in the REPL::Wrapper so we can print _BEFORE_ the prompt
        // is output
        if (req_bytes == 0) {
          lg::warn("[nREPL:{}] Client Disconnected: {}", tcp_port, address_to_string(addr));
        } else {
          lg::warn("[nREPL:{}] Error reading from socket on {}: {}", tcp_port,
                   address_to_string(addr), strerror(errno));
        }
        // Cleanup the socket and remove it from our set
        close_socket(sock);
        it = client_sockets.erase(it);  // Erase and move to the next element
        continue;
      } else {
        // Otherwise, process the message
        auto* header = (ReplServerHeader*)(header_buffer.data());
        // get the body of the message
        int expected_size = header->length;
        int got = 0;
        int tries = 0;
        bool skip_to_next_socket = false;
        while (got < expected_size) {
          if (want_exit_callback()) {
            lg::warn("[nREPL:{}] Terminating nREPL early", tcp_port);
            return std::nullopt;
          }
          tries++;
          if (tries > 100) {
            break;
          }
          if (got + expected_size > (int)buffer.size()) {
            
            //lg::info("{}\n",header_buffer.data());
            //lg::error(
            //    "[nREPL:{}]: Bad message, aborting the read.  Got :{}, Expected: {}, Buffer "
            //    "Size: {}",
            //    tcp_port, got, expected_size, buffer.size());
            return std::nullopt;
          }
          auto bytes_read = read_from_socket(sock, buffer.data() + got, expected_size - got);
          if (bytes_read <= 0) {
            if (bytes_read == 0) {
              lg::warn("[nREPL:{}] Client Disconnected: {}", tcp_port, address_to_string(addr));
            } else {
              lg::warn("[nREPL:{}] Error reading from socket on {}: {}", tcp_port,
                       address_to_string(addr), strerror(errno));
            }
            close_socket(sock);
            it = client_sockets.erase(it);  // Erase and move to the next element
            skip_to_next_socket = true;
            break;
          }
          got += bytes_read;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (skip_to_next_socket) {
          continue;
        }

        switch (header->type) {
          case ReplServerMessageType::PING:
            ping_response(sock);
            return std::nullopt;
          case ReplServerMessageType::EVAL:
            std::string msg(buffer.data(), header->length);
            lg::debug("[nREPL:{}] Received Message: {}", tcp_port, msg);
            return std::make_optional(msg);
        }
      }
    }
    ++it;
  }
  return std::nullopt;
}
