#include "../uartdrv.h"
#include "../driver.h"
#include <stdio.h>

struct uart_platform_data {
    int tx, rx;
    const char* hw;
};

int fake_probe(struct device *device) { 
    if (!device->platform_data || !((struct uart_platform_data*)device->platform_data)->hw) {
        dev_info(device, "NO HW");
        return -18;
    }
    dev_info(device, "hw: %s, %s ready", ((struct uart_platform_data*)device->platform_data)->hw, device->name);
    return 0; 
}
int fake_probe_fail(struct device *device) { dev_info(device, "uart NOT READY 😡😡😡😡"); return -22; }
void fake_remove(struct device *device) { dev_info(device, "uart BEING DELETED FROM THE WORLD"); }

void read_nbytes(struct device *device, uint8_t *dst, size_t len) {
  dev_info(device, "Read n bytes FOR HW%s", ((struct uart_platform_data*)device->platform_data)->hw);
}

void write_nbytes(struct device *device, const uint8_t *src, size_t len) {
  dev_info(device, "Write n bytes");
}
void acquire(struct device *device) {
  dev_info(device, "Write n bytes");
}

static struct uart_driver fake_uart_driver = {
    .drv = {.name = "uart",
                                                  .probe = fake_probe,
                                                  .remove = fake_remove, .subsys_id = SUBSYS_UART, .match_id = NULL},
                                       .read_nbytes = read_nbytes,
                                       .write_nbytes = write_nbytes,
                                       .acquire = acquire, .release = acquire,
};
bool match_trash (const int id) { return id % 2 == 0;}
static struct uart_driver trash_uart_driver = {
    .drv = {.name = "trash-uart",
                                                  .probe = fake_probe_fail,
                                                  .remove = fake_remove, .subsys_id = SUBSYS_UART, .match_id = match_trash},
                                       .read_nbytes = read_nbytes,
                                       .write_nbytes = write_nbytes,
                                       .acquire = acquire, .release = acquire,
};



bool uart_get(struct uart_handle* hdl, int id) {
    struct device* dev = device_find(SUBSYS_UART, id);
    if (!dev) {
        printf("UNABLE TO GET UART: NO SUCH DEVICE\n");
        return false;
    }
    *hdl=  (struct uart_handle) {
        .dev = dev,
        .drv = container_of(dev->drv, struct uart_driver, drv),
    };
    return true;
}

int initcall_test() {
    driver_register(&fake_uart_driver.drv);
    driver_register(&trash_uart_driver.drv);
    return 0;
}

initcall(initcall_test);

  struct device uart0 = {
    .id = 0,
    .name = "uart0",
    .subsys = SUBSYS_UART,
    .platform_data = &(struct uart_platform_data) {
        .hw = "SOMETHING"
    }
  };
  
  struct device uart1 = {
    .id = 3,
    .name = "uart1",
    .subsys = SUBSYS_UART,
    .platform_data = &(struct uart_platform_data) {
        .hw = NULL
    }
  };
  struct device uart1240 = {
    .id = 1240,
    .name = "uart1240",
    .subsys = SUBSYS_UART,
    .platform_data = &(struct uart_platform_data) {
        .hw = "SOMETHING1240"
    }
  };
  struct device CAN1 = {
    .id = 0,
    .name = "CAN1",
    .subsys = 124,
  };

int main() {
  device_init();
  driver_init();


  for (const initcall_t* call = __start_init; call != __stop_init; call++) {
   (*call)();
  }

  device_register(&uart0);
  device_register(&uart0);
  device_register(&uart1);
  device_register(&uart1240);
  device_register(&uart1);
  device_register(&CAN1);

  struct uart_handle hdl;
  if (uart_get(&hdl, 0)) {
    hdl.drv->read_nbytes(hdl.dev, NULL, 0);
  }
  
  device_remove(SUBSYS_UART, 1);
  device_remove(SUBSYS_UART, 0);
  
  device_remove(SUBSYS_UART, 0);
  device_remove(SUBSYS_UART, 1);

  device_free();
  driver_free();
}