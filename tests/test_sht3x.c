#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dash_model.h"
#include "sht3x.h"

static void close_to(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.002f);
}

int main(void)
{
    float t = 0, h = 0;
    /* Datasheet v7 table 20's published CRC vector: CRC(0xBEEF) = 0x92. */
    const uint8_t reference[] = {0xbe, 0xef, 0x92, 0xbe, 0xef, 0x92};
    assert(sht3x_decode(reference, &t, &h));
    close_to(t, 85.523003f);
    close_to(h, 74.584573f);

    const uint8_t low[] = {0, 0, 0x81, 0, 0, 0x81};
    const uint8_t high[] = {0xff, 0xff, 0xac, 0xff, 0xff, 0xac};
    const uint8_t room[] = {0x66, 0x66, 0x93, 0x80, 0x00, 0xa2};
    assert(sht3x_decode(low, &t, &h));
    close_to(t, -45); close_to(h, 0);
    assert(sht3x_decode(high, &t, &h));
    close_to(t, 130); close_to(h, 100);
    assert(sht3x_decode(room, &t, &h));
    close_to(t, 25); close_to(h, 50.000763f);

    /* Corrupt any data/CRC bit in either value. Even a valid temperature must
     * never be published when the humidity CRC fails. */
    for (unsigned byte = 0; byte < sizeof(reference); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t corrupt[6];
            memcpy(corrupt, reference, sizeof(corrupt));
            corrupt[byte] ^= (uint8_t)(1u << bit);
            t = -999; h = -999;
            assert(!sht3x_decode(corrupt, &t, &h));
            assert(t == -999 && h == -999);
        }
    }

    /* Hub refreshes and demo parsing must preserve device-owned readings. */
    dash_state_t state;
    dash_state_init(&state);
    assert(!state.sensor_valid);
    assert(sht3x_decode(room, &state.temp_c, &state.humidity));
    state.sensor_valid = true;
    const char *hub = "{\"schema\":1,\"generated_at\":1794830400,\"sensor\":{\"t\":99,\"h\":0}}";
    assert(dash_model_parse(&state, hub, strlen(hub)));
    assert(state.sensor_valid);
    close_to(state.temp_c, 25); close_to(state.humidity, 50.000763f);
    assert(!dash_model_parse(&state, "bad json", 8));
    assert(state.sensor_valid);
    close_to(state.temp_c, 25); close_to(state.humidity, 50.000763f);
    puts("SHT3x CRC, conversion, corruption rejection and local-state preservation passed");
    return 0;
}
