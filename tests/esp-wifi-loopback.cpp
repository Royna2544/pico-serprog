#include <ESP8266WiFi.h>
#include <cassert>
#include <cstdlib>
#include <iostream>

int main() {
  assert(std::getenv("MOCK_WIFI_LOOPBACK"));
  assert(WiFi.mode(WIFI_STA));
  assert(WiFi.begin("emulator", "test-only") == WL_CONNECTED);
  assert(WiFi.status() == WL_CONNECTED);
  assert(WiFi.localIP() == IPAddress(127, 0, 0, 1));
  assert(WiFi.scanNetworks() == 0);
  assert(WiFi.SSID() == "host-loopback");
  assert(WiFi.RSSI() == -50);
  assert(WiFi.macAddress() == "02:00:00:00:00:01");
  assert(WiFi.disconnect());
  assert(WiFi.status() == WL_DISCONNECTED);
  std::cout << "PASS host-only loopback Wi-Fi without NetworkManager\n";
}
