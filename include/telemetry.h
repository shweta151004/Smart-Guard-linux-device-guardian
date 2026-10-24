#ifndef TELEMETRY_H
#define TELEMETRY_H

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    double cpu_temperature_c;
    double cpu_usage_percent;
    unsigned long memory_available_kb;
} TelemetryData;

int telemetry_read_cpu_temperature(
    double *temperature_c
);

int telemetry_read_memory_available(
    unsigned long *memory_kb
);

int telemetry_read_cpu_usage(
    double *usage_percent
);

int telemetry_read_all(
    TelemetryData *data
);

#ifdef __cplusplus
}
#endif

#endif
