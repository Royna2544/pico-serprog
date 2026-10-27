#include "Stream.h"
#include <array>
#include <chrono>
#include <thread>
#include <vector>
namespace {
using Clock = std::chrono::steady_clock;
size_t formatTo(Print &out, const char *format, va_list args) {
  if (!format)
    return 0;
  va_list copy;
  va_copy(copy, args);
  int n = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (n < 0)
    return 0;
  std::vector<char> data(size_t(n) + 1);
  std::vsnprintf(data.data(), data.size(), format, args);
  return out.write(data.data(), size_t(n));
}
} // namespace
size_t Print::printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  auto n = formatTo(*this, format, args);
  va_end(args);
  return n;
}
size_t Print::printf_P(const char *format, ...) {
  va_list args;
  va_start(args, format);
  auto n = formatTo(*this, format, args);
  va_end(args);
  return n;
}
int Stream::timedRead() {
  auto end = Clock::now() + std::chrono::milliseconds(_timeout);
  do {
    int c = read();
    if (c >= 0)
      return c;
    if (!inputCanTimeout())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (Clock::now() < end);
  return -1;
}
int Stream::timedPeek() {
  auto end = Clock::now() + std::chrono::milliseconds(_timeout);
  do {
    int c = peek();
    if (c >= 0)
      return c;
    if (!inputCanTimeout())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (Clock::now() < end);
  return -1;
}
int Stream::read(uint8_t *data, size_t n) {
  if (!data && n)
    return -1;
  size_t done = 0;
  while (done < n) {
    int c = read();
    if (c < 0)
      break;
    data[done++] = uint8_t(c);
  }
  return int(done);
}
size_t Stream::readBytes(char *data, size_t n) {
  if (!data && n)
    return 0;
  size_t done = 0;
  auto end = Clock::now() + std::chrono::milliseconds(_timeout);
  while (done < n) {
    int ready = available();
    if (ready > 0) {
      int count = read(reinterpret_cast<uint8_t *>(data + done),
                       std::min(n - done, size_t(ready)));
      if (count > 0) {
        done += size_t(count);
        end = Clock::now() + std::chrono::milliseconds(_timeout);
        continue;
      }
    }
    if (!inputCanTimeout() || Clock::now() >= end)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done;
}
size_t Stream::readBytesUntil(char end, char *data, size_t n) {
  if (!data && n)
    return 0;
  size_t done = 0;
  while (done < n) {
    int c = timedRead();
    if (c < 0 || c == end)
      break;
    data[done++] = char(c);
  }
  return done;
}
String Stream::readString() {
  String out;
  for (int c; (c = timedRead()) >= 0;)
    out += char(c);
  return out;
}
String Stream::readStringUntil(char end) {
  String out;
  for (int c; (c = timedRead()) >= 0 && c != end;)
    out += char(c);
  return out;
}
bool Stream::find(const char *s) { return s && find(s, std::strlen(s)); }
bool Stream::find(const char *s, size_t n) {
  if (!s)
    return false;
  if (!n)
    return true;
  std::string window;
  for (int c; (c = timedRead()) >= 0;) {
    window += char(c);
    if (window.size() > n)
      window.erase(0, 1);
    if (window.size() == n && !std::memcmp(window.data(), s, n))
      return true;
  }
  return false;
}
bool Stream::findUntil(const char *s, const char *end) {
  if (!s || !end)
    return false;
  std::string window;
  size_t n = std::strlen(s), m = std::strlen(end);
  if (!n)
    return true;
  for (int c; (c = timedRead()) >= 0;) {
    window += char(c);
    if (window.size() > std::max(n, m))
      window.erase(0, 1);
    if (window.size() >= n && window.compare(window.size() - n, n, s) == 0)
      return true;
    if (m && window.size() >= m &&
        window.compare(window.size() - m, m, end) == 0)
      return false;
  }
  return false;
}
long Stream::parseInt() {
  String value;
  int c;
  while ((c = timedPeek()) >= 0 && c != '-' && (c < '0' || c > '9'))
    read();
  if (c == '-') {
    value += '-';
    read();
  }
  while ((c = timedPeek()) >= '0' && c <= '9') {
    value += char(c);
    read();
  }
  return value.toInt();
}
float Stream::parseFloat() {
  String value;
  int c;
  bool dot = false;
  while ((c = timedPeek()) >= 0 && c != '-' && c != '.' && (c < '0' || c > '9'))
    read();
  if (c == '-') {
    value += '-';
    read();
  }
  while ((c = timedPeek()) >= 0) {
    if (c == '.' && !dot)
      dot = true;
    else if (c < '0' || c > '9')
      break;
    value += char(c);
    read();
  }
  return value.toFloat();
}
size_t Stream::sendSize(Print *out, size_t n, unsigned long timeout) {
  if (!out)
    return 0;
  auto limit = std::chrono::milliseconds(timeout ? timeout : _timeout);
  auto end = Clock::now() + limit;
  std::array<uint8_t, 4096> buf{};
  size_t done = 0;
  while (done < n) {
    int ready = available();
    if (ready > 0) {
      int count =
          read(buf.data(), std::min({buf.size(), n - done, size_t(ready)}));
      if (count > 0) {
        size_t offset = 0;
        while (offset < size_t(count)) {
          size_t sent = out->write(buf.data() + offset, size_t(count) - offset);
          if (!sent)
            return done + offset;
          offset += sent;
        }
        done += size_t(count);
        end = Clock::now() + limit;
        continue;
      }
    }
    if (!inputCanTimeout() || Clock::now() >= end)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done;
}
size_t Stream::sendAll(Print *out, unsigned long timeout) {
  ssize_t remaining = streamRemaining();
  return sendSize(out,
                  remaining >= 0 ? size_t(remaining)
                                 : std::numeric_limits<size_t>::max(),
                  timeout);
}
