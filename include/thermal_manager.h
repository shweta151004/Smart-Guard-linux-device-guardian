#ifndef THERMAL_MANAGER_H
#define THERMAL_MANAGER_H

#ifdef __cplusplus
extern "C"
{
#endif

#define THERMAL_STATUS_TEXT_SIZE 32
#define THERMAL_MESSAGE_SIZE 160
#define THERMAL_TIMESTAMP_SIZE 64

typedef struct
{
    int enabled;
    int sensor_available;
    int throttled;

    double current_temperature_c;
    double high_threshold_c;
    double recovery_threshold_c;

    int normal_fps;
    int reduced_fps;
    int target_fps;

    unsigned long long incident_count;
    int last_email_result;

    char status[
        THERMAL_STATUS_TEXT_SIZE
    ];

    char message[
        THERMAL_MESSAGE_SIZE
    ];

    char last_transition_timestamp[
        THERMAL_TIMESTAMP_SIZE
    ];
} ThermalManagerStatus;

int thermal_manager_initialize(void);

void thermal_manager_shutdown(void);

int thermal_manager_should_process_frame(void);

int thermal_manager_get_status(
    ThermalManagerStatus *status
);

#ifdef __cplusplus
}
#endif

#endif
