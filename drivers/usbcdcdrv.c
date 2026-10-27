#include "usbcdcdrv.h"
#include "core/devicetree.h"
#include "tusb.h"
#include <pico/mutex.h>
#include <pico/stdio/driver.h>
#include <stdint.h>
#include <sys/errno.h>
#include <sys/types.h>

#define USB_CDC_WAIT_TIMEOUT_MS 10

static mutex_t usb_cdc_task_mutex;

__attribute__((constructor)) static void init_usb_cdc_task_mutex() {
  mutex_init(&usb_cdc_task_mutex);
}

static void safe_cdc_tud_task_timeout(uint32_t timeout_ms) {
  if (!mutex_try_enter(&usb_cdc_task_mutex, NULL)) {
    // If we can't acquire the mutex, it means another thread is already
    // running tud_task(). We can safely skip this call to avoid deadlock.
    return;
  }
  tud_task_ext(timeout_ms, false);
  mutex_exit(&usb_cdc_task_mutex);
}

static void safe_cdc_tud_task(void) {
  safe_cdc_tud_task_timeout(0);
}

static int usb_gadget_probe(struct device *dev) {
  tusb_init();
  return 0;
}

static void usb_gadget_remove(struct device *dev) {
  (void)dev; // Unused parameter
}

static const struct driver_fdt_match_id usb_gadget_match[] = {
    {.compatible = "royna,rp2040-usb-gadget", .data = NULL}, {}};

static struct usb_gadget_driver usb_gadget_drv = {
    .drv =
        {
            .name = "usb-gadget-drv",
            .probe = usb_gadget_probe,
            .remove = usb_gadget_remove,
            .match_table = usb_gadget_match,
        },
};

struct usb_cdc_driver_pdata {
  struct stdio_driver stdio_driver;
  struct device *cdc_dev;
  void (*chars_available_callback)(void *);
  void *chars_available_callback_param;
  bool stdio_enabled;

  usb_cdc_io_callback_t user_read_available;
  usb_cdc_io_callback_t user_write_done;
  usb_cdc_encoding_changed_callback_t user_line_coding;
  void *user_callback_param;
};

static void usb_cdc_out_chars(stdio_driver_t *self, const char *buf, int len) {
  struct usb_cdc_driver_pdata *pdata =
      container_of(self, struct usb_cdc_driver_pdata, stdio_driver);
  usb_cdc_nonblocking_write_bytes(pdata->cdc_dev, buf, len);
  safe_cdc_tud_task();
}

static void usb_cdc_out_flush(stdio_driver_t *self) {
  struct usb_cdc_driver_pdata *pdata =
      container_of(self, struct usb_cdc_driver_pdata, stdio_driver);
  usb_cdc_write_flush(pdata->cdc_dev);
  safe_cdc_tud_task();
}

#include "tusb_config.h"

static struct usb_cdc_io_complete_notification {
  void (*read_available)(void *);
  void (*write_done)(void *);
  void (*encoding_changed)(usb_cdc_line_coding_t const *coding, void *);
  void *param;
  mutex_t mutex;
} notification[CFG_TUD_CDC] = {}; // cdc interfaces

void tud_cdc_rx_cb(uint8_t itf) {
  mutex_enter_blocking(&notification[itf].mutex);
  if (notification[itf].read_available) {
    notification[itf].read_available(notification[itf].param);
  }
  mutex_exit(&notification[itf].mutex);
}

void tud_cdc_tx_complete_cb(uint8_t itf) {
  mutex_enter_blocking(&notification[itf].mutex);
  if (notification[itf].write_done) {
    notification[itf].write_done(notification[itf].param);
  }
  mutex_exit(&notification[itf].mutex);
}

static void usb_cdc_set_io_complete_callback(
    const struct device *dev, void (*read_available)(void *),
    void (*write_done)(void *),
    void (*encoding_changed)(usb_cdc_line_coding_t const *coding, void *),
    void *param) {
  if (dev->id < CFG_TUD_CDC) {
    mutex_enter_blocking(&notification[dev->id].mutex);
    notification[dev->id].read_available = read_available;
    notification[dev->id].write_done = write_done;
    notification[dev->id].encoding_changed = encoding_changed;
    notification[dev->id].param = param;
    mutex_exit(&notification[dev->id].mutex);
  }
}

static void usb_cdc_clear_io_complete_callback(const struct device *dev) {
  if (dev->id < CFG_TUD_CDC) {
    mutex_enter_blocking(&notification[dev->id].mutex);
    notification[dev->id].read_available = NULL;
    notification[dev->id].write_done = NULL;
    notification[dev->id].encoding_changed = NULL;
    notification[dev->id].param = NULL;
    mutex_exit(&notification[dev->id].mutex);
  }
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding) {
  // Handle line coding change for CDC interface 'itf'
  pr_info("CDC line coding changed on interface %d: baud=%u, stop_bits=%d, "
          "parity=%d, data_bits=%d",
          itf, coding->bit_rate, coding->stop_bits, coding->parity,
          coding->data_bits);
  if (itf < CFG_TUD_CDC) {
    struct usb_cdc_line_coding line_coding = {
        .bit_rate = coding->bit_rate,
        .stop_bits = coding->stop_bits,
        .parity = coding->parity,
        .data_bits = coding->data_bits,
    };
    mutex_enter_blocking(&notification[itf].mutex);
    if (notification[itf].encoding_changed) {
      notification[itf].encoding_changed(&line_coding, notification[itf].param);
    }
    mutex_exit(&notification[itf].mutex);
  }
}

int usb_cdc_in_chars(stdio_driver_t *self, char *buf, int len) {
  struct usb_cdc_driver_pdata *pdata =
      container_of(self, struct usb_cdc_driver_pdata, stdio_driver);
  safe_cdc_tud_task();
  return usb_cdc_nonblocking_read_bytes(pdata->cdc_dev, buf, len);
}

void usb_cdc_set_chars_available_callback(stdio_driver_t *self,
                                          void (*fn)(void *), void *param) {
  struct usb_cdc_driver_pdata *pdata =
      container_of(self, struct usb_cdc_driver_pdata, stdio_driver);
  pdata->chars_available_callback = fn;
  pdata->chars_available_callback_param = param;
}

static void usb_cdc_callback_for_set_chars(void *data) {
  struct usb_cdc_driver_pdata *pdata = (struct usb_cdc_driver_pdata *)data;
  if (pdata->chars_available_callback) {
    pdata->chars_available_callback(pdata->chars_available_callback_param);
  }
  safe_cdc_tud_task();
  if (pdata->user_read_available) {
    pdata->user_read_available(pdata->user_callback_param);
  }
}

static void usb_cdc_callback_for_read_available(void *data) {
  struct usb_cdc_driver_pdata *pdata = (struct usb_cdc_driver_pdata *)data;
  safe_cdc_tud_task();
  if (pdata->user_read_available) {
    pdata->user_read_available(pdata->user_callback_param);
  }
}
static void usb_cdc_callback_for_write_done(void *data) {
  struct usb_cdc_driver_pdata *pdata = (struct usb_cdc_driver_pdata *)data;
  safe_cdc_tud_task();
  if (pdata->user_write_done) {
    pdata->user_write_done(pdata->user_callback_param);
  }
}

static void
usb_cdc_callback_for_encoding_changed(usb_cdc_line_coding_t const *coding,
                                      void *data) {
  struct usb_cdc_driver_pdata *pdata = (struct usb_cdc_driver_pdata *)data;
  if (pdata->user_line_coding) {
    pdata->user_line_coding(coding, pdata->user_callback_param);
  }
  safe_cdc_tud_task();
}

static int usb_cdc_probe(struct device *dev) {
  struct reg reg_info;
  if (get_reg(dev, NULL, &reg_info)) {
    return -EINVAL;
  }
  if (reg_info.base < 0 || reg_info.base >= CFG_TUD_CDC) {
    dev_err(dev, "Invalid USB CDC interface number %d", reg_info.base);
    return -EINVAL;
  }
  dev->id = reg_info.base;
  mutex_init(&notification[dev->id].mutex);

  struct usb_cdc_driver_pdata *pdata = calloc(1, sizeof(*pdata));
  if (!pdata) {
    dev_err(dev, "Failed to allocate memory for USB CDC driver pdata");
    return -ENOMEM;
  }
  pdata->cdc_dev = dev;
  dev->platform_data = pdata;

  dev_dbg(dev, "Probing USB CDC device with id %d", dev->id);
  if (has_prop(dev, "royna,stdio")) {
    struct stdio_driver cdc_stdio_driver = {
        .out_chars = usb_cdc_out_chars,
        .out_flush = usb_cdc_out_flush,
        .in_chars = usb_cdc_in_chars,
        .set_chars_available_callback = usb_cdc_set_chars_available_callback,
    };
    pdata->stdio_enabled = true;
    usb_cdc_set_io_complete_callback(
        dev, usb_cdc_callback_for_set_chars, usb_cdc_callback_for_write_done,
        usb_cdc_callback_for_encoding_changed, pdata);
    pdata->stdio_driver = cdc_stdio_driver;
    stdio_set_driver_enabled(&pdata->stdio_driver, true);
    stdio_set_translate_crlf(&pdata->stdio_driver, true);
    stdio_filter_driver(NULL);

    dev_info(dev, "USB CDC device registered as stdio driver");
  } else {
    usb_cdc_set_io_complete_callback(
        dev, usb_cdc_callback_for_set_chars, usb_cdc_callback_for_write_done,
        usb_cdc_callback_for_encoding_changed, pdata);
  }
  safe_cdc_tud_task();

  return 0;
}

static void usb_cdc_remove(struct device *dev) {
  struct usb_cdc_driver_pdata *pdata = dev->platform_data;
  if (pdata) {
    usb_cdc_clear_io_complete_callback(dev);
    if (pdata->stdio_enabled) {
      stdio_set_driver_enabled(&pdata->stdio_driver, false);
    }
    free(pdata);
    dev->platform_data = NULL;
  }
}

static void wait_for_read(int intf) {
  while (!tud_cdc_n_available(intf)) {
    safe_cdc_tud_task_timeout(USB_CDC_WAIT_TIMEOUT_MS);
  }
}

static void wait_for_write(int intf) {
  while (!tud_cdc_n_write_available(intf)) {
    safe_cdc_tud_task_timeout(USB_CDC_WAIT_TIMEOUT_MS);
  }
}

static uint32_t usb_cdcdrv_read_available(const struct device *dev) {
  safe_cdc_tud_task();
  return tud_cdc_n_available(dev->id);
}

static uint32_t usb_cdcdrv_write_available(const struct device *dev) {
  safe_cdc_tud_task();
  return tud_cdc_n_write_available(dev->id);
}

static uint32_t usb_cdcdrv_read(const struct device *dev, void *buf,
                                uint32_t len, bool blocking) {
  uint32_t done = 0;
  while (done < len) {
    if (blocking)
      wait_for_read(dev->id);
    else {
      uint32_t available = tud_cdc_n_available(dev->id);
      if (available == 0) {
        break; // Non-blocking mode, exit if no data is available
      }
    }
    uint32_t available = tud_cdc_n_available(dev->id);
    uint32_t chunk = len - done < available ? len - done : available;
    if (chunk) {
      uint32_t n = tud_cdc_n_read(dev->id, (uint8_t *)buf + done, chunk);
      if (n <= chunk)
        done += n;
    }
  }
  safe_cdc_tud_task();
  return done;
}

static uint32_t usb_cdcdrv_write(const struct device *dev, const void *buf,
                                 uint32_t len, bool blocking) {
  uint32_t done = 0;
  while (done < len) {
    if (blocking)
      wait_for_write(dev->id);
    else {
      uint32_t available = tud_cdc_n_write_available(dev->id);
      if (available == 0) {
        break; // Non-blocking mode, exit if no space is available
      }
    }

    uint32_t available = tud_cdc_n_write_available(dev->id);
    uint32_t chunk = len - done < available ? len - done : available;
    if (chunk) {
      uint32_t n = tud_cdc_n_write(dev->id, (const uint8_t *)buf + done, chunk);
      if (n <= chunk)
        done += n;
    }
  }
  safe_cdc_tud_task();
  return done;
}

static void usb_cdcdrv_read_flush(const struct device *dev) {
  safe_cdc_tud_task();
  tud_cdc_n_read_flush(dev->id);
}

static void usb_cdcdrv_write_flush(const struct device *dev) {
  safe_cdc_tud_task();
  tud_cdc_n_write_flush(dev->id);
}

int usb_cdc_register_io_callbacks(
    const struct device *dev, usb_cdc_io_callback_t read_available,
    usb_cdc_io_callback_t write_done,
    usb_cdc_encoding_changed_callback_t encoding_changed, void *param) {
  struct usb_cdc_driver_pdata *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "USB CDC driver pdata is NULL");
    return -EINVAL;
  }
  pdata->user_read_available = read_available;
  pdata->user_write_done = write_done;
  pdata->user_line_coding = encoding_changed;
  pdata->user_callback_param = param;
  return 0;
}

void usb_cdc_clear_io_callbacks(const struct device *dev) {
  struct usb_cdc_driver_pdata *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "USB CDC driver pdata is NULL");
    return;
  }
  pdata->user_read_available = NULL;
  pdata->user_write_done = NULL;
  pdata->user_line_coding = NULL;
  pdata->user_callback_param = NULL;
}

static const struct driver_fdt_match_id usb_cdc_match[] = {
    {.compatible = "royna,rp2040-usb-cdc", .data = NULL}, {}};

static struct usb_cdc_driver usb_cdc_drv = {
    .drv =
        {
            .name = "usb-cdc-drv",
            .probe = usb_cdc_probe,
            .remove = usb_cdc_remove,
            .match_table = usb_cdc_match,
        },
    .read_available = usb_cdcdrv_read_available,
    .write_available = usb_cdcdrv_write_available,
    .read = usb_cdcdrv_read,
    .write = usb_cdcdrv_write,
    .read_flush = usb_cdcdrv_read_flush,
    .write_flush = usb_cdcdrv_write_flush,
    .register_io_callbacks = usb_cdc_register_io_callbacks,
    .clear_io_callbacks = usb_cdc_clear_io_callbacks,
};

PLATFORM_DRIVER(usb_gadget_drv);
PLATFORM_DRIVER(usb_cdc_drv);

uint32_t usb_cdc_read_bytes(const struct device *dev, void *buf, uint32_t len) {
  struct usb_cdc_driver *drv =
      container_of(dev->drv, struct usb_cdc_driver, drv);
  if (!drv || !drv->read) {
    dev_err(dev, "USB CDC driver does not support read");
    return -ENOSYS;
  }
  return drv->read(dev, buf, len, true);
}

uint32_t usb_cdc_nonblocking_read_bytes(const struct device *dev, void *buf,
                                        uint32_t len) {
  struct usb_cdc_driver *drv =
      container_of(dev->drv, struct usb_cdc_driver, drv);
  if (!drv || !drv->read) {
    dev_err(dev, "USB CDC driver does not support read");
    return -ENOSYS;
  }
  return drv->read(dev, buf, len, false);
}

uint32_t usb_cdc_nonblocking_write_bytes(const struct device *dev,
                                         const void *buf, uint32_t len) {
  struct usb_cdc_driver *drv =
      container_of(dev->drv, struct usb_cdc_driver, drv);
  if (!drv || !drv->write) {
    dev_err(dev, "USB CDC driver does not support write");
    return -ENOSYS;
  }
  return drv->write(dev, buf, len, false);
}

uint32_t usb_cdc_write_bytes(const struct device *dev, const void *buf,
                             uint32_t len) {
  struct usb_cdc_driver *drv =
      container_of(dev->drv, struct usb_cdc_driver, drv);
  if (!drv || !drv->write) {
    dev_err(dev, "USB CDC driver does not support write");
    return -ENOSYS;
  }
  return drv->write(dev, buf, len, true);
}

uint32_t usb_cdc_read_available(const struct device *dev) {
  struct usb_cdc_driver *drv =
      container_of(dev->drv, struct usb_cdc_driver, drv);
  if (!drv || !drv->read_available) {
    dev_err(dev, "USB CDC driver does not support read_available");
    return -ENOSYS;
  }
  return drv->read_available(dev);
}

uint32_t usb_cdc_write_available(const struct device *dev) {
  struct usb_cdc_driver *drv =
      container_of(dev->drv, struct usb_cdc_driver, drv);
  if (!drv || !drv->write_available) {
    dev_err(dev, "USB CDC driver does not support write_available");
    return -ENOSYS;
  }
  return drv->write_available(dev);
}

void usb_cdc_read_flush(const struct device *dev) {
  struct usb_cdc_driver *drv =
      container_of(dev->drv, struct usb_cdc_driver, drv);
  if (!drv || !drv->read_flush) {
    dev_err(dev, "USB CDC driver does not support read_flush");
    return;
  }
  drv->read_flush(dev);
}

void usb_cdc_write_flush(const struct device *dev) {
  struct usb_cdc_driver *drv =
      container_of(dev->drv, struct usb_cdc_driver, drv);
  if (!drv || !drv->write_flush) {
    dev_err(dev, "USB CDC driver does not support write_flush");
    return;
  }
  drv->write_flush(dev);
}

uint32_t usb_cdc_read_byte(const struct device *dev, uint8_t *byte) {
  return usb_cdc_read_bytes(dev, byte, 1);
}
uint32_t usb_cdc_write_byte(const struct device *dev, uint8_t byte) {
  return usb_cdc_write_bytes(dev, &byte, 1);
}
