#include "creds_validate.h"
#include <string.h>

bool creds_validate(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;
    size_t slen = strlen(ssid);
    size_t plen = strlen(password);
    if (slen < 1 || slen > 32) return false;
    if (plen != 0 && (plen < 8 || plen > 64)) return false;
    return true;
}
