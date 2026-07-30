#pragma once
#include <stdbool.h>

/* SSID 1..32 bytes; password empty (open net) or 8..64 bytes. */
bool creds_validate(const char *ssid, const char *password);
