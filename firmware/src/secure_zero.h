#pragma once

#include <stddef.h>
#include <stdint.h>

inline void wifiSecureZero(void* memory, size_t size) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(memory);
  while (size--) *bytes++ = 0;
}
