#pragma once
#include <stdbool.h>
#include <stdint.h>

void authtok_hex_encode(const uint8_t in[32], char out[65]);
bool authtok_hex_decode(const char *hex, uint8_t out[32]);
bool authtok_ct_equal(const uint8_t a[32], const uint8_t b[32]);
