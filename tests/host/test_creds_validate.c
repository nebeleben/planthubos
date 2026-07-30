#include <assert.h>
#include <stdio.h>
#include "creds_validate.h"

int main(void)
{
    assert(creds_validate("MyWifi", "password123"));          /* normal */
    assert(creds_validate("A", ""));                          /* open network */
    assert(creds_validate("0123456789012345678901234567891", "12345678")); /* 31-char ssid */
    assert(!creds_validate("", "password123"));               /* empty ssid */
    assert(!creds_validate("01234567890123456789012345678912X", "12345678")); /* 33-char ssid */
    assert(!creds_validate("MyWifi", "1234567"));             /* 7-char password */
    char long_pw[66]; for (int i = 0; i < 65; i++) long_pw[i] = 'a'; long_pw[65] = 0;
    assert(!creds_validate("MyWifi", long_pw));               /* 65-char password */
    printf("test_creds_validate: OK\n");
    return 0;
}
