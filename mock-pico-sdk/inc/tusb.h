#pragma once

#include <stdint.h>
#include <string.h>

#define PICO_DEFAULT_LED_PIN 25

void tusb_init(void);
void tud_task(void);
void tud_task_ext(int ss, int ms);
uint32_t tud_cdc_n_write(uint8_t itf, const void *buffer, uint32_t size);
uint32_t tud_cdc_n_write_flush(uint8_t itf);
uint32_t tud_cdc_n_available(uint8_t itf);
uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t size);
uint32_t tud_cdc_n_read_flush(uint8_t itf);
uint32_t tud_cdc_n_write_available(uint8_t itf);
