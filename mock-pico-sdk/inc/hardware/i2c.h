#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define I2C0_BASE 0x40044000
#define I2C1_BASE 0x40048000

typedef int i2c_hw_t;
#define i2c0 ((i2c_hw_t *)I2C0_BASE)
#define i2c1 ((i2c_hw_t *)I2C1_BASE)
typedef struct i2c_inst {
  i2c_hw_t *hw;
} i2c_inst_t;

/*! \brief   Initialise the I2C HW block
 *  \ingroup hardware_i2c
 *
 * Put the I2C hardware into a known state, and enable it. Must be called
 * before other functions. By default, the I2C is configured to operate as a
 * master.
 *
 * The I2C bus frequency is set as close as possible to requested, and
 * the actual rate set is returned
 *
 * \param i2c Either \ref i2c0 or \ref i2c1
 * \param baudrate Baudrate in Hz (e.g. 100kHz is 100000)
 * \return Actual set baudrate
 */
static inline unsigned int i2c_init(i2c_inst_t *i2c, unsigned int baudrate) {
  printf("i2c_init called with baudrate: %u\n", baudrate);
  return baudrate;
}

/*! \brief   Disable the I2C HW block
 *  \ingroup hardware_i2c
 *
 * \param i2c Either \ref i2c0 or \ref i2c1
 *
 * Disable the I2C again if it is no longer used. Must be reinitialised before
 * being used again.
 */
static inline void i2c_deinit(i2c_inst_t *i2c) {
  printf("i2c_deinit called\n");
}

/*! \brief  Set I2C baudrate
 *  \ingroup hardware_i2c
 *
 * Set I2C bus frequency as close as possible to requested, and return actual
 * rate set.
 * Baudrate may not be as exactly requested due to clocking limitations.
 *
 * \param i2c Either \ref i2c0 or \ref i2c1
 * \param baudrate Baudrate in Hz (e.g. 100kHz is 100000)
 * \return Actual set baudrate
 */
static inline unsigned int i2c_set_baudrate(i2c_inst_t *i2c,
                                            unsigned int baudrate) {
  printf("i2c_set_baudrate called with baudrate: %u\n", baudrate);
  return baudrate;
}

/*! \brief  Set I2C port to slave mode
 *  \ingroup hardware_i2c
 *
 * \param i2c Either \ref i2c0 or \ref i2c1
 * \param slave true to use slave mode, false to use master mode
 * \param addr If \p slave is true, set the slave address to this value
 */
static inline void i2c_set_slave_mode(i2c_inst_t *i2c, bool slave,
                                      uint8_t addr) {
  printf("i2c_set_slave_mode called with slave: %d, addr: 0x%02x\n", slave,
         addr);
}

static inline int i2c_read_blocking(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst,
                                    size_t len, bool nostop) {
  printf("i2c_read_blocking called with addr: 0x%02x, len: %zu, nostop: %d\n",
         addr, len, nostop);
  return 0;
}
static inline int i2c_write_blocking(i2c_inst_t *i2c, uint8_t addr,
                                     const uint8_t *src, size_t len,
                                     bool nostop) {
  printf("i2c_write_blocking called with addr: 0x%02x, len: %zu, nostop: %d\n",
         addr, len, nostop);
  return 0;
}