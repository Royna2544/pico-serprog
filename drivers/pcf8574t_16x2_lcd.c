
#include "pcf8574t_16x2_lcd.h"
#include "core/devicetree.h"
#include "i2cdrv.h"
#include "pico/stdlib.h"
#include <errno.h>
#include <stdint.h>

#define LCD_CMD_CLEAR_DISPLAY 0x01
#define LCD_CMD_RETURN_HOME 0x02
#define LCD_CMD_CURSOR_LEFT 0x10
#define LCD_CMD_CURSOR_RIGHT 0x14
#define LCD_CMD_SHIFT_DISPLAY_LEFT 0x18
#define LCD_CMD_SHIFT_DISPLAY_RIGHT 0x1C

// Entry mode set variants
#define LCD_CMD_ENTRY_DECREMENT_SET_NO_SHIFT 0x04
#define LCD_CMD_ENTRY_DECREMENT_SET_SHIFT 0x05
#define LCD_CMD_ENTRY_INCREMENT_SET_NO_SHIFT 0x06
#define LCD_CMD_ENTRY_INCREMENT_SET_SHIFT 0x07

// Display control
#define LCD_CMD_DISPLAY_OFF 0x08
#define LCD_CMD_DISPLAY_ON_CURSOR_OFF 0x0C
#define LCD_CMD_DISPLAY_ON_CURSOR_ON 0x0E
#define LCD_CMD_DISPLAY_ON_CURSOR_BLINK 0x0F
#define LCD_CMD_DISPLAY_ON_CURSOR_OFF_BLINK 0x0D

// Function set variants
#define LCD_CMD_FUNCTION_SET_4BIT_1LINE_5x8 0x20
#define LCD_CMD_FUNCTION_SET_4BIT_1LINE_5x10 0x24
#define LCD_CMD_FUNCTION_SET_8BIT_1LINE_5x8 0x30
#define LCD_CMD_FUNCTION_SET_8BIT_2LINE_5x8 0x38
#define LCD_CMD_FUNCTION_SET_4BIT_2LINE_5x8 0x28

// Memory addressing (base commands)
#define LCD_CMD_SET_CGRAM_ADDRESS 0x40
#define LCD_CMD_SET_DDRAM_ADDRESS 0x80

// DDRAM row starting positions (16x2)
#define LCD_DDRAM_ROW_1 0x00
#define LCD_DDRAM_ROW_2 0x40

struct backpack_byte {
  uint8_t rs : 1;
  uint8_t rw : 1;
  uint8_t e : 1;
  uint8_t backlight : 1;
  uint8_t data : 4;
};

struct pcf8574t_16x2_lcd_pdata {
  struct device *i2c_dev;
};

static uint8_t backpack_byte_to_uint8(struct backpack_byte *bb) {
  return (bb->rs & 0x01) | ((bb->rw & 0x01) << 1) | ((bb->e & 0x01) << 2) |
         ((bb->backlight & 0x01) << 3) | ((bb->data & 0x0F) << 4);
}

static int lcd_send_upper(struct device *i2c_dev, struct backpack_byte *bb,
                          uint8_t data) {
  int rc = 0;
  // Send the upper nibble first
  bb->e = 0;                     // Enable low
  bb->data = (data >> 4) & 0x0F; // Send upper nibble
  rc = i2c_write_byte(i2c_dev, backpack_byte_to_uint8(bb));
  if (rc < 0) {
    return -1;
  }
  dev_dbg(i2c_dev, "Sent upper nibble: en=0 0x%02X", bb->data);
  bb->e = 1; // Enable high
  rc = i2c_write_byte(i2c_dev, backpack_byte_to_uint8(bb));
  if (rc < 0) {
    return -1;
  }
  dev_dbg(i2c_dev, "Sent upper nibble: en=1 0x%02X", bb->data);
  bb->e = 0; // Enable low again
  rc = i2c_write_byte(i2c_dev, backpack_byte_to_uint8(bb));
  if (rc < 0) {
    return -1;
  }
  dev_dbg(i2c_dev, "Sent upper nibble: en=0 0x%02X", bb->data);
  return 0;
}

static int lcd_send_lower(struct device *i2c_dev, struct backpack_byte *bb,
                          uint8_t data) {
  int rc = 0;
  // Now send the low nibble
  bb->e = 0;              // Enable high again for low nibble
  bb->data = data & 0x0F; // Send low nibble
  rc = i2c_write_byte(i2c_dev, backpack_byte_to_uint8(bb));
  if (rc < 0) {
    return -1;
  }
  dev_dbg(i2c_dev, "Sent low nibble: en=0 0x%02X", bb->data);
  bb->e = 1; // Enable high
  rc = i2c_write_byte(i2c_dev, backpack_byte_to_uint8(bb));
  if (rc < 0) {
    return -1;
  }
  dev_dbg(i2c_dev, "Sent low nibble: en=1 0x%02X", bb->data);
  bb->e = 0; // Enable low again
  rc = i2c_write_byte(i2c_dev, backpack_byte_to_uint8(bb));
  if (rc < 0) {
    return -1;
  }
  dev_dbg(i2c_dev, "Sent low nibble: en=0 0x%02X", bb->data);
  return 0;
}

static int lcd_send_all(struct device *i2c_dev, struct backpack_byte *bb,
                        uint8_t data) {
  int rc = 0;
  rc = lcd_send_upper(i2c_dev, bb, data);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send upper nibble of command 0x%02X", data);
    return rc;
  }
  rc = lcd_send_lower(i2c_dev, bb, data);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send lower nibble of command 0x%02X", data);
    return rc;
  }
  return 0;
}

static int lcd_send_command(struct device *i2c_dev, uint8_t command) {
  struct backpack_byte bb;
  bb.rs = 0; // Command mode
  bb.rw = 0; // Write mode
  int rc = lcd_send_all(i2c_dev, &bb, command);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send command 0x%02X", command);
    return rc;
  }
  return 0;
}

static int lcd_send_data(struct device *i2c_dev, uint8_t data) {
  struct backpack_byte bb;
  bb.rs = 1; // Data mode
  bb.rw = 0; // Write mode
  int rc = lcd_send_all(i2c_dev, &bb, data);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send data 0x%02X", data);
    return rc;
  }
  return 0;
}

static int lcd_write_string(const struct device *dev, const char *str) {
  struct pcf8574t_16x2_lcd_pdata *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  while (*str) {
    int rc = lcd_send_data(pdata->i2c_dev, (uint8_t)(*str));
    if (rc < 0) {
      dev_err(dev, "Failed to send character '%c': %d", *str, rc);
      return rc;
    }
    str++;
  }
  return 0;
}

static int lcd_init(struct device *i2c_dev) {
  dev_dbg(i2c_dev, "Initializing LCD");
  int rc = 0;

  sleep_ms(50); // Wait for LCD to power up
  rc = lcd_send_lower(i2c_dev, &(struct backpack_byte){.rs = 0, .rw = 0},
                      0x3); // Function set: 8-bit mode
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send initial function set command");
    goto fail_free;
  }
  sleep_ms(5); // Wait for more than 4.1ms
  rc = lcd_send_lower(i2c_dev, &(struct backpack_byte){.rs = 0, .rw = 0}, 0x3);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send second function set command");
    goto fail_free;
  }
  sleep_ms(1); // Wait for more than 100us
  rc = lcd_send_lower(i2c_dev, &(struct backpack_byte){.rs = 0, .rw = 0}, 0x3);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send third function set command");
    goto fail_free;
  }
  sleep_ms(1); // Wait for more than 100us
  rc = lcd_send_lower(i2c_dev, &(struct backpack_byte){.rs = 0, .rw = 0},
                      0x2); // Function set: 4-bit mode
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send fourth function set command");
    goto fail_free;
  }
  rc = lcd_send_command(i2c_dev, LCD_CMD_FUNCTION_SET_4BIT_2LINE_5x8);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send function set command");
    goto fail_free;
  }
  rc = lcd_send_command(i2c_dev, LCD_CMD_DISPLAY_OFF);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send display off command");
    goto fail_free;
  }
  rc = lcd_send_command(i2c_dev, LCD_CMD_CLEAR_DISPLAY);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send clear display command");
    goto fail_free;
  }
  rc = lcd_send_command(i2c_dev, LCD_CMD_ENTRY_INCREMENT_SET_NO_SHIFT);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send entry mode set command");
    goto fail_free;
  }
  rc = lcd_send_command(i2c_dev, LCD_CMD_DISPLAY_ON_CURSOR_OFF);
  if (rc < 0) {
    dev_err(i2c_dev, "Failed to send display on cursor off command");
    goto fail_free;
  }
  dev_dbg(i2c_dev, "LCD initialized");
  return 0;
fail_free:
  return rc;
}

static int lcd_probe(struct device *dev) {
  dev_dbg(dev, "Probing LCD device");
  struct device *i2c_dev =
      find_device_by_prop(dev, "i2c-dev", dev->fdt_node_offset, 0);
  if (!i2c_dev) {
    dev_err(dev, "Failed to find I2C device for LCD");
    return -ENODEV;
  }
  struct pcf8574t_16x2_lcd_pdata *pdata = calloc(1, sizeof(*pdata));
  if (!pdata) {
    dev_err(i2c_dev, "Failed to allocate platform data for LCD");
    return -ENOMEM;
  }
  pdata->i2c_dev = i2c_dev;
  int rc = lcd_init(i2c_dev);
  if (rc < 0) {
    dev_err(dev, "Failed to initialize LCD: %d", rc);
    return rc;
  }
  dev->platform_data = pdata;
  lcd_write_string(dev, "LCD Initialized");

  dev_dbg(dev, "LCD device probed successfully");
  return 0;
}

static void lcd_remove(struct device *dev) {
  free(dev->platform_data);
  dev->platform_data = NULL;
}

static int lcd_clear(const struct device *dev) {
  struct pcf8574t_16x2_lcd_pdata *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  return lcd_send_command(pdata->i2c_dev, LCD_CMD_CLEAR_DISPLAY);
}

static int lcd_set_cursor(const struct device *dev, uint8_t row, uint8_t col) {
  struct pcf8574t_16x2_lcd_pdata *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  if (row > 1 || col > 15) {
    dev_err(dev, "Invalid cursor position: row=%d, col=%d", row, col);
    return -EINVAL;
  }
  uint8_t address = (row == 0 ? LCD_DDRAM_ROW_1 : LCD_DDRAM_ROW_2) + col;
  return lcd_send_command(pdata->i2c_dev, LCD_CMD_SET_DDRAM_ADDRESS | address);
}

static int lcd_write_char(const struct device *dev, char c) {
  struct pcf8574t_16x2_lcd_pdata *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  return lcd_send_data(pdata->i2c_dev, (uint8_t)c);
}
int lcd_set_display_mode(const struct device *dev, enum display_mode mode) {
  struct pcf8574t_16x2_lcd_pdata *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "Platform data is NULL");
    return -EINVAL;
  }
  uint8_t command = LCD_CMD_DISPLAY_OFF; // Start with display off
  if (mode & DISPLAY_ON)
    command |= 0x04; // Display on
  if (mode & CURSOR_ON)
    command |= 0x02; // Cursor on
  if (mode & CURSOR_BLINK)
    command |= 0x01; // Cursor blink
  return lcd_send_command(pdata->i2c_dev, command);
}

static const struct driver_fdt_match_id lcd_match_table[] = {
    {.compatible = "royna,pcf8574t-16x2-lcd", .data = NULL}, {}};
static struct pcf8574t_16x2_lcd_driver lcd_drv = {
    .drv =
        {
            .name = "pcf8574t-16x2-lcd-drv",
            .probe = lcd_probe,
            .remove = lcd_remove,
            .match_table = lcd_match_table,
        },
    .clear = lcd_clear,
    .set_cursor = lcd_set_cursor,
    .write_char = lcd_write_char,
    .write_string = lcd_write_string,
    .set_display_mode = lcd_set_display_mode,
};

PLATFORM_DRIVER(lcd_drv);