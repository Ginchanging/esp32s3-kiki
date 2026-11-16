#include "sht3x.h"

/* Sensirion SHT3x-DIS datasheet v7, sections 4.12/4.13:
 * https://sensirion.com/media/documents/213E6A3B/63A5A569/Datasheet_SHT3x_DIS.pdf */
static uint8_t crc8(const uint8_t data[2])
{
    uint8_t crc = 0xff;
    for (int i = 0; i < 2; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1);
        }
    }
    return crc;
}

bool sht3x_decode(const uint8_t frame[6], float *temp_c, float *humidity)
{
    if (crc8(frame) != frame[2] || crc8(frame + 3) != frame[5]) {
        return false;
    }
    uint16_t t = (uint16_t)((uint16_t)frame[0] << 8 | frame[1]);
    uint16_t h = (uint16_t)((uint16_t)frame[3] << 8 | frame[4]);
    *temp_c = -45.0f + 175.0f * t / 65535.0f;
    *humidity = 100.0f * h / 65535.0f;
    return true;
}
