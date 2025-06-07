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

// new http response
void ReplServer::http_response(int socket) {
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


      //if http, branch off
      
      // grab the packet bytes
      std::string test = fmt::format("{}", header_buffer.data());

      // if GET request
      if (std::strncmp(header_buffer.data(), "GET", 3) == 0) {
        lg::info("GET REQUEST: {}",test);
        http_response(sock);
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
