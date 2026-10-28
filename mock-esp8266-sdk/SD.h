#pragma once

#include "FS.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

inline struct SDCard {
  static const std::filesystem::path get_root_path() {
    return "/home/royna/Desktop/poorthing/pico-serprog/mock-esp8266-sdk/sdcard";
  }
  bool begin(int) { return 1; }
  bool remove(const char *path) {
    std::error_code ec;
    std::filesystem::remove(get_root_path() / path, ec);
    if (ec) {
      return 0;
    }
    return 1;
  }
#define FILE_READ "r"
#define FILE_WRITE "w"

  fs::File open(std::filesystem::path path, const char *mode) {
    if (path.is_absolute()) {
      path = path.lexically_relative("/");
    }
    path = std::filesystem::weakly_canonical(get_root_path() / path);
    printf("SDCard::open(%s, %s)\n", path.c_str(), mode);
    FILE *file = std::fopen(path.c_str(), mode);
    return file ? fs::File(file, String(path.c_str())) : fs::File();
  }

} SD;
