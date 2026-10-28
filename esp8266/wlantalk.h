#ifndef WLANTALK_H
#define WLANTALK_H

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// MyProtcol: Text based talking protocol between Pico and ESP8266.
// Format: :<total_length>:<command>:<data>\n (Total length includes the length
// of the entire message, including the colons and newline character)
//  Commands:
// - ECHO: Echoes back the data sent to the ESP8266.
//         ex) :15:ECHO:Hello\n
// - SLAVE: Master initiates, slave role reponds with "MASTER". Data is the
// self-represented name. Comes first in the talk.
//         ex) :15:SLAVE:Pico\n -> Slave responds with :19:MASTER:ESP8266\n

struct wlan_talk_message {
  char command[7]; // 6 bytes for command + 1 byte for null terminator
  char data[99];   // Data buffer
  size_t data_len; // Length of the data
};

// Typedefs for UART getc and putc functions to be used in the talk protocol
typedef int (*wlan_uart_getc_func)(void *privdata);
typedef void (*wlan_uart_putc_func)(void *privdata, int c);

// Returns 0 on success, -1 for an invalid frame. getc_func blocks for a byte.
static inline int parse_wlan_talk_message(wlan_uart_getc_func getc_func,
                                          void *privdata,
                                          struct wlan_talk_message *msg) {
  if (!getc_func || !msg || getc_func(privdata) != ':')
    return -1;

  struct wlan_talk_message parsed = {0};
  const size_t max_length =
      1 + 3 + 1 + sizeof(parsed.command) - 1 + 1 + sizeof(parsed.data) - 1 + 1;
  size_t total_length = 0, digits = 0;
  int c;
  while ((c = getc_func(privdata)) != ':') {
    if (c < '0' || c > '9' || ++digits > 3 ||
        total_length > (max_length - (size_t)(c - '0')) / 10)
      return -1;
    total_length = total_length * 10 + (size_t)(c - '0');
  }
  if (!digits || total_length > max_length)
    return -1;

  size_t consumed = 2 + digits; // Opening colon, length digits, separator.
  size_t command_len = 0;
  while (consumed < total_length) {
    c = getc_func(privdata);
    consumed++;
    if (c == ':')
      break;
    if (c < '!' || c > '~' || command_len == sizeof(parsed.command) - 1)
      return -1;
    parsed.command[command_len++] = (char)c;
  }
  if (!command_len || c != ':' || consumed >= total_length)
    return -1;

  size_t data_len = total_length - consumed - 1; // Final newline.
  if (data_len >= sizeof(parsed.data))
    return -1;
  for (size_t i = 0; i < data_len; i++) {
    c = getc_func(privdata);
    if (c < ' ' || c > '~')
      return -1;
    parsed.data[i] = (char)c;
  }
  if (getc_func(privdata) != '\n')
    return -1;

  parsed.data_len = data_len;
  *msg = parsed;

  return 0;
}

// Emits one complete frame; returns its byte count or -1 for invalid input.
static inline int make_wlan_talk_message(wlan_uart_putc_func putc_func,
                                         void *privdata, const char *command,
                                         const char *data) {
  if (!putc_func || !command || !data)
    return -1;

  struct wlan_talk_message bounds;
  assert(strlen(command) < sizeof(bounds.command));
  assert(strlen(data) < sizeof(bounds.data));

  size_t command_len = 0, data_len = 0;
  while (command_len < sizeof(bounds.command) && command[command_len]) {
    int c = (unsigned char)command[command_len];
    if (c < '!' || c > '~' || c == ':')
      return -1;
    command_len++;
  }
  if (!command_len || command_len == sizeof(bounds.command))
    return -1;
  while (data_len < sizeof(bounds.data) && data[data_len]) {
    int c = (unsigned char)data[data_len];
    if (c < ' ' || c > '~')
      return -1;
    data_len++;
  }
  if (data_len == sizeof(bounds.data))
    return -1;

  size_t base_length = command_len + data_len + 4;
  size_t digits = 1, total_length;
  for (;;) {
    total_length = base_length + digits;
    size_t needed = 1;
    for (size_t n = total_length; n >= 10; n /= 10)
      needed++;
    if (digits == needed)
      break;
    digits = needed;
  }

  char length_digits[3];
  size_t n = total_length;
  for (size_t i = digits; i > 0; i--) {
    length_digits[i - 1] = (char)('0' + n % 10);
    n /= 10;
  }
  putc_func(privdata, ':');
  for (size_t i = 0; i < digits; i++)
    putc_func(privdata, length_digits[i]);
  putc_func(privdata, ':');
  for (size_t i = 0; i < command_len; i++)
    putc_func(privdata, command[i]);
  putc_func(privdata, ':');
  for (size_t i = 0; i < data_len; i++)
    putc_func(privdata, data[i]);
  putc_func(privdata, '\n');
  return (int)total_length;
}

#endif // WLANTALK_H