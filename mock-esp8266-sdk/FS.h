#pragma once
#include "Stream.h"
#include <cstdio>
#include <filesystem>
#include <memory>
struct SDCard;

namespace fs {
enum SeekMode { SeekSet, SeekCur, SeekEnd };
class FS;
class File : public Stream {
  std::shared_ptr<FILE> file_;
  String name_;
  File(FILE *file, String name)
      : file_(file,
              [](FILE *p) {
                if (p)
                  std::fclose(p);
              }),
        name_(std::move(name)) {}
  friend class FS;
  friend struct ::SDCard;

public:
  File() = default;
  operator bool() const { return bool(file_); }
  int available() override;
  int read() override;
  int read(uint8_t *data, size_t n) override;
  using Stream::read;
  int peek() override;
  using Print::write;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *data, size_t n) override;
  void flush() override;
  void close() { file_.reset(); }
  size_t size() const;
  size_t position() const;
  bool seek(uint32_t offset, SeekMode mode = SeekSet);
  const char *name() const { return name_.c_str(); }
  const char *fullName() const { return name(); }
  bool isFile() const;
  bool isDirectory() const;
  bool inputCanTimeout() override { return false; }
  ssize_t streamRemaining() override;
};
class FS {
  std::shared_ptr<const std::filesystem::path> root_;
  std::filesystem::path resolve(const char *path) const;

public:
  FS() = default;
  explicit FS(const char *root);
  explicit FS(const String &root) : FS(root.c_str()) {}
  bool begin() const { return bool(root_); }
  void end() { root_.reset(); }
  File open(const char *path, const char *mode = "r") const;
  File open(const String &path, const char *mode = "r") const {
    return open(path.c_str(), mode);
  }
  bool exists(const char *path) const;
  bool exists(const String &path) const { return exists(path.c_str()); }
  bool remove(const char *path);
  bool mkdir(const char *path);
  bool rmdir(const char *path) { return remove(path); }
  bool rename(const char *from, const char *to);
};
} // namespace fs
