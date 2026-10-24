#define _POSIX_C_SOURCE 200809L

#include "thermal_manager.h"
#include "telemetry.h"

#include <curl/curl.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STUDENT_ID "device-001"

#define ADDRESS_SIZE 320
#define PASSWORD_SIZE 512
#define URL_SIZE 512
#define BODY_SIZE 2048

typedef struct
{
    int enabled;
    int email_enabled;
    int poll_seconds;

    double high_threshold_c;
    double recovery_threshold_c;

    int normal_fps;
    int reduced_fps;

    char status_path[PATH_MAX];
    char test_temperature_path[PATH_MAX];

    char smtp_url[URL_SIZE];
    char username[ADDRESS_SIZE];
    char password[PASSWORD_SIZE];
    char from_address[ADDRESS_SIZE];
    char to_address[ADDRESS_SIZE];
} ThermalConfig;

typedef struct
{
    pthread_mutex_t mutex;
    pthread_t thread;

    int initialized;
    int running;
    int thread_started;

    long long last_processing_slot_ns;

    ThermalConfig config;
    ThermalManagerStatus status;
} ThermalState;

static ThermalState thermal_state =
{
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

static int parse_boolean(
    const char *value,
    int default_value
)
{
    if (
        value == NULL ||
        value[0] == '\0'
    )
    {
        return default_value;
    }

    if (
        strcmp(value, "1") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0
    )
    {
        return 1;
    }

    if (
        strcmp(value, "0") == 0 ||
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0
    )
    {
        return 0;
    }

    return default_value;
}

static int read_integer_environment(
    const char *name,
    int default_value,
    int minimum_value,
    int maximum_value
)
{
    const char *value = getenv(name);

    if (
        value == NULL ||
        value[0] == '\0'
    )
    {
        return default_value;
    }

    char *end = NULL;

    errno = 0;

    long parsed = strtol(
        value,
        &end,
        10
    );

    if (
        errno != 0 ||
        end == value ||
        *end != '\0' ||
        parsed < minimum_value ||
        parsed > maximum_value
    )
    {
        return default_value;
    }

    return (int)parsed;
}

static double read_double_environment(
    const char *name,
    double default_value,
    double minimum_value,
    double maximum_value
)
{
    const char *value = getenv(name);

    if (
        value == NULL ||
        value[0] == '\0'
    )
    {
        return default_value;
    }

    char *end = NULL;

    errno = 0;

    double parsed = strtod(
        value,
        &end
    );

    if (
        errno != 0 ||
        end == value ||
        *end != '\0' ||
        parsed < minimum_value ||
        parsed > maximum_value
    )
    {
        return default_value;
    }

    return parsed;
}

static int copy_value(
    char *destination,
    size_t destination_size,
    const char *value
)
{
    if (
        destination == NULL ||
        destination_size == 0 ||
        value == NULL ||
        value[0] == '\0'
    )
    {
        return -1;
    }

    int length = snprintf(
        destination,
        destination_size,
        "%s",
        value
    );

    if (
        length < 0 ||
        (size_t)length >= destination_size
    )
    {
        return -1;
    }

    return 0;
}

static int copy_environment_or_default(
    const char *name,
    const char *default_value,
    char *destination,
    size_t destination_size
)
{
    const char *value = getenv(name);

    if (
        value == NULL ||
        value[0] == '\0'
    )
    {
        value = default_value;
    }

    return copy_value(
        destination,
        destination_size,
        value
    );
}

static void create_timestamp(
    char *buffer,
    size_t buffer_size
)
{
    time_t current_time = time(NULL);
    struct tm local_time;

    if (
        current_time == (time_t)-1 ||
        localtime_r(
            &current_time,
            &local_time
        ) == NULL ||
        strftime(
            buffer,
            buffer_size,
            "%Y-%m-%dT%H:%M:%S%z",
            &local_time
        ) == 0
    )
    {
        snprintf(
            buffer,
            buffer_size,
            "unavailable"
        );
    }
}

static long long current_monotonic_ns(void)
{
    struct timespec current_time;

    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &current_time
        ) != 0
    )
    {
        return -1;
    }

    return
        (long long)current_time.tv_sec *
            1000000000LL +
        (long long)current_time.tv_nsec;
}

static void sleep_interruptibly(int seconds)
{
    struct timespec delay;

    delay.tv_sec = seconds;
    delay.tv_nsec = 0;

    while (
        nanosleep(
            &delay,
            &delay
        ) != 0
    )
    {
        if (errno != EINTR)
        {
            break;
        }
    }
}

static int load_configuration(
    ThermalConfig *config
)
{
    if (config == NULL)
    {
        return -1;
    }

    memset(
        config,
        0,
        sizeof(*config)
    );

    config->enabled =
        parse_boolean(
            getenv("THERMAL_MANAGER_ENABLED"),
            1
        );

    config->email_enabled =
        parse_boolean(
            getenv("THERMAL_EMAIL_ENABLED"),
            1
        );

    config->poll_seconds =
        read_integer_environment(
            "THERMAL_POLL_SECONDS",
            2,
            1,
            60
        );

    config->high_threshold_c =
        read_double_environment(
            "THERMAL_HIGH_C",
            85.0,
            40.0,
            120.0
        );

    config->recovery_threshold_c =
        read_double_environment(
            "THERMAL_RECOVERY_C",
            75.0,
            30.0,
            115.0
        );

    config->normal_fps =
        read_integer_environment(
            "THERMAL_NORMAL_FPS",
            30,
            1,
            120
        );

    config->reduced_fps =
        read_integer_environment(
            "THERMAL_REDUCED_FPS",
            10,
            1,
            60
        );

    if (
        config->recovery_threshold_c >=
            config->high_threshold_c ||
        config->reduced_fps >=
            config->normal_fps
    )
    {
        return -1;
    }

    if (
        copy_environment_or_default(
            "THERMAL_STATUS_PATH",
            "/run/smart-guard/thermal_status",
            config->status_path,
            sizeof(config->status_path)
        ) != 0 ||
        copy_environment_or_default(
            "THERMAL_TEST_TEMPERATURE_PATH",
            "/run/smart-guard/"
            "thermal_test_temperature",
            config->test_temperature_path,
            sizeof(
                config->test_temperature_path
            )
        ) != 0
    )
    {
        return -1;
    }

    if (!config->email_enabled)
    {
        return 0;
    }

    if (
        copy_environment_or_default(
            "EMAIL_SMTP_URL",
            NULL,
            config->smtp_url,
            sizeof(config->smtp_url)
        ) != 0 ||
        copy_environment_or_default(
            "EMAIL_USERNAME",
            NULL,
            config->username,
            sizeof(config->username)
        ) != 0 ||
        copy_environment_or_default(
            "EMAIL_PASSWORD",
            NULL,
            config->password,
            sizeof(config->password)
        ) != 0 ||
        copy_environment_or_default(
            "EMAIL_FROM",
            NULL,
            config->from_address,
            sizeof(config->from_address)
        ) != 0 ||
        copy_environment_or_default(
            "EMAIL_TO",
            NULL,
            config->to_address,
            sizeof(config->to_address)
        ) != 0
    )
    {
        return -1;
    }

    return 0;
}

static int read_test_temperature(
    const char *path,
    double *temperature_c
)
{
    if (
        path == NULL ||
        temperature_c == NULL
    )
    {
        return -1;
    }

    FILE *file = fopen(
        path,
        "r"
    );

    if (file == NULL)
    {
        return -1;
    }

    double value = 0.0;

    int result = fscanf(
        file,
        "%lf",
        &value
    );

    fclose(file);

    if (
        result != 1 ||
        value < -20.0 ||
        value > 200.0
    )
    {
        return -1;
    }

    *temperature_c = value;

    return 0;
}

static int read_effective_temperature(
    const ThermalConfig *config,
    double *temperature_c
)
{
    if (
        config == NULL ||
        temperature_c == NULL
    )
    {
        return -1;
    }

    if (
        read_test_temperature(
            config->test_temperature_path,
            temperature_c
        ) == 0
    )
    {
        return 0;
    }

    return telemetry_read_cpu_temperature(
        temperature_c
    );
}

static int send_thermal_email(
    const ThermalConfig *config,
    double temperature_c,
    unsigned long long incident_count
)
{
    if (
        config == NULL ||
        !config->email_enabled
    )
    {
        return -1;
    }

    CURL *curl = curl_easy_init();

    if (curl == NULL)
    {
        return -1;
    }

    CURLcode result = CURLE_OK;

    struct curl_slist *recipients = NULL;
    struct curl_slist *headers = NULL;

    curl_mime *mime = NULL;
    curl_mimepart *part = NULL;

    char envelope_from[
        ADDRESS_SIZE + 4
    ];

    char envelope_to[
        ADDRESS_SIZE + 4
    ];

    char from_header[
        ADDRESS_SIZE + 40
    ];

    char to_header[
        ADDRESS_SIZE + 8
    ];

    char timestamp[64];
    char body[BODY_SIZE];

    create_timestamp(
        timestamp,
        sizeof(timestamp)
    );

    snprintf(
        envelope_from,
        sizeof(envelope_from),
        "<%s>",
        config->from_address
    );

    snprintf(
        envelope_to,
        sizeof(envelope_to),
        "<%s>",
        config->to_address
    );

    snprintf(
        from_header,
        sizeof(from_header),
        "From: Smart Guard Thermal Manager <%s>",
        config->from_address
    );

    snprintf(
        to_header,
        sizeof(to_header),
        "To: %s",
        config->to_address
    );

    snprintf(
        body,
        sizeof(body),
        "Smart Guard adaptive thermal management alert\n"
        "\n"
        "Student ID: %s\n"
        "Timestamp: %s\n"
        "CPU temperature: %.1f C\n"
        "High-temperature threshold: %.1f C\n"
        "Processing FPS changed: %d -> %d\n"
        "Thermal incident number: %llu\n"
        "\n"
        "The system automatically reduced the "
        "image-processing rate to protect the CPU.\n"
        "Normal processing will resume after the "
        "temperature falls to %.1f C or lower.\n",
        STUDENT_ID,
        timestamp,
        temperature_c,
        config->high_threshold_c,
        config->normal_fps,
        config->reduced_fps,
        incident_count,
        config->recovery_threshold_c
    );

    recipients = curl_slist_append(
        recipients,
        envelope_to
    );

    headers = curl_slist_append(
        headers,
        from_header
    );

    headers = curl_slist_append(
        headers,
        to_header
    );

    headers = curl_slist_append(
        headers,
        "Subject: Smart Guard Thermal Alert - "
        "CPU Temperature High"
    );

    if (
        recipients == NULL ||
        headers == NULL
    )
    {
        result = CURLE_OUT_OF_MEMORY;
        goto cleanup;
    }

    mime = curl_mime_init(curl);

    if (mime == NULL)
    {
        result = CURLE_OUT_OF_MEMORY;
        goto cleanup;
    }

    part = curl_mime_addpart(mime);

    if (part == NULL)
    {
        result = CURLE_OUT_OF_MEMORY;
        goto cleanup;
    }

    result = curl_mime_data(
        part,
        body,
        CURL_ZERO_TERMINATED
    );

    if (result != CURLE_OK)
    {
        goto cleanup;
    }

    result = curl_mime_type(
        part,
        "text/plain; charset=utf-8"
    );

    if (result != CURLE_OK)
    {
        goto cleanup;
    }

#define SET_OPTION(option, value) \
    do \
    { \
        result = curl_easy_setopt( \
            curl, \
            option, \
            value \
        ); \
        if (result != CURLE_OK) \
        { \
            goto cleanup; \
        } \
    } while (0)

    SET_OPTION(
        CURLOPT_URL,
        config->smtp_url
    );

    SET_OPTION(
        CURLOPT_USERNAME,
        config->username
    );

    SET_OPTION(
        CURLOPT_PASSWORD,
        config->password
    );

    SET_OPTION(
        CURLOPT_USE_SSL,
        CURLUSESSL_ALL
    );

    SET_OPTION(
        CURLOPT_MAIL_FROM,
        envelope_from
    );

    SET_OPTION(
        CURLOPT_MAIL_RCPT,
        recipients
    );

    SET_OPTION(
        CURLOPT_HTTPHEADER,
        headers
    );

    SET_OPTION(
        CURLOPT_MIMEPOST,
        mime
    );

    SET_OPTION(
        CURLOPT_CONNECTTIMEOUT,
        10L
    );

    SET_OPTION(
        CURLOPT_TIMEOUT,
        20L
    );

    SET_OPTION(
        CURLOPT_NOSIGNAL,
        1L
    );

    SET_OPTION(
        CURLOPT_SSL_VERIFYPEER,
        1L
    );

    SET_OPTION(
        CURLOPT_SSL_VERIFYHOST,
        2L
    );

#undef SET_OPTION

    result = curl_easy_perform(curl);

cleanup:

    if (result != CURLE_OK)
    {
        fprintf(
            stderr,
            "Thermal alert email failed: %s\n",
            curl_easy_strerror(result)
        );
    }

    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    return result == CURLE_OK
        ? 0
        : -1;
}

static int write_status_file(void)
{
    ThermalManagerStatus status;
    char path[PATH_MAX];

    pthread_mutex_lock(
        &thermal_state.mutex
    );

    status = thermal_state.status;

    snprintf(
        path,
        sizeof(path),
        "%s",
        thermal_state.config.status_path
    );

    pthread_mutex_unlock(
        &thermal_state.mutex
    );

    char temporary_path[
        PATH_MAX + 64
    ];

    int path_length = snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp.%ld",
        path,
        (long)getpid()
    );

    if (
        path_length < 0 ||
        (size_t)path_length >=
            sizeof(temporary_path)
    )
    {
        return -1;
    }

    FILE *file = fopen(
        temporary_path,
        "w"
    );

    if (file == NULL)
    {
        return -1;
    }

    char timestamp[64];

    create_timestamp(
        timestamp,
        sizeof(timestamp)
    );

    fprintf(
        file,
        "version=1\n"
        "status=%s\n"
        "timestamp=%s\n"
        "enabled=%d\n"
        "sensor_available=%d\n"
        "throttled=%d\n"
        "current_temperature_c=%.2f\n"
        "high_threshold_c=%.2f\n"
        "recovery_threshold_c=%.2f\n"
        "normal_fps=%d\n"
        "reduced_fps=%d\n"
        "target_fps=%d\n"
        "incident_count=%llu\n"
        "last_email_result=%d\n"
        "last_transition_timestamp=%s\n"
        "message=%s\n",
        status.status,
        timestamp,
        status.enabled,
        status.sensor_available,
        status.throttled,
        status.current_temperature_c,
        status.high_threshold_c,
        status.recovery_threshold_c,
        status.normal_fps,
        status.reduced_fps,
        status.target_fps,
        status.incident_count,
        status.last_email_result,
        status.last_transition_timestamp,
        status.message
    );

    if (
        fflush(file) != 0 ||
        fclose(file) != 0
    )
    {
        unlink(temporary_path);
        return -1;
    }

    if (
        rename(
            temporary_path,
            path
        ) != 0
    )
    {
        unlink(temporary_path);
        return -1;
    }

    return 0;
}

static void set_transition_timestamp_locked(void)
{
    create_timestamp(
        thermal_state.status.
            last_transition_timestamp,
        sizeof(
            thermal_state.status.
                last_transition_timestamp
        )
    );
}

static void *thermal_thread(void *argument)
{
    (void)argument;

    while (1)
    {
        pthread_mutex_lock(
            &thermal_state.mutex
        );

        int running =
            thermal_state.running;

        ThermalConfig config =
            thermal_state.config;

        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        if (!running)
        {
            break;
        }

        double temperature_c = -1.0;

        int temperature_result =
            read_effective_temperature(
                &config,
                &temperature_c
            );

        int send_email_now = 0;
        unsigned long long incident_number = 0;

        pthread_mutex_lock(
            &thermal_state.mutex
        );

        if (temperature_result == 0)
        {
            thermal_state.status.
                sensor_available = 1;

            thermal_state.status.
                current_temperature_c =
                    temperature_c;

            if (
                !thermal_state.status.throttled &&
                temperature_c >=
                    config.high_threshold_c
            )
            {
                thermal_state.status.throttled = 1;

                thermal_state.status.target_fps =
                    config.reduced_fps;

                thermal_state.status.
                    incident_count++;

                thermal_state.status.
                    last_email_result = -2;

                thermal_state.
                    last_processing_slot_ns = 0;

                snprintf(
                    thermal_state.status.status,
                    sizeof(
                        thermal_state.status.status
                    ),
                    "throttled"
                );

                snprintf(
                    thermal_state.status.message,
                    sizeof(
                        thermal_state.status.message
                    ),
                    "CPU temperature exceeded the "
                    "high threshold; processing FPS "
                    "was reduced"
                );

                set_transition_timestamp_locked();

                send_email_now =
                    config.email_enabled;

                incident_number =
                    thermal_state.status.
                        incident_count;

                printf(
                    "Thermal throttling activated: "
                    "temperature=%.1fC threshold=%.1fC "
                    "fps=%d->%d incident=%llu\n",
                    temperature_c,
                    config.high_threshold_c,
                    config.normal_fps,
                    config.reduced_fps,
                    incident_number
                );
            }
            else if (
                thermal_state.status.throttled &&
                temperature_c <=
                    config.recovery_threshold_c
            )
            {
                thermal_state.status.throttled = 0;

                thermal_state.status.target_fps =
                    config.normal_fps;

                thermal_state.
                    last_processing_slot_ns = 0;

                snprintf(
                    thermal_state.status.status,
                    sizeof(
                        thermal_state.status.status
                    ),
                    "normal"
                );

                snprintf(
                    thermal_state.status.message,
                    sizeof(
                        thermal_state.status.message
                    ),
                    "CPU temperature recovered; "
                    "normal processing FPS resumed"
                );

                set_transition_timestamp_locked();

                printf(
                    "Thermal recovery completed: "
                    "temperature=%.1fC recovery=%.1fC "
                    "fps=%d\n",
                    temperature_c,
                    config.recovery_threshold_c,
                    config.normal_fps
                );
            }
            else
            {
                snprintf(
                    thermal_state.status.status,
                    sizeof(
                        thermal_state.status.status
                    ),
                    "%s",
                    thermal_state.status.throttled
                        ? "throttled"
                        : "normal"
                );

                snprintf(
                    thermal_state.status.message,
                    sizeof(
                        thermal_state.status.message
                    ),
                    "%s",
                    thermal_state.status.throttled
                        ? "Thermal throttling is active"
                        : "CPU temperature is within "
                          "the normal range"
                );
            }
        }
        else
        {
            thermal_state.status.
                sensor_available = 0;

            snprintf(
                thermal_state.status.status,
                sizeof(
                    thermal_state.status.status
                ),
                "sensor_error"
            );

            snprintf(
                thermal_state.status.message,
                sizeof(
                    thermal_state.status.message
                ),
                "CPU temperature sensor is unavailable"
            );
        }

        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        write_status_file();

        if (send_email_now)
        {
            int email_result =
                send_thermal_email(
                    &config,
                    temperature_c,
                    incident_number
                );

            pthread_mutex_lock(
                &thermal_state.mutex
            );

            thermal_state.status.
                last_email_result =
                    email_result;

            snprintf(
                thermal_state.status.message,
                sizeof(
                    thermal_state.status.message
                ),
                "%s",
                email_result == 0
                    ? "Thermal throttling is active; "
                      "alert email was sent"
                    : "Thermal throttling is active; "
                      "alert email failed"
            );

            pthread_mutex_unlock(
                &thermal_state.mutex
            );

            if (email_result == 0)
            {
                printf(
                    "Thermal alert email sent successfully.\n"
                );
            }

            write_status_file();
        }

        sleep_interruptibly(
            config.poll_seconds
        );
    }

    return NULL;
}

int thermal_manager_initialize(void)
{
    pthread_mutex_lock(
        &thermal_state.mutex
    );

    if (thermal_state.initialized)
    {
        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        return 0;
    }

    ThermalConfig config;

    if (
        load_configuration(
            &config
        ) != 0
    )
    {
        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        return -1;
    }

    memset(
        &thermal_state.status,
        0,
        sizeof(thermal_state.status)
    );

    thermal_state.config = config;
    thermal_state.running = 1;
    thermal_state.initialized = 1;
    thermal_state.thread_started = 0;
    thermal_state.last_processing_slot_ns = 0;

    thermal_state.status.enabled =
        config.enabled;

    thermal_state.status.sensor_available = 0;
    thermal_state.status.throttled = 0;
    thermal_state.status.current_temperature_c = -1.0;

    thermal_state.status.high_threshold_c =
        config.high_threshold_c;

    thermal_state.status.recovery_threshold_c =
        config.recovery_threshold_c;

    thermal_state.status.normal_fps =
        config.normal_fps;

    thermal_state.status.reduced_fps =
        config.reduced_fps;

    thermal_state.status.target_fps =
        config.normal_fps;

    thermal_state.status.incident_count = 0;
    thermal_state.status.last_email_result = -2;

    snprintf(
        thermal_state.status.status,
        sizeof(thermal_state.status.status),
        "%s",
        config.enabled
            ? "starting"
            : "disabled"
    );

    snprintf(
        thermal_state.status.message,
        sizeof(thermal_state.status.message),
        "%s",
        config.enabled
            ? "Waiting for CPU temperature reading"
            : "Thermal manager is disabled"
    );

    snprintf(
        thermal_state.status.
            last_transition_timestamp,
        sizeof(
            thermal_state.status.
                last_transition_timestamp
        ),
        "none"
    );

    if (
        config.email_enabled &&
        curl_global_init(
            CURL_GLOBAL_DEFAULT
        ) != CURLE_OK
    )
    {
        thermal_state.initialized = 0;
        thermal_state.running = 0;

        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        return -1;
    }

    if (
        pthread_create(
            &thermal_state.thread,
            NULL,
            thermal_thread,
            NULL
        ) != 0
    )
    {
        if (config.email_enabled)
        {
            curl_global_cleanup();
        }

        thermal_state.initialized = 0;
        thermal_state.running = 0;

        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        return -1;
    }

    thermal_state.thread_started = 1;

    pthread_mutex_unlock(
        &thermal_state.mutex
    );

    printf(
        "Thermal manager initialized: "
        "high=%.1fC recovery=%.1fC "
        "normal_fps=%d reduced_fps=%d "
        "email=%s\n",
        config.high_threshold_c,
        config.recovery_threshold_c,
        config.normal_fps,
        config.reduced_fps,
        config.email_enabled
            ? "enabled"
            : "disabled"
    );

    return 0;
}

void thermal_manager_shutdown(void)
{
    pthread_mutex_lock(
        &thermal_state.mutex
    );

    if (!thermal_state.initialized)
    {
        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        return;
    }

    thermal_state.running = 0;

    int thread_started =
        thermal_state.thread_started;

    pthread_t thread =
        thermal_state.thread;

    int email_enabled =
        thermal_state.config.email_enabled;

    pthread_mutex_unlock(
        &thermal_state.mutex
    );

    if (thread_started)
    {
        pthread_join(
            thread,
            NULL
        );
    }

    pthread_mutex_lock(
        &thermal_state.mutex
    );

    thermal_state.initialized = 0;
    thermal_state.thread_started = 0;
    thermal_state.last_processing_slot_ns = 0;

    pthread_mutex_unlock(
        &thermal_state.mutex
    );

    if (email_enabled)
    {
        curl_global_cleanup();
    }
}

int thermal_manager_should_process_frame(void)
{
    long long now =
        current_monotonic_ns();

    if (now < 0)
    {
        return 1;
    }

    pthread_mutex_lock(
        &thermal_state.mutex
    );

    if (
        !thermal_state.initialized ||
        !thermal_state.config.enabled
    )
    {
        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        return 1;
    }

    int target_fps =
        thermal_state.status.target_fps;

    if (target_fps <= 0)
    {
        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        return 1;
    }

    long long interval_ns =
        1000000000LL /
        (long long)target_fps;

    int should_process = 0;

    if (
        thermal_state.
            last_processing_slot_ns == 0 ||
        now -
            thermal_state.
                last_processing_slot_ns >=
            interval_ns
    )
    {
        thermal_state.
            last_processing_slot_ns = now;

        should_process = 1;
    }

    pthread_mutex_unlock(
        &thermal_state.mutex
    );

    return should_process;
}

int thermal_manager_get_status(
    ThermalManagerStatus *status
)
{
    if (status == NULL)
    {
        return -1;
    }

    pthread_mutex_lock(
        &thermal_state.mutex
    );

    if (!thermal_state.initialized)
    {
        pthread_mutex_unlock(
            &thermal_state.mutex
        );

        return -1;
    }

    *status = thermal_state.status;

    pthread_mutex_unlock(
        &thermal_state.mutex
    );

    return 0;
}
