#include "MD5Builder.h"
#include "base64.h"
#include <array>
#include <openssl/evp.h>
#include <stdexcept>
struct MD5Builder::State {
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  bool finished = false;
  ~State() { EVP_MD_CTX_free(context); }
};
MD5Builder::MD5Builder() : state_(std::make_unique<State>()) { begin(); }
MD5Builder::~MD5Builder() = default;
void MD5Builder::begin() {
  if (!state_->context ||
      EVP_DigestInit_ex(state_->context, EVP_md5(), nullptr) != 1)
    throw std::runtime_error("MD5 initialization failed");
  state_->finished = false;
  digest_.fill(0);
}
void MD5Builder::add(const uint8_t *data, size_t n) {
  if (state_->finished || (!data && n) ||
      EVP_DigestUpdate(state_->context, data, n) != 1)
    throw std::runtime_error("MD5 update failed");
}
bool MD5Builder::addStream(Stream &source, size_t n) {
  std::array<uint8_t, 4096> data{};
  while (n) {
    size_t count = source.readBytes(data.data(), std::min(n, data.size()));
    if (!count)
      return false;
    add(data.data(), count);
    n -= count;
  }
  return true;
}
void MD5Builder::calculate() {
  if (state_->finished)
    return;
  unsigned size = 0;
  if (EVP_DigestFinal_ex(state_->context, digest_.data(), &size) != 1 ||
      size != 16)
    throw std::runtime_error("MD5 finalization failed");
  state_->finished = true;
}
String MD5Builder::toString() const {
  String out;
  out.reserve(32);
  for (uint8_t c : digest_) {
    out += "0123456789abcdef"[c >> 4];
    out += "0123456789abcdef"[c & 15];
  }
  return out;
}
String base64::encode(const uint8_t *data, size_t n, bool newline) {
  if ((!data && n) || n > size_t(INT32_MAX / 4 * 3 - 3))
    return {};
  String out;
  out.resize(4 * ((n + 2) / 3));
  if (n)
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()), data,
                    int(n));
  if (!newline)
    return out;
  String wrapped;
  for (size_t i = 0; i < out.size(); i += 72) {
    wrapped += out.substring(i, i + 72);
    wrapped += '\n';
  }
  return wrapped;
}
