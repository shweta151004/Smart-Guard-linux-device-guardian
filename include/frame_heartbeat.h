#ifndef FRAME_HEARTBEAT_H
#define FRAME_HEARTBEAT_H

#ifdef __cplusplus
extern "C"
{
#endif

int frame_heartbeat_initialize(
    const char *path
);

int frame_heartbeat_record(
    unsigned long long frame_sequence
);

void frame_heartbeat_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
