#pragma once
#include <cstring>
#define PROGMEM
#define PGM_P const char *
#define PSTR(x) (x)
#define FPSTR(x) (reinterpret_cast<const __FlashStringHelper *>(x))
#define F(x) FPSTR(PSTR(x))
#define strlen_P std::strlen
#define memcpy_P std::memcpy
#define strcpy_P std::strcpy
class __FlashStringHelper;
