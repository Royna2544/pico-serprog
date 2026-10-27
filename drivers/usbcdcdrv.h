#include "core/driver.h"

#include <stdint.h>

struct usb_gadget_driver {
  struct driver drv;
};

struct usb_gadget_device {
  struct device dev;
};

typedef struct usb_cdc_line_coding {
  uint32_t bit_rate;
  uint8_t stop_bits; ///< 0: 1 stop bit - 1: 1.5 stop bits - 2: 2 stop bits
  uint8_t parity;    ///< 0: None - 1: Odd - 2: Even - 3: Mark - 4: Space
  uint8_t data_bits; ///< can be 5, 6, 7, 8 or 16
} usb_cdc_line_coding_t;

typedef void (*usb_cdc_io_callback_t)(void *);
typedef void (*usb_cdc_encoding_changed_callback_t)(
    usb_cdc_line_coding_t const *, void *);

struct usb_cdc_driver {
  struct driver drv;
  uint32_t (*read_available)(const struct device *);
  uint32_t (*write_available)(const struct device *);

  // Read and write functions for the USB CDC device
  uint32_t (*read)(const struct device *, void *, uint32_t, bool blocking);
  uint32_t (*write)(const struct device *, const void *, uint32_t,
                    bool blocking);

  // Flush functions for the USB CDC device
  // Important: Read flush clears the read buffer, while write flush ensures all
  // data is sent
  void (*read_flush)(const struct device *);
  void (*write_flush)(const struct device *);

  int (*register_io_callbacks)(const struct device *, usb_cdc_io_callback_t,
                               usb_cdc_io_callback_t,
                               usb_cdc_encoding_changed_callback_t, void *);
  void (*clear_io_callbacks)(const struct device *);
};

uint32_t usb_cdc_read_bytes(const struct device *dev, void *buf, uint32_t len);
uint32_t usb_cdc_write_bytes(const struct device *dev, const void *buf,
                             uint32_t len);
uint32_t usb_cdc_read_byte(const struct device *dev, uint8_t *byte);
uint32_t usb_cdc_write_byte(const struct device *dev, uint8_t byte);

uint32_t usb_cdc_nonblocking_read_bytes(const struct device *dev, void *buf,
                                        uint32_t len);
uint32_t usb_cdc_nonblocking_write_bytes(const struct device *dev,
                                         const void *buf, uint32_t len);
uint32_t usb_cdc_nonblocking_read_byte(const struct device *dev, uint8_t *byte);
uint32_t usb_cdc_nonblocking_write_byte(const struct device *dev, uint8_t byte);

uint32_t usb_cdc_read_available(const struct device *dev);
uint32_t usb_cdc_write_available(const struct device *dev);
void usb_cdc_read_flush(const struct device *dev);
void usb_cdc_write_flush(const struct device *dev);
