#include <ESP8266WebServer.h>
#include <MockNetwork.h>
#include <StreamDev.h>
#include <arpa/inet.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <uri/UriBraces.h>
#include <uri/UriGlob.h>
#include <uri/UriRegex.h>
#include <vector>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      throw std::runtime_error(std::string(__FILE__) + ":" +                   \
                               std::to_string(__LINE__) + ": " #condition);    \
  } while (0)
namespace {
const IPAddress loopback(127, 0, 0, 1);
template <class F> void waitFor(F predicate) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!predicate()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
struct Fd {
  int value = -1;
  explicit Fd(int fd) : value(fd) { CHECK(fd >= 0); }
  ~Fd() {
    if (value >= 0)
      ::close(value);
  }
  Fd(const Fd &) = delete;
};
uint16_t portOf(int fd) {
  sockaddr_in address{};
  socklen_t n = sizeof(address);
  CHECK(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &n) == 0);
  return ntohs(address.sin_port);
}
sockaddr_in address(uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = uint32_t(loopback);
  a.sin_port = htons(port);
  return a;
}
void bindLocal(int fd) {
  auto a = address(0);
  CHECK(bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0);
  timeval timeout{3, 0};
  CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
        0);
}

void support() {
  IPAddress ip;
  CHECK(ip.fromString("127.0.0.1") && ip == loopback);
  CHECK(!ip.fromString("256.1.2.3") && ip == loopback);
  CHECK(IPAddress(uint32_t(ip)).toString() == "127.0.0.1");
  CHECK(String(-123) == "-123" && String(255, 16) == "ff");
  CHECK(String(int8_t(-50)) == "-50" && String(true) == "1");
  CHECK((String("n=") + 123) == "n=123");
  String output;
  S2Stream sink(output);
  CHECK(sink.println(0xab, HEX) == 4 && output == "AB\r\n");
  output.clear();
  sink.print(loopback);
  CHECK(output == "127.0.0.1");
  const char data[] = {'a', 0, 'b'};
  StreamConstPtr source(data, sizeof(data));
  output.clear();
  CHECK(source.sendAll(sink) == 3 && output.size() == 3 && output[1] == 0);
  CHECK(ESP8266WebServer::credentialHash("Mufasa", "testrealm@host.com",
                                         "Circle Of Life") ==
        "939e7578ed9e3c518a452acee763bce9");
}
void tcp() {
  WiFiServer server(loopback, 0);
  server.begin();
  CHECK(server.status() == 1 && server.port() != 0);
  CHECK(!server.hasClient() && !server.accept());
  server.setNoDelay(true);
  WiFiClient outgoing;
  CHECK(outgoing.connect("localhost", server.port()) == 1);
  waitFor([&] { return server.hasClient(); });
  WiFiClient incoming = server.accept();
  CHECK(incoming && incoming.getNoDelay());
  CHECK(outgoing.localIP() == loopback && outgoing.remoteIP() == loopback);
  CHECK(outgoing.remotePort() == server.port() &&
        outgoing.localPort() == incoming.remotePort());
  CHECK(outgoing.read() == -1 && outgoing.available() == 0);
  const uint8_t bytes[] = {'a', 0, 'b', '\n'};
  CHECK(incoming.write(bytes, sizeof(bytes)) == sizeof(bytes));
  waitFor([&] { return outgoing.available() == 4; });
  CHECK(outgoing.peek() == 'a');
  char peek[4];
  CHECK(outgoing.peekBytes(peek, 4) == 4 && peek[1] == 0 &&
        outgoing.available() == 4);
  auto copy = outgoing;
  auto clone = outgoing.clone();
  CHECK(copy.read() == 'a' && clone->read() == 0);
  CHECK(outgoing.peekAvailable() == 2 && outgoing.peekBuffer()[0] == 'b');
  outgoing.peekConsume(1);
  CHECK(outgoing.read() == '\n');
  outgoing.keepAlive(7, 3, 2);
  CHECK(outgoing.isKeepAliveEnabled());
  CHECK(outgoing.getKeepAliveIdle() == 7 &&
        outgoing.getKeepAliveInterval() == 3 &&
        outgoing.getKeepAliveCount() == 2);
  outgoing.disableKeepAlive();
  CHECK(!outgoing.isKeepAliveEnabled());
  CHECK(outgoing.getKeepAliveIdle() == 0 &&
        outgoing.getKeepAliveInterval() == 0 &&
        outgoing.getKeepAliveCount() == 0);
  outgoing.setNoDelay(true);
  CHECK(outgoing.getNoDelay());
  outgoing.setSync(true);
  CHECK(outgoing.getSync());
  CHECK(outgoing.println("reply") == 7);
  CHECK(outgoing.flush(1000));
  waitFor([&] { return incoming.available() >= 7; });
  CHECK(incoming.readStringUntil('\n') == "reply\r");
  CHECK(incoming.write("tail") == 4);
  CHECK(incoming.stop(1000));
  waitFor([&] { return outgoing.available() == 4; });
  CHECK(outgoing.connected());
  char tail[4];
  CHECK(outgoing.readBytes(tail, 4) == 4 && std::string(tail, 4) == "tail");
  waitFor([&] { return !outgoing.connected(); });
  CHECK(outgoing.status() == CLOSE_WAIT);
  outgoing.stop();
  CHECK(!copy && !*clone);
  uint16_t closed = server.port();
  server.close();
  CHECK(!server.status());
  WiFiClient refused;
  refused.setTimeout(100);
  CHECK(!refused.connect(loopback, closed));
  CHECK(!refused.connect(nullptr, closed));
}
void pending() {
  WiFiServer server(loopback, 0);
  server.begin(0, 2);
  CHECK(server.status());
  WiFiClient a, b;
  CHECK(a.connect(loopback, server.port()) &&
        b.connect(loopback, server.port()));
  waitFor([&] { return server.hasMaxPendingClients(); });
  CHECK(server.hasClientData() == 0);
  CHECK(a.write("x") == 1);
  waitFor([&] { return server.hasClientData() == 1; });
  auto first = server.accept();
  CHECK(first.remotePort() == a.localPort());
  auto second = server.available();
  CHECK(second.remotePort() == b.localPort());
  CHECK(!server.hasClient());
  server.stop();
  CHECK(a.write("still accepted") == 14);
  CHECK(first.connected());
  WiFiServer occupied(loopback, 0);
  occupied.begin();
  WiFiServer conflict(loopback, occupied.port());
  conflict.begin();
  CHECK(conflict.status() == 0);
}
void peekFragments() {
  WiFiServer server(loopback, 0);
  server.begin();
  WiFiClient client;
  CHECK(client.connect(loopback, server.port()));
  auto peer = server.accept();
  CHECK(peer);
  client.setTimeout(500);
  char output[5];
  auto writer = std::async(std::launch::async, [peer]() mutable {
    CHECK(peer.write("ab") == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(peer.write("cde") == 3);
  });
  CHECK(client.peekBytes(output, 5) == 5 && std::string(output, 5) == "abcde");
  writer.get();
  CHECK(client.readBytes(output, 5) == 5);
  CHECK(client.read(reinterpret_cast<uint8_t *>(output), 5) == 0);
  std::vector<uint8_t> large(65536), received(65536);
  for (size_t i = 0; i < large.size(); i++)
    large[i] = uint8_t(i);
  CHECK(peer.write(large.data(), large.size()) == large.size());
  CHECK(client.peekBytes(received.data(), received.size()) == received.size() &&
        received == large);
  CHECK(client.readBytes(received.data(), received.size()) == received.size() &&
        received == large);
}
void ownership() {
  Fd listener(socket(AF_INET, SOCK_STREAM, 0));
  bindLocal(listener.value);
  CHECK(listen(listener.value, 5) == 0);
  WiFiClient a, b;
  CHECK(a.connect(loopback, portOf(listener.value)));
  Fd pa(::accept(listener.value, nullptr, nullptr));
  CHECK(b.connect(loopback, portOf(listener.value)));
  Fd pb(::accept(listener.value, nullptr, nullptr));
  {
    auto copy = a;
    CHECK(copy.connected());
  }
  CHECK(a.connected());
  WiFiClient::stopAllExcept(&a);
  CHECK(a.connected() && !b.connected());
  auto copy = a;
  copy.abort();
  CHECK(!a.connected());
  CHECK(a.connect(loopback, portOf(listener.value)));
  Fd pc(::accept(listener.value, nullptr, nullptr));
  WiFiClient::stopAll();
  CHECK(!a.connected());
}
void pressure() {
  Fd listener(socket(AF_INET, SOCK_STREAM, 0));
  bindLocal(listener.value);
  int small = 1024;
  CHECK(setsockopt(listener.value, SOL_SOCKET, SO_RCVBUF, &small,
                   sizeof(small)) == 0);
  CHECK(listen(listener.value, 1) == 0);
  WiFiClient client;
  CHECK(client.connect(loopback, portOf(listener.value)));
  Fd accepted(::accept(listener.value, nullptr, nullptr));
  client.setTimeout(50);
  std::vector<uint8_t> data(16 * 1024 * 1024, 0x5a);
  auto start = std::chrono::steady_clock::now();
  size_t n = client.write(data.data(), data.size());
  CHECK(n > 0 && n < data.size() && client.getWriteError());
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
  client.abort();
}
void udp() {
  Fd peer(socket(AF_INET, SOCK_DGRAM, 0));
  bindLocal(peer.value);
  WiFiUDP udp;
  CHECK(udp.begin(0));
  CHECK(udp.beginPacket("localhost", portOf(peer.value)));
  const uint8_t data[] = {'a', 0, 'b'};
  CHECK(udp.write(data, 3) == 3 && udp.endPacket());
  char buffer[32];
  sockaddr_in sender{};
  socklen_t length = sizeof(sender);
  CHECK(recvfrom(peer.value, buffer, sizeof(buffer), 0,
                 reinterpret_cast<sockaddr *>(&sender), &length) == 3 &&
        buffer[1] == 0);
  CHECK(ntohs(sender.sin_port) == udp.localPort());
  auto destination = address(udp.localPort());
  CHECK(sendto(peer.value, "first", 5, 0,
               reinterpret_cast<sockaddr *>(&destination),
               sizeof(destination)) == 5);
  CHECK(sendto(peer.value, "next", 4, 0,
               reinterpret_cast<sockaddr *>(&destination),
               sizeof(destination)) == 4);
  waitFor([&] { return udp.parsePacket() == 5; });
  CHECK(udp.available() == 5 && udp.peek() == 'f' && udp.read() == 'f');
  CHECK(udp.remoteIP() == loopback && udp.remotePort() == portOf(peer.value) &&
        udp.destinationIP() == loopback);
  CHECK(udp.parsePacket() == 4 && udp.available() == 4);
  CHECK(udp.read(buffer, 2) == 2 && std::string(buffer, 2) == "ne");
  CHECK(udp.beginPacket(loopback, portOf(peer.value)));
  CHECK(udp.print("flush") == 5);
  udp.flush();
  CHECK(udp.available() == 2 &&
        recv(peer.value, buffer, sizeof(buffer), 0) == 5);
  CHECK(udp.read(buffer, sizeof(buffer)) == 2 &&
        std::string(buffer, 2) == "xt");
  CHECK(udp.read() == -1);
  CHECK(udp.beginPacket(loopback, portOf(peer.value)));
  std::vector<uint8_t> tooBig(65508);
  CHECK(udp.write(tooBig.data(), tooBig.size()) == 0 && udp.getWriteError());
  CHECK(udp.endPacket());
  CHECK(recv(peer.value, buffer, sizeof(buffer), 0) ==
        0); // A valid empty datagram.
  auto copy = udp;
  udp.stop();
  CHECK(!copy && !udp);
  WiFiUDP a, b;
  CHECK(a.begin(0) && b.begin(0));
  WiFiUDP::stopAllExcept(&a);
  CHECK(a && !b);
  WiFiUDP::stopAll();
  CHECK(!a);
}
void multicast() {
  IPAddress group(239, 255, 42, 99);
  WiFiUDP listener, sender;
  CHECK(listener.beginMulticast(loopback, group, 0));
  CHECK(sender.beginPacketMulticast(group, listener.localPort(), loopback, 1));
  CHECK(sender.write("group") == 5 && sender.endPacket());
  waitFor([&] { return listener.parsePacket() == 5; });
  CHECK(listener.destinationIP() == group && listener.remoteIP() == loopback);
  char data[5];
  CHECK(listener.read(data, 5) == 5 && std::string(data, 5) == "group");
}
void broadcast() {
  WiFiUDP listener, sender;
  CHECK(listener.begin(0));
  IPAddress address(127, 255, 255, 255);
  CHECK(sender.beginPacket(address, listener.localPort()));
  CHECK(sender.write("broadcast") == 9 && sender.endPacket());
  waitFor([&] { return listener.parsePacket() == 9; });
  CHECK(listener.destinationIP() == address);
}
void descriptors() {
  auto count = [] {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator());
  };
  auto before = count();
  for (int i = 0; i < 100; i++) {
    WiFiServer server(loopback, 0);
    server.begin();
    WiFiClient client;
    CHECK(client.connect(loopback, server.port()));
    auto accepted = server.accept();
    CHECK(accepted);
    WiFiUDP udp;
    CHECK(udp.begin(0));
  }
  CHECK(count() == before);
}
void descriptorZero() {
  ::close(STDIN_FILENO);
  WiFiServer server(loopback, 0);
  server.begin();
  CHECK(server.status());
  WiFiClient client;
  CHECK(client.connect(loopback, server.port()));
  auto accepted = server.accept();
  CHECK(accepted);
  server.close();
  CHECK(client.write("ok") == 2);
}
void reset() {
  WiFiServer server(loopback, 0);
  server.begin();
  WiFiClient client;
  CHECK(client.connect(loopback, server.port()));
  auto peer = server.accept();
  CHECK(peer);
  WiFiUDP udp;
  CHECK(udp.begin(0));
  esp_mock::resetNetwork();
  CHECK(!server.status() && !client && !peer && !udp);
  server.begin();
  CHECK(server.status());
  CHECK(client.connect(loopback, server.port()));
  CHECK(server.accept() && udp.begin(0));
}
class CustomHandler : public RequestHandler {
  bool canHandle(HTTPMethod method, const String &uri) override {
    return method == HTTP_GET && uri == "/custom";
  }
  bool handle(ESP8266WebServer &server, HTTPMethod, const String &) override {
    server.send(200, "text/plain", "custom");
    return true;
  }
};
void httpFixture(const char *root) {
  ESP8266WebServer server(loopback, 0);
  FS files(root);
  CHECK(files.begin());
  bool quit = false;
  String uploaded, filename;
  size_t starts = 0, ends = 0;
  server.collectHeaders("X-Test", "If-None-Match", "Authorization");
  server.enableCORS(true);
  server.enableETag(true);
  server.on("/", HTTP_ANY,
            [&] { server.send(200, "text/html", "<h1>mock</h1>"); });
  server.on("/api", HTTP_ANY, [&] {
    server.sendHeader("X-Echo", server.header("X-Test"));
    server.send(200, "application/octet-stream", server.arg("plain"));
  });
  server.on("/args", [&] {
    server.send(200, "text/plain",
                server.arg("name") + "|" + server.arg("value") + "|" +
                    server.hostHeader());
  });
  server.on(UriBraces("/items/{}"), HTTP_GET,
            [&] { server.send(200, "text/plain", server.pathArg(0)); });
  server.on(UriGlob("/glob/*"), HTTP_GET,
            [&] { server.send(200, "text/plain", "glob"); });
  server.on(UriRegex("^/regex/([0-9]+)$"), HTTP_GET,
            [&] { server.send(200, "text/plain", server.pathArg(0)); });
  server.on("/form", HTTP_POST, [&] {
    server.send(200, "text/plain",
                server.arg("name") + "|" + server.arg("value"));
  });
  server.on("/auth", [&] {
    if (!server.authenticate("user", "pw"))
      server.requestAuthentication(BASIC_AUTH, "mock", "auth required");
    else
      server.send(200, "text/plain", "authorized");
  });
  server.on("/digest", [&] {
    if (!server.authenticate("user", "pw"))
      server.requestAuthentication(DIGEST_AUTH, "mock", "auth required");
    else
      server.send(200, "text/plain", "authorized");
  });
  server.on("/chunked", [&] {
    CHECK(server.chunkedResponseModeStart(200, "text/plain"));
    server.sendContent("first");
    server.sendContent(String("second"));
    server.chunkedResponseFinalize();
  });
  server.on(
      "/upload", HTTP_POST,
      [&] {
        server.sendHeader("X-Filename", filename);
        server.sendHeader("X-Upload-Events",
                          String(starts) + "," + String(ends));
        server.send(200, "application/octet-stream", uploaded);
      },
      [&] {
        auto &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          uploaded.clear();
          filename = upload.filename;
          starts++;
        } else if (upload.status == UPLOAD_FILE_WRITE)
          uploaded.concat(reinterpret_cast<const char *>(upload.buf),
                          upload.currentSize);
        else if (upload.status == UPLOAD_FILE_END)
          ends++;
      });
  server.on("/stream", [&] {
    StreamConstPtr data("streamed", 8);
    server.send(200, "text/plain", data, 8);
  });
  server.addHandler(new CustomHandler);
  server.addHook([](const String &, const String &uri, WiFiClient *client,
                    ESP8266WebServer::ContentTypeFunction) {
    if (uri != "/hook")
      return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;
    client->write("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: "
                  "close\r\n\r\nhook");
    return ESP8266WebServer::CLIENT_REQUEST_IS_HANDLED;
  });
  server.serveStatic("/static/", files, "/");
  server.onNotFound([&] { server.send(404, "text/plain", "missing"); });
  server.on("/quit", [&] {
    server.keepAlive(false);
    server.send(200, "text/plain", "bye");
    quit = true;
  });
  server.begin();
  CHECK(server.getServer().status());
  std::cout << server.getServer().port() << std::endl;
  while (!quit) {
    server.handleClient();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  server.client().flush(1000);
  server.stop();
}
} // namespace
int main(int argc, char **argv) {
  try {
    CHECK(argc >= 2);
    std::string test = argv[1];
    if (test == "support")
      support();
    else if (test == "tcp")
      tcp();
    else if (test == "pending")
      pending();
    else if (test == "peek-fragments")
      peekFragments();
    else if (test == "ownership")
      ownership();
    else if (test == "pressure")
      pressure();
    else if (test == "udp")
      udp();
    else if (test == "multicast")
      multicast();
    else if (test == "broadcast")
      broadcast();
    else if (test == "descriptors")
      descriptors();
    else if (test == "descriptor-zero")
      descriptorZero();
    else if (test == "reset")
      reset();
    else if (test == "http") {
      CHECK(argc == 3);
      httpFixture(argv[2]);
    } else
      throw std::runtime_error("unknown case");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
