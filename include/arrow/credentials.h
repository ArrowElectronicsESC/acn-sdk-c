#if !defined(WIFI_CREDENTIALS_H_)
#define WIFI_CREDENTIALS_H_

#include <unint.h>

typedef struct _wifi_credentials {
    char *ssid;
    char *pass;
    uint32_t sec;
} wifi_credentials_t;

uint32_t credentials_qnt();
wifi_credentials_t *credentials_get(int index);

#endif  // WIFI_CREDENTIALS_H_
