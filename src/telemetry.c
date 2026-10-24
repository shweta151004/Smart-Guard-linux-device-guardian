#define _POSIX_C_SOURCE 200809L

#include "telemetry.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
    unsigned long long idle;
    unsigned long long total;
} CpuSnapshot;

static int read_text_file(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        return -1;
    }

    if (fgets(buffer, (int)buffer_size, file) == NULL)
    {
        fclose(file);
        return -1;
    }

    fclose(file);

    size_t length = strlen(buffer);

    while (
        length > 0 &&
        (
            buffer[length - 1] == '\n' ||
            buffer[length - 1] == '\r'
        )
    )
    {
        buffer[length - 1] = '\0';
        length--;
    }

    return 0;
}

static int read_long_file(
    const char *path,
    long *value
)
{
    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        return -1;
    }

    int result = fscanf(file, "%ld", value);

    fclose(file);

    return result == 1 ? 0 : -1;
}

static int thermal_type_score(const char *type)
{
    if (strcmp(type, "x86_pkg_temp") == 0)
    {
        return 100;
    }

    if (
        strstr(type, "cpu") != NULL ||
        strstr(type, "CPU") != NULL
    )
    {
        return 90;
    }

    if (
        strstr(type, "soc") != NULL ||
        strstr(type, "SOC") != NULL
    )
    {
        return 80;
    }

    if (strcmp(type, "acpitz") == 0)
    {
        return 20;
    }

    return 0;
}

int telemetry_read_cpu_temperature(double *temperature_c)
{
    if (temperature_c == NULL)
    {
        return -1;
    }

    DIR *directory = opendir("/sys/class/thermal");

    if (directory == NULL)
    {
        return -1;
    }

    struct dirent *entry;
    int best_score = -1;
    long best_temperature = 0;

    while ((entry = readdir(directory)) != NULL)
    {
        if (strncmp(entry->d_name, "thermal_zone", 12) != 0)
        {
            continue;
        }

        char type_path[PATH_MAX];
        char temp_path[PATH_MAX];

        int type_length = snprintf(
            type_path,
            sizeof(type_path),
            "/sys/class/thermal/%s/type",
            entry->d_name
        );

        int temp_length = snprintf(
            temp_path,
            sizeof(temp_path),
            "/sys/class/thermal/%s/temp",
            entry->d_name
        );

        if (
            type_length < 0 ||
            temp_length < 0 ||
            (size_t)type_length >= sizeof(type_path) ||
            (size_t)temp_length >= sizeof(temp_path)
        )
        {
            continue;
        }

        char type[128];
        long temperature;

        if (read_text_file(type_path, type, sizeof(type)) != 0)
        {
            continue;
        }

        if (read_long_file(temp_path, &temperature) != 0)
        {
            continue;
        }

        if (temperature <= 0 || temperature >= 200000)
        {
            continue;
        }

        int score = thermal_type_score(type);

        if (score > best_score)
        {
            best_score = score;
            best_temperature = temperature;
        }
    }

    closedir(directory);

    if (best_score < 0)
    {
        return -1;
    }

    *temperature_c = (double)best_temperature / 1000.0;

    return 0;
}

int telemetry_read_memory_available(unsigned long *memory_kb)
{
    if (memory_kb == NULL)
    {
        return -1;
    }

    FILE *file = fopen("/proc/meminfo", "r");

    if (file == NULL)
    {
        return -1;
    }

    char line[256];
    unsigned long value = 0;
    int found = 0;

    while (fgets(line, sizeof(line), file) != NULL)
    {
        if (
            sscanf(
                line,
                "MemAvailable: %lu kB",
                &value
            ) == 1
        )
        {
            found = 1;
            break;
        }
    }

    fclose(file);

    if (!found)
    {
        return -1;
    }

    *memory_kb = value;

    return 0;
}

static int read_cpu_snapshot(CpuSnapshot *snapshot)
{
    if (snapshot == NULL)
    {
        return -1;
    }

    FILE *file = fopen("/proc/stat", "r");

    if (file == NULL)
    {
        return -1;
    }

    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;

    int fields = fscanf(
        file,
        "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &user,
        &nice,
        &system,
        &idle,
        &iowait,
        &irq,
        &softirq,
        &steal
    );

    fclose(file);

    if (fields != 8)
    {
        return -1;
    }

    snapshot->idle = idle + iowait;

    snapshot->total =
        user +
        nice +
        system +
        idle +
        iowait +
        irq +
        softirq +
        steal;

    return 0;
}

int telemetry_read_cpu_usage(double *usage_percent)
{
    if (usage_percent == NULL)
    {
        return -1;
    }

    CpuSnapshot first;
    CpuSnapshot second;

    if (read_cpu_snapshot(&first) != 0)
    {
        return -1;
    }

    struct timespec delay;

    delay.tv_sec = 0;
    delay.tv_nsec = 200000000L;

    nanosleep(&delay, NULL);

    if (read_cpu_snapshot(&second) != 0)
    {
        return -1;
    }

    unsigned long long total_delta =
        second.total - first.total;

    unsigned long long idle_delta =
        second.idle - first.idle;

    if (total_delta == 0)
    {
        return -1;
    }

    *usage_percent =
        100.0 *
        (double)(total_delta - idle_delta) /
        (double)total_delta;

    return 0;
}

int telemetry_read_all(TelemetryData *data)
{
    if (data == NULL)
    {
        return -1;
    }

    if (
        telemetry_read_cpu_temperature(
            &data->cpu_temperature_c
        ) != 0
    )
    {
        return -1;
    }

    if (
        telemetry_read_memory_available(
            &data->memory_available_kb
        ) != 0
    )
    {
        return -1;
    }

    if (
        telemetry_read_cpu_usage(
            &data->cpu_usage_percent
        ) != 0
    )
    {
        return -1;
    }

    return 0;
}
