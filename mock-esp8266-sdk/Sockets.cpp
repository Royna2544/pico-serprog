// POSIX implementation of the ESP8266 Arduino 3.1.2 plain TCP/UDP APIs.
#include "ESP8266WiFi.h"
#include "MockNetwork.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <future>
#include <mutex>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Guard = std::lock_guard<std::recursive_mutex>;
std::atomic<bool> defaultNoDelay{false}, defaultSync{false};
std::atomic<uint16_t> localPortStart{0};
sockaddr_in endpoint(IPAddress ip, uint16_t port) {
  sockaddr_in out{};
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  out.sin_addr.s_addr = uint32_t(ip);
  return out;
}
bool nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
         fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}
int waitSocket(int fd, short events, Clock::time_point end) {
  for (;;) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                         end - Clock::now())
                         .count();
    pollfd item{fd, events, 0};
    int rc = poll(
        &item, 1,
        int(std::max<int64_t>(0, std::min<int64_t>(remaining, INT32_MAX))));
    if (rc < 0 && errno == EINTR) {
      if (Clock::now() >= end) {
        errno = ETIMEDOUT;
        return 0;
      }
      continue;
    }
    if (rc == 0)
      errno = ETIMEDOUT;
    return rc;
  }
}
bool resolve(const char *host, IPAddress &result, unsigned long timeout) {
  if (!host || !*host) {
    errno = EINVAL;
    return false;
  }
  if (result.fromString(host))
    return true;
  // A timed-out caller never leaves a worker holding references to its stack.
  auto answer = std::make_shared<std::promise<std::pair<int, IPAddress>>>();
  auto future = answer->get_future();
  try {
    std::thread([name = std::string(host), answer] {
      addrinfo hints{}, *addresses = nullptr;
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      int rc = getaddrinfo(name.c_str(), nullptr, &hints, &addresses);
      IPAddress ip;
      if (!rc && addresses)
        ip = IPAddress(reinterpret_cast<sockaddr_in *>(addresses->ai_addr)
                           ->sin_addr.s_addr);
      else if (!rc)
        rc = EAI_NONAME;
      if (addresses)
        freeaddrinfo(addresses);
      answer->set_value({rc, ip});
    }).detach();
  } catch (const std::system_error &) {
    errno = EAGAIN;
    return false;
  }
  if (future.wait_for(std::chrono::milliseconds(timeout)) !=
      std::future_status::ready) {
    errno = ETIMEDOUT;
    return false;
  }
  auto value = future.get();
  if (value.first) {
    errno = EHOSTUNREACH;
    return false;
  }
  result = value.second;
  return true;
}
template <class T> struct Registry {
  std::mutex mutex;
  std::vector<std::weak_ptr<T>> values;
  static Registry &get() {
    static Registry value;
    return value;
  }
  static void add(const std::shared_ptr<T> &p) {
    auto &r = get();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.values.erase(std::remove_if(r.values.begin(), r.values.end(),
                                  [](const auto &w) { return w.expired(); }),
                   r.values.end());
    r.values.emplace_back(p);
  }
  static void stopExcept(const std::shared_ptr<T> &except) {
    std::vector<std::shared_ptr<T>> live;
    {
      auto &r = get();
      std::lock_guard<std::mutex> lock(r.mutex);
      for (auto &w : r.values)
        if (auto p = w.lock())
          if (p != except)
            live.push_back(p);
    }
    for (auto &p : live)
      p->close();
  }
};
} // namespace

namespace esp_mock {
struct TcpState {
  std::recursive_mutex mutex;
  int fd = -1;
  bool eof = false, sync = defaultSync.load();
  std::vector<uint8_t> input;
  size_t position = 0;
  explicit TcpState(int socket) : fd(socket) {}
  ~TcpState() { close(); }
  void close() {
    Guard lock(mutex);
    if (fd >= 0)
      ::close(fd);
    fd = -1;
    eof = true;
    input.clear();
    position = 0;
  }
  size_t buffered() const { return input.size() - position; }
  void fill(size_t wanted = 1) {
    if (fd < 0 || eof || buffered() >= wanted)
      return;
    if (position) {
      input.erase(input.begin(), input.begin() + position);
      position = 0;
    }
    size_t previous = input.size();
    input.resize(previous + 16384);
    ssize_t n;
    do {
      n = recv(fd, input.data() + previous, 16384, MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
      input.resize(previous + size_t(n));
    else {
      input.resize(previous);
      if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        eof = true;
    }
  }
};
struct ServerState {
  int fd = -1;
  IPAddress address;
  uint16_t port;
  uint8_t backlog = 5;
  int noDelay = -1;
  std::deque<std::shared_ptr<TcpState>> pending;
  ServerState(IPAddress ip, uint16_t p) : address(ip), port(p) {}
  ~ServerState() { close(); }
  void close() {
    if (fd >= 0)
      ::close(fd);
    fd = -1;
    pending.clear();
  }
};
struct UdpState {
  std::recursive_mutex mutex;
  int fd = -1;
  sockaddr_in destination{}, remote{};
  IPAddress receivedOn;
  std::vector<uint8_t> tx, rx;
  size_t position = 0;
  bool building = false;
  explicit UdpState(int socket) : fd(socket) {}
  ~UdpState() { close(); }
  void close() {
    Guard lock(mutex);
    if (fd >= 0)
      ::close(fd);
    fd = -1;
    tx.clear();
    rx.clear();
    position = 0;
    building = false;
  }
};
} // namespace esp_mock
namespace {
std::shared_ptr<esp_mock::TcpState> tcpState(int fd) {
  if (!nonblocking(fd)) {
    int error = errno;
    ::close(fd);
    errno = error;
    return {};
  }
  int flag = defaultNoDelay.load();
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
  std::shared_ptr<esp_mock::TcpState> state;
  try {
    state = std::make_shared<esp_mock::TcpState>(fd);
  } catch (...) {
    ::close(fd);
    throw;
  }
  Registry<esp_mock::TcpState>::add(state);
  return state;
}
void fillPending(esp_mock::ServerState &server) {
  if (server.fd < 0)
    return;
  while (server.pending.size() < server.backlog) {
    int fd = ::accept(server.fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    auto state = tcpState(fd);
    if (!state)
      continue;
    if (server.noDelay >= 0)
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &server.noDelay,
                 sizeof(server.noDelay));
    server.pending.push_back(std::move(state));
  }
}
sockaddr_in socketAddress(int fd, bool remote) {
  sockaddr_in address{};
  socklen_t n = sizeof(address);
  if (fd >= 0) {
    if (remote)
      getpeername(fd, reinterpret_cast<sockaddr *>(&address), &n);
    else
      getsockname(fd, reinterpret_cast<sockaddr *>(&address), &n);
  }
  return address;
}
int tcpOption(const std::shared_ptr<esp_mock::TcpState> &state, int option,
              int level = IPPROTO_TCP) {
  if (!state)
    return 0;
  Guard lock(state->mutex);
  int value = 0;
  socklen_t n = sizeof(value);
  if (state->fd >= 0)
    getsockopt(state->fd, level, option, &value, &n);
  return value;
}
} // namespace

WiFiClient::WiFiClient() { _timeout = 5000; }
WiFiClient::WiFiClient(std::shared_ptr<esp_mock::TcpState> state)
    : state_(std::move(state)) {
  _timeout = 5000;
}
std::unique_ptr<WiFiClient> WiFiClient::clone() const {
  return std::make_unique<WiFiClient>(*this);
}
int ESP8266WiFiClass::hostByName(const char *host, IPAddress &ip,
                                 uint32_t timeout) {
  return resolve(host, ip, timeout);
}
int WiFiClient::connect(const char *host, uint16_t port) {
  IPAddress ip;
  if (!port) {
    errno = EINVAL;
    return 0;
  }
  auto start = Clock::now();
  if (!resolve(host, ip, _timeout))
    return 0;
  auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
                  Clock::now() - start)
                  .count();
  unsigned long saved = _timeout;
  _timeout = used < int64_t(saved) ? saved - unsigned(used) : 0;
  int rc = connect(ip, port);
  _timeout = saved;
  return rc;
}
int WiFiClient::connect(IPAddress ip, uint16_t port) {
  if (state_)
    stop();
  if (!port || !ip.isSet()) {
    errno = EINVAL;
    return 0;
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return 0;
  auto state = tcpState(fd);
  if (!state)
    return 0;
  uint16_t local = localPortStart.load();
  if (local) {
    local = localPortStart.fetch_add(1);
    auto source = endpoint(IPAddress(), local);
    if (bind(fd, reinterpret_cast<sockaddr *>(&source), sizeof(source)) < 0)
      return 0;
  }
  auto address = endpoint(ip, port);
  int rc =
      ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  if (rc < 0) {
    if (errno != EINPROGRESS ||
        waitSocket(fd, POLLOUT,
                   Clock::now() + std::chrono::milliseconds(_timeout)) <= 0)
      return 0;
    int error = 0;
    socklen_t n = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n) < 0)
      return 0;
    if (error) {
      errno = error;
      return 0;
    }
  }
  state_ = std::move(state);
  return 1;
}
int WiFiClient::available() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  state_->fill();
  int ready = 0;
  if (state_->fd >= 0)
    ioctl(state_->fd, FIONREAD, &ready);
  return int(std::min(size_t(INT32_MAX),
                      state_->buffered() + size_t(std::max(ready, 0))));
}
int WiFiClient::read() {
  uint8_t value;
  return read(&value, 1) == 1 ? value : -1;
}
int WiFiClient::read(uint8_t *data, size_t n) {
  if (!n)
    return 0;
  if (!data) {
    errno = EINVAL;
    return -1;
  }
  if (!state_)
    return -1;
  Guard lock(state_->mutex);
  state_->fill();
  size_t count = std::min(n, state_->buffered());
  if (!count)
    return 0;
  std::memcpy(data, state_->input.data() + state_->position, count);
  state_->position += count;
  return int(count);
}
int WiFiClient::peek() {
  if (!state_)
    return -1;
  Guard lock(state_->mutex);
  state_->fill();
  return state_->buffered() ? state_->input[state_->position] : -1;
}
size_t WiFiClient::peekBytes(uint8_t *data, size_t n) {
  if (!data && n) {
    errno = EINVAL;
    return 0;
  }
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  auto end = Clock::now() + std::chrono::milliseconds(_timeout);
  while (state_->fd >= 0 && !state_->eof && state_->buffered() < n) {
    size_t previous = state_->buffered();
    state_->fill(n);
    if (state_->buffered() >= n || state_->eof)
      break;
    if (state_->buffered() == previous &&
        waitSocket(state_->fd, POLLIN, end) <= 0)
      break;
    if (Clock::now() >= end)
      break;
  }
  size_t count = std::min(n, state_->buffered());
  if (count)
    std::memcpy(data, state_->input.data() + state_->position, count);
  return count;
}
size_t WiFiClient::peekAvailable() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  state_->fill();
  return state_->buffered();
}
const char *WiFiClient::peekBuffer() {
  if (!state_)
    return nullptr;
  Guard lock(state_->mutex);
  state_->fill();
  return state_->buffered() ? reinterpret_cast<const char *>(
                                  state_->input.data() + state_->position)
                            : nullptr;
}
void WiFiClient::peekConsume(size_t n) {
  if (!state_)
    return;
  Guard lock(state_->mutex);
  state_->position += std::min(n, state_->buffered());
}
uint8_t WiFiClient::connected() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  state_->fill();
  return state_->fd >= 0 && (!state_->eof || state_->buffered());
}
uint8_t WiFiClient::status() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  state_->fill();
  return state_->fd < 0 ? CLOSED : state_->eof ? CLOSE_WAIT : ESTABLISHED;
}
size_t WiFiClient::write(uint8_t value) { return write(&value, 1); }
size_t WiFiClient::write(const uint8_t *data, size_t n) {
  if (!n)
    return 0;
  if (!data || !state_) {
    errno = EINVAL;
    setWriteError();
    return 0;
  }
  Guard lock(state_->mutex);
  if (state_->fd < 0) {
    errno = EBADF;
    setWriteError();
    return 0;
  }
  auto deadline = Clock::now() + std::chrono::milliseconds(_timeout);
  size_t sent = 0;
  while (sent < n) {
    ssize_t count =
        ::send(state_->fd, data + sent, n - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (count > 0) {
      sent += size_t(count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        waitSocket(state_->fd, POLLOUT, deadline) > 0)
      continue;
    setWriteError();
    break;
  }
  if (state_->sync && !flush(unsigned(_timeout)))
    setWriteError();
  return sent;
}
bool WiFiClient::flush(unsigned int timeout) {
  if (!state_)
    return true;
  Guard lock(state_->mutex);
  if (state_->fd < 0)
    return false;
  auto end = Clock::now() +
             std::chrono::milliseconds(timeout ? timeout
                                               : WIFICLIENT_MAX_FLUSH_WAIT_MS);
  for (;;) {
    int pending = 0;
    if (ioctl(state_->fd, TIOCOUTQ, &pending) < 0)
      return false;
    if (!pending)
      return true;
    if (Clock::now() >= end) {
      errno = ETIMEDOUT;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
bool WiFiClient::stop(unsigned int timeout) {
  if (!state_)
    return true;
  bool done = flush(timeout);
  state_->close();
  state_.reset();
  return done;
}
void WiFiClient::abort() {
  if (!state_)
    return;
  {
    Guard lock(state_->mutex);
    linger option{1, 0};
    if (state_->fd >= 0)
      setsockopt(state_->fd, SOL_SOCKET, SO_LINGER, &option, sizeof(option));
    state_->close();
  }
  state_.reset();
}
IPAddress WiFiClient::remoteIP() {
  if (!state_)
    return {};
  Guard lock(state_->mutex);
  return IPAddress(socketAddress(state_->fd, true).sin_addr.s_addr);
}
uint16_t WiFiClient::remotePort() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  return ntohs(socketAddress(state_->fd, true).sin_port);
}
IPAddress WiFiClient::localIP() {
  if (!state_)
    return {};
  Guard lock(state_->mutex);
  return IPAddress(socketAddress(state_->fd, false).sin_addr.s_addr);
}
uint16_t WiFiClient::localPort() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  return ntohs(socketAddress(state_->fd, false).sin_port);
}
void WiFiClient::setLocalPortStart(uint16_t port) { localPortStart = port; }
int WiFiClient::availableForWrite() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  if (state_->fd < 0)
    return 0;
  int size = 0, used = 0;
  socklen_t n = sizeof(size);
  if (getsockopt(state_->fd, SOL_SOCKET, SO_SNDBUF, &size, &n) < 0 ||
      ioctl(state_->fd, TIOCOUTQ, &used) < 0)
    return 0;
  return std::max(0, size - used);
}
void WiFiClient::stopAll() { Registry<esp_mock::TcpState>::stopExcept({}); }
void WiFiClient::stopAllExcept(WiFiClient *except) {
  Registry<esp_mock::TcpState>::stopExcept(except ? except->state_ : nullptr);
}
void WiFiClient::keepAlive(uint16_t idle, uint16_t interval, uint8_t count) {
  if (!state_)
    return;
  Guard lock(state_->mutex);
  if (state_->fd < 0)
    return;
  int enabled = idle && interval && count;
  setsockopt(state_->fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
  if (enabled) {
    int a = idle, b = interval, c = count;
    setsockopt(state_->fd, IPPROTO_TCP, TCP_KEEPIDLE, &a, sizeof(a));
    setsockopt(state_->fd, IPPROTO_TCP, TCP_KEEPINTVL, &b, sizeof(b));
    setsockopt(state_->fd, IPPROTO_TCP, TCP_KEEPCNT, &c, sizeof(c));
  }
}
bool WiFiClient::isKeepAliveEnabled() const {
  return tcpOption(state_, SO_KEEPALIVE, SOL_SOCKET);
}
uint16_t WiFiClient::getKeepAliveIdle() const {
  return isKeepAliveEnabled() ? uint16_t(tcpOption(state_, TCP_KEEPIDLE)) : 0;
}
uint16_t WiFiClient::getKeepAliveInterval() const {
  return isKeepAliveEnabled() ? uint16_t(tcpOption(state_, TCP_KEEPINTVL)) : 0;
}
uint8_t WiFiClient::getKeepAliveCount() const {
  return isKeepAliveEnabled() ? uint8_t(tcpOption(state_, TCP_KEEPCNT)) : 0;
}
void WiFiClient::setDefaultNoDelay(bool value) { defaultNoDelay = value; }
bool WiFiClient::getDefaultNoDelay() { return defaultNoDelay; }
bool WiFiClient::getNoDelay() const { return tcpOption(state_, TCP_NODELAY); }
void WiFiClient::setNoDelay(bool value) {
  if (!state_)
    return;
  Guard lock(state_->mutex);
  int n = value;
  if (state_->fd >= 0)
    setsockopt(state_->fd, IPPROTO_TCP, TCP_NODELAY, &n, sizeof(n));
}
void WiFiClient::setDefaultSync(bool value) { defaultSync = value; }
bool WiFiClient::getDefaultSync() { return defaultSync; }
bool WiFiClient::getSync() const {
  if (!state_)
    return defaultSync;
  Guard lock(state_->mutex);
  return state_->sync;
}
void WiFiClient::setSync(bool value) {
  if (state_) {
    Guard lock(state_->mutex);
    state_->sync = value;
  }
}

WiFiServer::WiFiServer(const IPAddress &address, uint16_t port)
    : state_(std::make_shared<esp_mock::ServerState>(address, port)) {
  Registry<esp_mock::ServerState>::add(state_);
}
WiFiServer::WiFiServer(uint16_t port) : WiFiServer(IPAddress(), port) {}
WiFiServer::~WiFiServer() { close(); }
void WiFiServer::begin() { begin(state_->port, state_->backlog); }
void WiFiServer::begin(uint16_t port) { begin(port, 5); }
void WiFiServer::begin(uint16_t port, uint8_t backlog) {
  close();
  state_->port = port;
  state_->backlog = backlog;
  if (!backlog) {
    errno = EINVAL;
    return;
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  auto address = endpoint(state_->address, port);
  if (!nonblocking(fd) ||
      bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
      listen(fd, backlog) < 0) {
    int error = errno;
    ::close(fd);
    errno = error;
    return;
  }
  state_->fd = fd;
  state_->port = ntohs(socketAddress(fd, false).sin_port);
}
WiFiClient WiFiServer::accept() {
  fillPending(*state_);
  if (state_->pending.empty())
    return {};
  auto client = state_->pending.front();
  state_->pending.pop_front();
  return WiFiClient(std::move(client));
}
WiFiClient WiFiServer::available(uint8_t *status) {
  auto client = accept();
  if (status)
    *status = client.status();
  return client;
}
bool WiFiServer::hasClient() {
  fillPending(*state_);
  return !state_->pending.empty();
}
size_t WiFiServer::hasClientData() {
  fillPending(*state_);
  return state_->pending.empty()
             ? 0
             : size_t(WiFiClient(state_->pending.front()).available());
}
bool WiFiServer::hasMaxPendingClients() {
  fillPending(*state_);
  return state_->fd >= 0 && state_->pending.size() >= state_->backlog;
}
void WiFiServer::setNoDelay(bool value) {
  state_->noDelay = value;
  for (auto &client : state_->pending) {
    int option = value;
    setsockopt(client->fd, IPPROTO_TCP, TCP_NODELAY, &option, sizeof(option));
  }
}
bool WiFiServer::getNoDelay() {
  return state_->noDelay < 0 ? WiFiClient::getDefaultNoDelay()
                             : bool(state_->noDelay);
}
uint8_t WiFiServer::status() { return state_->fd >= 0 ? 1 : 0; }
uint16_t WiFiServer::port() const { return state_->port; }
void WiFiServer::close() { state_->close(); }

WiFiUDP::operator bool() const {
  if (!state_)
    return false;
  Guard lock(state_->mutex);
  return state_->fd >= 0;
}
uint8_t WiFiUDP::begin(uint16_t port) { return begin(IPAddress(), port); }
uint8_t WiFiUDP::begin(IPAddress ip, uint16_t port) {
  state_.reset();
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return 0;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
#ifdef IP_PKTINFO
  setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &yes, sizeof(yes));
#endif
  auto address = endpoint(ip, port);
  if (!nonblocking(fd) ||
      bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
    int error = errno;
    ::close(fd);
    errno = error;
    return 0;
  }
  try {
    state_ = std::make_shared<esp_mock::UdpState>(fd);
  } catch (...) {
    ::close(fd);
    throw;
  }
  Registry<esp_mock::UdpState>::add(state_);
  return 1;
}
void WiFiUDP::stop() {
  if (state_)
    state_->close();
  state_.reset();
}
uint8_t WiFiUDP::beginMulticast(IPAddress interfaceAddress, IPAddress group,
                                uint16_t port) {
  if (!IN_MULTICAST(ntohl(uint32_t(group)))) {
    errno = EINVAL;
    return 0;
  }
  if (!begin(port))
    return 0;
  ip_mreq membership{};
  membership.imr_multiaddr.s_addr = uint32_t(group);
  membership.imr_interface.s_addr = uint32_t(interfaceAddress);
  if (setsockopt(state_->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                 sizeof(membership)) < 0) {
    int error = errno;
    stop();
    errno = error;
    return 0;
  }
  return 1;
}
int WiFiUDP::beginPacket(const char *host, uint16_t port) {
  IPAddress ip;
  return resolve(host, ip, _timeout) ? beginPacket(ip, port) : 0;
}
int WiFiUDP::beginPacket(IPAddress ip, uint16_t port) {
  if (!port || !ip.isSet()) {
    errno = EINVAL;
    return 0;
  }
  if (!*this && !begin(0))
    return 0;
  Guard lock(state_->mutex);
  state_->destination = endpoint(ip, port);
  state_->tx.clear();
  state_->building = true;
  return 1;
}
int WiFiUDP::beginPacketMulticast(IPAddress group, uint16_t port,
                                  IPAddress interfaceAddress, int ttl) {
  if (ttl < 0 || ttl > 255 || !IN_MULTICAST(ntohl(uint32_t(group)))) {
    errno = EINVAL;
    return 0;
  }
  if (!beginPacket(group, port))
    return 0;
  Guard lock(state_->mutex);
  in_addr interface{uint32_t(interfaceAddress)};
  unsigned char value = static_cast<unsigned char>(ttl);
  if (setsockopt(state_->fd, IPPROTO_IP, IP_MULTICAST_IF, &interface,
                 sizeof(interface)) < 0 ||
      setsockopt(state_->fd, IPPROTO_IP, IP_MULTICAST_TTL, &value,
                 sizeof(value)) < 0) {
    state_->building = false;
    return 0;
  }
  return 1;
}
size_t WiFiUDP::write(uint8_t value) { return write(&value, 1); }
size_t WiFiUDP::write(const uint8_t *data, size_t n) {
  if (!state_ || (!data && n)) {
    errno = EINVAL;
    setWriteError();
    return 0;
  }
  Guard lock(state_->mutex);
  if (!state_->building || state_->fd < 0) {
    errno = ENOTCONN;
    setWriteError();
    return 0;
  }
  if (n > 65507 - state_->tx.size()) {
    errno = EMSGSIZE;
    setWriteError();
    return 0;
  }
  if (n)
    state_->tx.insert(state_->tx.end(), data, data + n);
  return n;
}
int WiFiUDP::endPacket() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  if (!state_->building || state_->fd < 0)
    return 0;
  ssize_t n;
  do {
    n = sendto(state_->fd, state_->tx.data(), state_->tx.size(), MSG_NOSIGNAL,
               reinterpret_cast<sockaddr *>(&state_->destination),
               sizeof(state_->destination));
  } while (n < 0 && errno == EINTR);
  bool ok = n >= 0 && size_t(n) == state_->tx.size();
  state_->tx.clear();
  state_->building = false;
  if (!ok)
    setWriteError();
  return ok;
}
int WiFiUDP::parsePacket() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  if (state_->fd < 0)
    return 0;
  state_->rx.clear();
  state_->position = 0;
  ssize_t size;
  do {
    size = recvfrom(state_->fd, nullptr, 0, MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT,
                    nullptr, nullptr);
  } while (size < 0 && errno == EINTR);
  if (size < 0)
    return 0;
  state_->rx.resize(size_t(size));
  iovec data{state_->rx.data(), state_->rx.size()};
  msghdr message{};
  message.msg_name = &state_->remote;
  message.msg_namelen = sizeof(state_->remote);
  message.msg_iov = &data;
  message.msg_iovlen = 1;
#ifdef IP_PKTINFO
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(in_pktinfo))]{};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
#endif
  ssize_t received;
  do {
    received = recvmsg(state_->fd, &message, MSG_DONTWAIT);
  } while (received < 0 && errno == EINTR);
  if (received < 0) {
    state_->rx.clear();
    return 0;
  }
  state_->rx.resize(size_t(received));
  state_->receivedOn =
      IPAddress(socketAddress(state_->fd, false).sin_addr.s_addr);
#ifdef IP_PKTINFO
  for (cmsghdr *item = CMSG_FIRSTHDR(&message); item;
       item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level == IPPROTO_IP && item->cmsg_type == IP_PKTINFO &&
        item->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
      in_pktinfo info{};
      std::memcpy(&info, CMSG_DATA(item), sizeof(info));
      state_->receivedOn = IPAddress(info.ipi_addr.s_addr);
    }
  }
#endif
  return int(received);
}
int WiFiUDP::available() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  return int(state_->rx.size() - state_->position);
}
int WiFiUDP::read() {
  uint8_t value;
  return read(&value, 1) == 1 ? value : -1;
}
int WiFiUDP::read(uint8_t *data, size_t n) {
  if (!data && n) {
    errno = EINVAL;
    return -1;
  }
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  size_t count = std::min(n, state_->rx.size() - state_->position);
  if (count)
    std::memcpy(data, state_->rx.data() + state_->position, count);
  state_->position += count;
  return int(count);
}
int WiFiUDP::peek() {
  if (!state_)
    return -1;
  Guard lock(state_->mutex);
  return state_->position < state_->rx.size() ? state_->rx[state_->position]
                                              : -1;
}
void WiFiUDP::flush() { (void)endPacket(); }
IPAddress WiFiUDP::remoteIP() {
  if (!state_)
    return {};
  Guard lock(state_->mutex);
  return IPAddress(state_->remote.sin_addr.s_addr);
}
uint16_t WiFiUDP::remotePort() {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  return ntohs(state_->remote.sin_port);
}
IPAddress WiFiUDP::destinationIP() const {
  if (!state_)
    return {};
  Guard lock(state_->mutex);
  return state_->receivedOn;
}
uint16_t WiFiUDP::localPort() const {
  if (!state_)
    return 0;
  Guard lock(state_->mutex);
  return ntohs(socketAddress(state_->fd, false).sin_port);
}
void WiFiUDP::stopAll() { Registry<esp_mock::UdpState>::stopExcept({}); }
void WiFiUDP::stopAllExcept(WiFiUDP *except) {
  Registry<esp_mock::UdpState>::stopExcept(except ? except->state_ : nullptr);
}

void esp_mock::resetNetwork() {
  Registry<esp_mock::ServerState>::stopExcept({});
  WiFiClient::stopAll();
  WiFiUDP::stopAll();
}
