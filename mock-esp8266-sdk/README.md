# ESP8266 host networking

The host SDK implements the ESP8266 Arduino core **3.1.2** plain networking
interfaces with real Linux IPv4 sockets. Include the standard headers:
`ESP8266WiFi.h`, `WiFiClient.h`, `WiFiServer.h`, `WiFiUdp.h`, and
`ESP8266WebServer.h`. The existing NetworkManager station/AP implementation
remains in `Wlan.cpp`.

- `WiFiServer`: listening, pending-connection queues, `accept()`/`available()`,
  backlog and no-delay settings, endpoint ports, and shutdown.
- `WiFiClient`: hostname/IP connections, bounded connect/write/read waits,
  partial writes, binary streams, EOF with buffered input, peek APIs,
  keepalive, no-delay/sync settings, cloning/copying, and stop/abort helpers.
- `WiFiUDP`: datagram construction and parsing, binary and empty packets,
  packet boundaries, peer/destination metadata, multicast, broadcast,
  shared-context copies, and stop helpers. `flush()` sends pending output;
  it does not discard input. Unread input is discarded by `parsePacket()`.
- `ESP8266WebServer`: the upstream HTTP engine, including method-based
  routes, query/form arguments, headers, REST bodies, uploads, Basic/Digest
  authentication, chunked responses, keep-alive, hooks/custom handlers,
  URI matchers, file streaming, CORS, and ETags. It services one active
  HTTP client at a time, as the ESP8266 engine does. Call `handleClient()`
  from the sketch's loop.

Client and UDP copies share their connection context. Destroying a copy only
releases its reference; explicit `stop()`/`abort()` closes the shared transport.
`WiFiClient::connect()` stops its old connection before connecting again.
Each accepted server client has an independent lifetime from its listener.
The host runtime closes listeners, TCP connections, and UDP sockets when the
simulated ESP powers off or reboots, before the next `setup()` invocation.
Concurrent access to the same sketch object still needs caller synchronization;
the shared TCP/UDP contexts serialize their socket and buffer operations.

Networking uses the host's routes, resolver, interfaces, and port permissions;
it does not require a mocked successful `WiFi.begin()` to use loopback or an
already configured host connection. Port numbers are not remapped. Port zero
selects an ephemeral host port, discoverable through `WiFiServer::port()` or
`WiFiUDP::localPort()`. TLS-specific classes and IPv6 are outside these four
plain networking interfaces.

`FS hostFiles("/absolute/web/root")` supplies `serveStatic()` and file streaming
with a real host directory. An unconfigured `FS` has no root. File operations
reject paths and symbolic links escaping the configured root. Program-memory
strings use ordinary host memory. OpenSSL supplies the HTTP engine's base64
and MD5 helpers. The vendored engine's revision, license, and host adjustments
are recorded in `vendor/ESP8266WebServer/UPSTREAM.md`.

The CMake `esp-mock-sdk` target exports its headers, C++17 requirement, and
dependencies. Host dependencies are libnm, OpenSSL/libcrypto, POSIX threads,
and Python 3 for the wire tests. `ctest` in a `MOCK_HARDWARE=ON` build runs
the socket and independent Python HTTP tests. `MOCK_NETWORK_SANITIZERS=ON`
adds AddressSanitizer and UBSan to the host SDK and its consumers.
