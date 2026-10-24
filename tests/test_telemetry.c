#include <stdio.h>
#include <stdlib.h>

#include "telemetry.h"

int main(void)
{
    TelemetryData data;

    if (telemetry_read_all(&data) != 0)
    {
        fprintf(stderr, "Telemetry reading failed.\n");
        return EXIT_FAILURE;
    }

    printf(
        "CPU temperature: %.2f C\n",
        data.cpu_temperature_c
    );

    printf(
        "Available memory: %.2f MB\n",
        (double)data.memory_available_kb / 1024.0
    );

    printf(
        "CPU usage: %.2f %%\n",
        data.cpu_usage_percent
    );

    return EXIT_SUCCESS;
}
