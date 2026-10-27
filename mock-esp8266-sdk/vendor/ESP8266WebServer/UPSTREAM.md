# ESP8266 Arduino HTTP engine

Source: https://github.com/esp8266/Arduino/tree/3.1.2/libraries/ESP8266WebServer/src
Revision: 210897ef83305496947c4e73c937bab52a33cb48 (3.1.2).
The original copyright notices and LGPL 2.1 license are retained.

Host adaptations: the unused libb64/cencode.h include is replaced with
MD5Builder.h; destruction releases the final request's argument arrays;
UriRegex compiles and validates its expression even with NDEBUG enabled.
Trailing whitespace in source files is normalized.
POSIX transports, Arduino streams, rooted filesystem, and OpenSSL-backed
base64/MD5 adapters live outside this directory.
