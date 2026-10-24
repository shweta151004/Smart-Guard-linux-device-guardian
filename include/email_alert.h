#ifndef EMAIL_ALERT_H
#define EMAIL_ALERT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

int email_alert_initialize(void);

int email_alert_queue(
    int persons,
    const char *timestamp,
    double cpu_temperature_c,
    const unsigned char *jpeg_data,
    size_t jpeg_size
);


int email_alert_queue_immediate(
    int persons,
    const char *timestamp,
    double cpu_temperature_c,
    const unsigned char *jpeg_data,
    size_t jpeg_size
);

void email_alert_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
