// moth-helper.exe <port> <token>: connects to 127.0.0.1:<port>, says hello, then answers one
// JSON line at a time until the socket closes or the controller says bye.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "nlohmann/json.hpp"
#include "proto/handler.hpp"
#include "win/win_memory.hpp"

namespace {

bool send_all(SOCKET s, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    int n = send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: moth-helper <port> <token>\n");
    return 2;
  }
  int port = std::atoi(argv[1]);
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return 2;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    std::fprintf(stderr, "moth-helper: cannot connect to 127.0.0.1:%d\n", port);
    return 2;
  }
  BOOL on = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);

  nlohmann::json hello = {{"hello",
                           {{"token", argv[2]}, {"version", 1}, {"pid", GetCurrentProcessId()}, {"arch", "x86_64"}}}};
  if (!send_all(s, hello.dump() + "\n")) return 2;

  auto platform = moth::make_win_platform();
  moth::Handler handler(*platform, moth::EngineConfig{.workers = 0});

  std::string buf;
  char chunk[65536];
  bool done = false;
  while (!done) {
    int n = recv(s, chunk, sizeof chunk, 0);
    if (n <= 0) break;
    buf.append(chunk, static_cast<size_t>(n));
    size_t nl;
    while (!done && (nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      std::string reply = handler.handle_line(line);
      reply.push_back('\n');
      if (!send_all(s, reply)) {
        done = true;
        break;
      }
      if (handler.wants_exit()) done = true;
    }
  }
  closesocket(s);
  WSACleanup();
  return 0;
}
