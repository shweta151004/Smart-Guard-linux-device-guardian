#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#include <stddef.h>

int camera_stream_start(
    const char *device,
    unsigned int width,
    unsigned int height,
    unsigned int fps
);

void camera_stream_stop(void);

int camera_stream_wait_for_frame(
    unsigned long long after_sequence,
    unsigned char **frame_data,
    size_t *frame_size,
    unsigned long long *frame_sequence,
    int timeout_ms
);

#endif
