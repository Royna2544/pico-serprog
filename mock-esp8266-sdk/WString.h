#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
class __FlashStringHelper;

// Arduino String semantics with host-owned storage, including binary strings.
class String : public std::string {
  static std::string integer(unsigned long long value, unsigned base) {
    if (base < 2 || base > 36)
      base = 10;
    std::string out;
    do {
      out += "0123456789abcdefghijklmnopqrstuvwxyz"[value % base];
      value /= base;
    } while (value);
    std::reverse(out.begin(), out.end());
    return out;
  }

public:
  String() = default;
  String(const char *s) : std::string(s ? s : "") {}
  String(const __FlashStringHelper *s)
      : String(reinterpret_cast<const char *>(s)) {}
  String(const char *s, size_t n) : std::string(s ? s : "", s ? n : 0) {}
  String(const std::string &s) : std::string(s) {}
  String(std::string &&s) : std::string(std::move(s)) {}
  explicit String(char c) : std::string(1, c) {}
  template <class T, std::enable_if_t<std::is_integral<T>::value &&
                                          !std::is_same<T, char>::value,
                                      int> = 0>
  explicit String(T n, unsigned base = 10) {
    using U = std::make_unsigned_t<
        std::conditional_t<std::is_same<T, bool>::value, unsigned char, T>>;
    if constexpr (std::is_signed<T>::value) {
      if (n < 0 && base == 10) {
        assign("-");
        append(integer(U(U(0) - U(n)), base));
        return;
      }
    }
    assign(integer(static_cast<U>(n), base));
  }
  explicit String(double n, unsigned digits = 2) {
    int count =
        std::snprintf(nullptr, 0, "%.*f", int(std::min(digits, 20U)), n);
    if (count > 0) {
      resize(count);
      std::snprintf(data(), size() + 1, "%.*f", int(std::min(digits, 20U)), n);
    }
  }
  using std::string::operator=;
  using std::string::operator+=;
  String &operator+=(const __FlashStringHelper *s) {
    append(reinterpret_cast<const char *>(s));
    return *this;
  }
  String &operator=(const __FlashStringHelper *s) {
    assign(reinterpret_cast<const char *>(s));
    return *this;
  }
  template <class T, std::enable_if_t<std::is_integral<T>::value &&
                                          !std::is_same<T, char>::value,
                                      int> = 0>
  String &operator+=(T n) {
    append(String(n));
    return *this;
  }
  String &operator=(const char *s) {
    std::string::operator=(s ? s : "");
    return *this;
  }
  char operator[](size_t i) const {
    return i < size() ? std::string::operator[](i) : '\0';
  }
  char &operator[](size_t i) {
    static thread_local char zero;
    if (i >= size()) {
      zero = 0;
      return zero;
    }
    return std::string::operator[](i);
  }
  bool isEmpty() const { return empty(); }
  explicit operator bool() const { return true; }
  bool reserve(size_t n) {
    try {
      std::string::reserve(n);
      return true;
    } catch (...) {
      return false;
    }
  }
  bool concat(const char *s, size_t n) {
    if (!s && n)
      return false;
    append(s ? s : "", n);
    return true;
  }
  bool concat(const String &s) {
    append(s);
    return true;
  }
  bool concat(const char *s) {
    if (!s)
      return false;
    append(s);
    return true;
  }
  bool concat(char c) {
    push_back(c);
    return true;
  }
  bool equals(const String &s) const { return compare(s) == 0; }
  bool equalsConstantTime(const String &s) const {
    if (size() != s.size())
      return false;
    volatile unsigned char difference = 0;
    for (size_t i = 0; i < size(); i++)
      difference = static_cast<unsigned char>(
          difference | (static_cast<unsigned char>((*this)[i]) ^
                        static_cast<unsigned char>(s[i])));
    return difference == 0;
  }
  bool equalsIgnoreCase(const String &s) const {
    return size() == s.size() &&
           std::equal(begin(), end(), s.begin(),
                      [](unsigned char a, unsigned char b) {
                        return std::tolower(a) == std::tolower(b);
                      });
  }
  bool startsWith(const String &s, size_t offset = 0) const {
    return offset <= size() && compare(offset, s.size(), s) == 0;
  }
  bool endsWith(const String &s) const {
    return size() >= s.size() && compare(size() - s.size(), s.size(), s) == 0;
  }
  int indexOf(char c, size_t from = 0) const {
    auto n = find(c, from);
    return n == npos ? -1 : int(n);
  }
  int indexOf(const String &s, size_t from = 0) const {
    auto n = find(s, from);
    return n == npos ? -1 : int(n);
  }
  int lastIndexOf(char c) const {
    auto n = rfind(c);
    return n == npos ? -1 : int(n);
  }
  int lastIndexOf(const String &s) const {
    auto n = rfind(s);
    return n == npos ? -1 : int(n);
  }
  String substring(size_t from, size_t to = std::string::npos) const {
    if (to != npos && from > to)
      std::swap(from, to);
    from = std::min(from, size());
    to = std::min(to, size());
    return substr(from, to - from);
  }
  void remove(size_t from, size_t n = std::string::npos) {
    if (from < size())
      erase(from, n);
  }
  void replace(const String &old, const String &value) {
    if (old.empty())
      return;
    for (size_t pos = 0; (pos = find(old, pos)) != npos; pos += value.size())
      std::string::replace(pos, old.size(), value);
  }
  void replace(char a, char b) { std::replace(begin(), end(), a, b); }
  void trim() {
    auto first = find_first_not_of(" \t\r\n\v\f");
    if (first == npos) {
      clear();
      return;
    }
    erase(find_last_not_of(" \t\r\n\v\f") + 1);
    erase(0, first);
  }
  void toLowerCase() {
    for (char &c : *this)
      c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  void toUpperCase() {
    for (char &c : *this)
      c = char(std::toupper(static_cast<unsigned char>(c)));
  }
  long toInt() const { return std::strtol(c_str(), nullptr, 10); }
  float toFloat() const { return std::strtof(c_str(), nullptr); }
  double toDouble() const { return std::strtod(c_str(), nullptr); }
  char charAt(size_t n) const { return (*this)[n]; }
  void setCharAt(size_t n, char c) {
    if (n < size())
      std::string::operator[](n) = c;
  }
  void getBytes(unsigned char *out, size_t n, size_t from = 0) const {
    if (!out || !n)
      return;
    size_t count = from < size() ? std::min(n - 1, size() - from) : 0;
    if (count)
      std::memcpy(out, data() + from, count);
    out[count] = 0;
  }
  void toCharArray(char *out, size_t n, size_t from = 0) const {
    getBytes(reinterpret_cast<unsigned char *>(out), n, from);
  }
};

inline String operator+(const String &a, const String &b) {
  return std::string(a) + std::string(b);
}
inline String operator+(const String &a, const char *b) {
  return std::string(a) + (b ? b : "");
}
inline String operator+(const char *a, const String &b) {
  return std::string(a ? a : "") + std::string(b);
}
inline String operator+(const String &a, char b) { return std::string(a) + b; }
inline String operator+(char a, const String &b) { return a + std::string(b); }
inline bool operator==(const String &a, const __FlashStringHelper *b) {
  return a.compare(reinterpret_cast<const char *>(b)) == 0;
}
inline bool operator==(const __FlashStringHelper *a, const String &b) {
  return b == a;
}
inline bool operator!=(const String &a, const __FlashStringHelper *b) {
  return !(a == b);
}
inline bool operator!=(const __FlashStringHelper *a, const String &b) {
  return !(b == a);
}
template <class T, std::enable_if_t<std::is_integral<T>::value &&
                                        !std::is_same<T, char>::value,
                                    int> = 0>
String operator+(const String &a, T n) {
  return a + String(n);
}

inline const String emptyString;
