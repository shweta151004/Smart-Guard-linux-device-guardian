#ifndef GUARD_MODE_H
#define GUARD_MODE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

int guard_mode_initialize(
    const char *state_file
);

int guard_mode_is_enabled(void);

int guard_mode_set_enabled(
    int enabled
);

int guard_mode_handle_detection(
    int persons,
    const char *timestamp,
    double cpu_temperature_c,
    const unsigned char *jpeg_data,
    size_t jpeg_size
);

void guard_mode_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
