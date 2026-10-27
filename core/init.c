#include "init.h"
#include "core/printk.h"
#include "hardware/structs/scb.h"
#include "pico/platform.h"

static inline uint32_t read_msp(void) {
  uint32_t value;
  __asm volatile("mrs %0, msp" : "=r"(value));
  return value;
}

static inline uint32_t read_control(void) {
  uint32_t value;
  __asm volatile("mrs %0, control" : "=r"(value));
  return value;
}

static inline uint32_t read_primask(void) {
  uint32_t value;
  __asm volatile("mrs %0, primask" : "=r"(value));
  return value;
}

static inline uint32_t read_ipsr(void) {
  uint32_t value;
  __asm volatile("mrs %0, ipsr" : "=r"(value));
  return value;
}

int print_cpu_info(void) {
  uint32_t cpuid = scb_hw->cpuid;

  unsigned implementer = (cpuid >> 24) & 0xff;
  unsigned variant = (cpuid >> 20) & 0xf;
  unsigned part = (cpuid >> 4) & 0xfff;
  unsigned revision = cpuid & 0xf;

  pr_info("Booting RoynaOS on physical CPU %u", get_core_num());

  pr_info("CPU%u: ARM processor [0x%08lx]", get_core_num(),
          (unsigned long)cpuid);

  pr_info("CPU%u: implementer 0x%02x, part 0x%03x, r%up%u", get_core_num(),
          implementer, part, variant, revision);
  pr_info("CPU%u: VTOR=0x%08lx MSP=0x%08lx", get_core_num(),
          (unsigned long)scb_hw->vtor, (unsigned long)read_msp());

  pr_info("CPU%u: CONTROL=0x%lx PRIMASK=%lu IPSR=%lu", get_core_num(),
          (unsigned long)read_control(), (unsigned long)read_primask(),
          (unsigned long)read_ipsr());
  return 0;
}

early_initcall(print_cpu_info);