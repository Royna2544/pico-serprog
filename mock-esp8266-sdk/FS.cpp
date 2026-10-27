#include "FS.h"
#include <cerrno>
#include <sys/stat.h>
namespace fs {
int File::available() {
  return int(std::min(size_t(INT32_MAX),
                      size() > position() ? size() - position() : 0));
}
int File::read() { return file_ ? std::fgetc(file_.get()) : -1; }
int File::read(uint8_t *data, size_t n) {
  if (!file_ || (!data && n))
    return -1;
  return int(std::fread(data, 1, n, file_.get()));
}
int File::peek() {
  if (!file_)
    return -1;
  int c = std::fgetc(file_.get());
  if (c != EOF)
    std::ungetc(c, file_.get());
  return c;
}
size_t File::write(uint8_t c) { return write(&c, 1); }
size_t File::write(const uint8_t *data, size_t n) {
  if (!file_ || (!data && n))
    return 0;
  return std::fwrite(data, 1, n, file_.get());
}
void File::flush() {
  if (file_)
    std::fflush(file_.get());
}
size_t File::size() const {
  struct stat s{};
  return file_ && fstat(fileno(file_.get()), &s) == 0
             ? size_t(std::max<off_t>(0, s.st_size))
             : 0;
}
size_t File::position() const {
  long n = file_ ? std::ftell(file_.get()) : -1;
  return n < 0 ? 0 : size_t(n);
}
bool File::seek(uint32_t offset, SeekMode mode) {
  return file_ && std::fseek(file_.get(), offset,
                             mode == SeekSet   ? SEEK_SET
                             : mode == SeekCur ? SEEK_CUR
                                               : SEEK_END) == 0;
}
bool File::isFile() const {
  struct stat s{};
  return file_ && fstat(fileno(file_.get()), &s) == 0 && S_ISREG(s.st_mode);
}
bool File::isDirectory() const {
  struct stat s{};
  return file_ && fstat(fileno(file_.get()), &s) == 0 && S_ISDIR(s.st_mode);
}
ssize_t File::streamRemaining() { return available(); }
FS::FS(const char *root) {
  if (!root)
    return;
  std::error_code error;
  auto path = std::filesystem::canonical(root, error);
  if (!error && std::filesystem::is_directory(path, error))
    root_ = std::make_shared<const std::filesystem::path>(std::move(path));
}
std::filesystem::path FS::resolve(const char *path) const {
  if (!root_ || !path) {
    errno = EINVAL;
    return {};
  }
  std::error_code error;
  auto candidate = std::filesystem::weakly_canonical(
      *root_ / std::filesystem::path(path).relative_path(), error);
  if (error) {
    errno = error.value();
    return {};
  }
  auto a = root_->begin(), b = candidate.begin();
  for (; a != root_->end(); ++a, ++b)
    if (b == candidate.end() || *a != *b) {
      errno = EACCES;
      return {};
    }
  return candidate;
}
File FS::open(const char *path, const char *mode) const {
  auto name = resolve(path);
  if (name.empty() || !mode)
    return {};
  FILE *file = std::fopen(name.c_str(), mode);
  return file ? File(file, String(path)) : File();
}
bool FS::exists(const char *path) const {
  auto p = resolve(path);
  std::error_code e;
  return !p.empty() && std::filesystem::exists(p, e);
}
bool FS::remove(const char *path) {
  auto p = resolve(path);
  std::error_code e;
  return !p.empty() && p != *root_ && std::filesystem::remove(p, e);
}
bool FS::mkdir(const char *path) {
  auto p = resolve(path);
  std::error_code e;
  return !p.empty() && std::filesystem::create_directory(p, e);
}
bool FS::rename(const char *from, const char *to) {
  auto a = resolve(from), b = resolve(to);
  if (a.empty() || b.empty() || a == *root_)
    return false;
  std::error_code e;
  std::filesystem::rename(a, b, e);
  return !e;
}
} // namespace fs
