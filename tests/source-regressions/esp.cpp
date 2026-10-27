#include <cassert>
#include <deque>
#include <string>
#include "Arduino.h"

static std::deque<unsigned char> input;
static std::string output;
MockSerial Serial;
void MockSerial::begin(unsigned long) {}
void MockSerial::reset() { input.clear(); }
int MockSerial::available() { return !input.empty(); }
int MockSerial::peek() { return input.empty() ? -1 : input.front(); }
int MockSerial::read() {
  if (input.empty()) return -1;
  int value = input.front(); input.pop_front(); return value;
}
size_t MockSerial::write(uint8_t value) { output += (char)value; return 1; }
void pinMode(unsigned, int) {}
void digitalWrite(unsigned, int) {}
void delay(unsigned long) {}
#include "esp8266/esp8266.ino"

static void feed(const std::string &bytes) {
  for (unsigned char c : bytes) input.push_back(c);
  loop();
}
int main() {
  std::string frame;
  auto put = [](void *data, int c) { *static_cast<std::string *>(data) += (char)c; };
  assert(make_wlan_talk_message(put, &frame, "SLAVE", "pico-serprog") == 23);
  feed(frame);
  const std::string reply = output;
  assert(reply.size() == 19);
  output.clear();
  for (size_t i = 0; i < frame.size(); i++) {
    feed(frame.substr(i, 1));
    if (i + 1 < frame.size()) assert(output.empty());
  }
  assert(output == reply);
  output.clear();
  feed(frame + frame);
  assert(output == reply + reply);
  output.clear();
  feed(":999:SLAVE:bad\n" + frame);
  assert(output == reply);
  output.clear();
  feed(":" + std::string(256, 'X') + "\nnoise" + frame);
  assert(output == reply);
  return 0;
}
