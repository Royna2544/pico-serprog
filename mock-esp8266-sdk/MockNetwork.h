#pragma once
namespace esp_mock {
// Runtime hook for simulated power-off/reboot; sketch code uses standard APIs.
void resetNetwork();
} // namespace esp_mock
