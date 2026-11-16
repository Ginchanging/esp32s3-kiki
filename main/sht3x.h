/* SHT3x wire format, kept independent of ESP-IDF for host-side verification. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Decode T[2], CRC, RH[2], CRC. On a bad CRC neither output is changed. */
bool sht3x_decode(const uint8_t frame[6], float *temp_c, float *humidity);
