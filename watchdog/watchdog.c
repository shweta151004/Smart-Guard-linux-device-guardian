#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STUDENT_ID "device-001"

#define ADDRESS_SIZE 320
#define PASSWORD_SIZE 512
#define URL_SIZE 512
#define SERVICE_SIZE 128
#define MESSAGE_SIZE 512
#define BODY_SIZE 8192

typedef struct
{
    int enabled;
    int email_enabled;
    int stale_seconds;
    int poll_seconds;
    int startup_grace_seconds;
    int restart_retry_seconds;

    char heartbeat_path[PATH_MAX];
    char status_path[PATH_MAX];
    char target_service[SERVICE_SIZE];

    char smtp_url[URL_SIZE];
    char username[ADDRESS_SIZE];
    char password[PASSWORD_SIZE];
    char from_address[ADDRESS_SIZE];
    char to_address[ADDRESS_SIZE];
} WatchdogConfig;

typedef struct
{
    long pid;
    unsigned long long sequence;
    long long monotonic_ns;
    long long realtime_unix;
} FrameHeartbeat;

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static int parse_enabled_value(
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

static int load_configuration(
    WatchdogConfig *config
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
        parse_enabled_value(
            getenv("WATCHDOG_ENABLED"),
            1
        );

    config->email_enabled =
        parse_enabled_value(
            getenv("WATCHDOG_EMAIL_ENABLED"),
            1
        );

    config->stale_seconds =
        read_integer_environment(
            "WATCHDOG_STALE_SECONDS",
            30,
            5,
            3600
        );

    config->poll_seconds =
        read_integer_environment(
            "WATCHDOG_POLL_SECONDS",
            2,
            1,
            60
        );

    config->startup_grace_seconds =
        read_integer_environment(
            "WATCHDOG_STARTUP_GRACE_SECONDS",
            45,
            10,
            600
        );

    config->restart_retry_seconds =
        read_integer_environment(
            "WATCHDOG_RESTART_RETRY_SECONDS",
            60,
            30,
            3600
        );

    if (
        copy_environment_or_default(
            "WATCHDOG_HEARTBEAT_PATH",
            "/run/smart-guard/frame_heartbeat",
            config->heartbeat_path,
            sizeof(config->heartbeat_path)
        ) != 0 ||
        copy_environment_or_default(
            "WATCHDOG_STATUS_PATH",
            "/run/smart-guard-watchdog/status",
            config->status_path,
            sizeof(config->status_path)
        ) != 0 ||
        copy_environment_or_default(
            "WATCHDOG_TARGET_SERVICE",
            "smart-guard.service",
            config->target_service,
            sizeof(config->target_service)
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
            "%Y-%m-%d %H:%M:%S %z",
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

static int read_heartbeat(
    const char *path,
    FrameHeartbeat *heartbeat
)
{
    if (
        path == NULL ||
        heartbeat == NULL
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

    FrameHeartbeat result;

    memset(
        &result,
        0,
        sizeof(result)
    );

    int has_pid = 0;
    int has_sequence = 0;
    int has_monotonic = 0;
    int has_realtime = 0;

    char line[256];

    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    )
    {
        if (
            sscanf(
                line,
                "pid=%ld",
                &result.pid
            ) == 1
        )
        {
            has_pid = 1;
            continue;
        }

        if (
            sscanf(
                line,
                "sequence=%llu",
                &result.sequence
            ) == 1
        )
        {
            has_sequence = 1;
            continue;
        }

        if (
            sscanf(
                line,
                "monotonic_ns=%lld",
                &result.monotonic_ns
            ) == 1
        )
        {
            has_monotonic = 1;
            continue;
        }

        if (
            sscanf(
                line,
                "realtime_unix=%lld",
                &result.realtime_unix
            ) == 1
        )
        {
            has_realtime = 1;
        }
    }

    fclose(file);

    if (
        !has_pid ||
        !has_sequence ||
        !has_monotonic ||
        !has_realtime ||
        result.pid <= 0 ||
        result.sequence == 0 ||
        result.monotonic_ns <= 0
    )
    {
        return -1;
    }

    *heartbeat = result;

    return 0;
}

static int write_status(
    const WatchdogConfig *config,
    const char *status,
    long long heartbeat_age_seconds,
    const FrameHeartbeat *heartbeat,
    unsigned long long incident_count,
    int email_result,
    int restart_result,
    const char *message
)
{
    if (
        config == NULL ||
        status == NULL ||
        message == NULL
    )
    {
        return -1;
    }

    char temporary_path[
        PATH_MAX + 64
    ];

    int temporary_length = snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp.%ld",
        config->status_path,
        (long)getpid()
    );

    if (
        temporary_length < 0 ||
        (size_t)temporary_length >=
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
        "watchdog_pid=%ld\n"
        "target_service=%s\n"
        "stale_threshold_seconds=%d\n"
        "heartbeat_age_seconds=%lld\n"
        "heartbeat_pid=%ld\n"
        "heartbeat_sequence=%llu\n"
        "incident_count=%llu\n"
        "email_result=%d\n"
        "restart_result=%d\n"
        "message=%s\n",
        status,
        timestamp,
        (long)getpid(),
        config->target_service,
        config->stale_seconds,
        heartbeat_age_seconds,
        heartbeat != NULL
            ? heartbeat->pid
            : 0L,
        heartbeat != NULL
            ? heartbeat->sequence
            : 0ULL,
        incident_count,
        email_result,
        restart_result,
        message
    );

    if (
        fflush(file) != 0 ||
        fsync(fileno(file)) != 0 ||
        fclose(file) != 0
    )
    {
        unlink(temporary_path);
        return -1;
    }

    if (
        rename(
            temporary_path,
            config->status_path
        ) != 0
    )
    {
        unlink(temporary_path);
        return -1;
    }

    return 0;
}

static int send_watchdog_email(
    const WatchdogConfig *config,
    const FrameHeartbeat *heartbeat,
    long long heartbeat_age_seconds,
    const char *reason
)
{
    if (
        config == NULL ||
        reason == NULL ||
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

    char envelope_from[ADDRESS_SIZE + 4];
    char envelope_to[ADDRESS_SIZE + 4];
    char from_header[ADDRESS_SIZE + 32];
    char to_header[ADDRESS_SIZE + 8];

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
        "From: Smart Guard Watchdog <%s>",
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
        "Smart Guard software watchdog alert\n"
        "\n"
        "Student ID: %s\n"
        "Timestamp: %s\n"
        "Failure reason: %s\n"
        "No new processed frame threshold: %d seconds\n"
        "Observed heartbeat age: %lld seconds\n"
        "Last application PID: %ld\n"
        "Last frame sequence: %llu\n"
        "Heartbeat path: %s\n"
        "\n"
        "The watchdog is restarting %s automatically.\n",
        STUDENT_ID,
        timestamp,
        reason,
        config->stale_seconds,
        heartbeat_age_seconds,
        heartbeat != NULL
            ? heartbeat->pid
            : 0L,
        heartbeat != NULL
            ? heartbeat->sequence
            : 0ULL,
        config->heartbeat_path,
        config->target_service
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
        "Subject: Smart Guard Watchdog Alert - Frame Processing Stalled"
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

#define SET_CURL_OPTION(option, value) \
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

    SET_CURL_OPTION(
        CURLOPT_URL,
        config->smtp_url
    );

    SET_CURL_OPTION(
        CURLOPT_USERNAME,
        config->username
    );

    SET_CURL_OPTION(
        CURLOPT_PASSWORD,
        config->password
    );

    SET_CURL_OPTION(
        CURLOPT_USE_SSL,
        CURLUSESSL_ALL
    );

    SET_CURL_OPTION(
        CURLOPT_MAIL_FROM,
        envelope_from
    );

    SET_CURL_OPTION(
        CURLOPT_MAIL_RCPT,
        recipients
    );

    SET_CURL_OPTION(
        CURLOPT_HTTPHEADER,
        headers
    );

    SET_CURL_OPTION(
        CURLOPT_MIMEPOST,
        mime
    );

    SET_CURL_OPTION(
        CURLOPT_CONNECTTIMEOUT,
        10L
    );

    SET_CURL_OPTION(
        CURLOPT_TIMEOUT,
        20L
    );

    SET_CURL_OPTION(
        CURLOPT_NOSIGNAL,
        1L
    );

    SET_CURL_OPTION(
        CURLOPT_SSL_VERIFYPEER,
        1L
    );

    SET_CURL_OPTION(
        CURLOPT_SSL_VERIFYHOST,
        2L
    );

#undef SET_CURL_OPTION

    result = curl_easy_perform(curl);

cleanup:

    if (result != CURLE_OK)
    {
        fprintf(
            stderr,
            "Watchdog email failed: %s\n",
            curl_easy_strerror(result)
        );
    }

    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    return result == CURLE_OK ? 0 : -1;
}

static int restart_target_service(
    const char *service_name
)
{
    if (
        service_name == NULL ||
        service_name[0] == '\0'
    )
    {
        return -1;
    }

    pid_t child = fork();

    if (child < 0)
    {
        return -1;
    }

    if (child == 0)
    {
        execl(
            "/usr/bin/systemctl",
            "systemctl",
            "restart",
            service_name,
            (char *)NULL
        );

        _exit(127);
    }

    int status = 0;

    while (
        waitpid(
            child,
            &status,
            0
        ) < 0
    )
    {
        if (errno == EINTR)
        {
            continue;
        }

        return -1;
    }

    if (
        !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0
    )
    {
        return -1;
    }

    return 0;
}

static void sleep_interruptibly(int seconds)
{
    struct timespec delay;

    delay.tv_sec = seconds;
    delay.tv_nsec = 0;

    while (
        keep_running &&
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

int main(void)
{
    setvbuf(
        stdout,
        NULL,
        _IOLBF,
        0
    );

    setvbuf(
        stderr,
        NULL,
        _IOLBF,
        0
    );

    signal(
        SIGINT,
        handle_signal
    );

    signal(
        SIGTERM,
        handle_signal
    );

    WatchdogConfig config;

    if (
        load_configuration(
            &config
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Watchdog configuration is incomplete.\n"
        );

        return EXIT_FAILURE;
    }

    if (
        config.email_enabled &&
        curl_global_init(
            CURL_GLOBAL_DEFAULT
        ) != CURLE_OK
    )
    {
        fprintf(
            stderr,
            "Watchdog curl initialization failed.\n"
        );

        return EXIT_FAILURE;
    }

    printf(
        "Smart Guard watchdog started: "
        "heartbeat=%s threshold=%d seconds "
        "target=%s email=%s\n",
        config.heartbeat_path,
        config.stale_seconds,
        config.target_service,
        config.email_enabled
            ? "enabled"
            : "disabled"
    );

    long long startup_time =
        current_monotonic_ns();

    int incident_active = 0;

    unsigned long long incident_count = 0;

    int last_email_result = -2;
    int last_restart_result = -2;

    long long last_restart_time = 0;

    FrameHeartbeat last_heartbeat;

    memset(
        &last_heartbeat,
        0,
        sizeof(last_heartbeat)
    );

    write_status(
        &config,
        config.enabled
            ? "starting"
            : "disabled",
        -1,
        NULL,
        incident_count,
        last_email_result,
        last_restart_result,
        config.enabled
            ? "Waiting for frame heartbeat"
            : "Watchdog disabled by configuration"
    );

    while (keep_running)
    {
        if (!config.enabled)
        {
            sleep_interruptibly(
                config.poll_seconds
            );

            continue;
        }

        long long now =
            current_monotonic_ns();

        FrameHeartbeat heartbeat;

        memset(
            &heartbeat,
            0,
            sizeof(heartbeat)
        );

        int heartbeat_result =
            read_heartbeat(
                config.heartbeat_path,
                &heartbeat
            );

        long long heartbeat_age_seconds =
            -1;

        int heartbeat_fresh = 0;

        if (
            heartbeat_result == 0 &&
            now >= heartbeat.monotonic_ns
        )
        {
            heartbeat_age_seconds =
                (
                    now -
                    heartbeat.monotonic_ns
                ) / 1000000000LL;

            if (
                heartbeat_age_seconds <=
                    config.stale_seconds
            )
            {
                heartbeat_fresh = 1;
                last_heartbeat = heartbeat;
            }
        }

        if (heartbeat_fresh)
        {
            if (incident_active)
            {
                printf(
                    "Watchdog recovery detected: "
                    "pid=%ld sequence=%llu\n",
                    heartbeat.pid,
                    heartbeat.sequence
                );
            }

            incident_active = 0;

            last_email_result = -2;
            last_restart_result = -2;

            write_status(
                &config,
                "healthy",
                heartbeat_age_seconds,
                &heartbeat,
                incident_count,
                last_email_result,
                last_restart_result,
                "Frame heartbeat is current"
            );
        }
        else
        {
            long long startup_age_seconds =
                startup_time > 0 &&
                now >= startup_time
                    ? (
                        now -
                        startup_time
                    ) / 1000000000LL
                    : 0;

            if (
                startup_age_seconds <
                config.startup_grace_seconds
            )
            {
                write_status(
                    &config,
                    "starting",
                    heartbeat_age_seconds,
                    heartbeat_result == 0
                        ? &heartbeat
                        : NULL,
                    incident_count,
                    last_email_result,
                    last_restart_result,
                    "Heartbeat is inside startup grace period"
                );
            }
            else if (!incident_active)
            {
                const char *reason =
                    heartbeat_result == 0
                        ? "heartbeat_stale"
                        : "heartbeat_missing";

                incident_active = 1;
                incident_count++;

                printf(
                    "Watchdog stale frame detected: "
                    "reason=%s age=%lld threshold=%d "
                    "pid=%ld sequence=%llu\n",
                    reason,
                    heartbeat_age_seconds,
                    config.stale_seconds,
                    heartbeat_result == 0
                        ? heartbeat.pid
                        : last_heartbeat.pid,
                    heartbeat_result == 0
                        ? heartbeat.sequence
                        : last_heartbeat.sequence
                );

                write_status(
                    &config,
                    "stale",
                    heartbeat_age_seconds,
                    heartbeat_result == 0
                        ? &heartbeat
                        : &last_heartbeat,
                    incident_count,
                    last_email_result,
                    last_restart_result,
                    "No new processed frame was detected"
                );

                if (config.email_enabled)
                {
                    last_email_result =
                        send_watchdog_email(
                            &config,
                            heartbeat_result == 0
                                ? &heartbeat
                                : &last_heartbeat,
                            heartbeat_age_seconds,
                            reason
                        );

                    if (last_email_result == 0)
                    {
                        printf(
                            "Watchdog email sent successfully.\n"
                        );
                    }
                }
                else
                {
                    last_email_result = 1;

                    printf(
                        "Watchdog email is disabled.\n"
                    );
                }

                write_status(
                    &config,
                    "restarting",
                    heartbeat_age_seconds,
                    heartbeat_result == 0
                        ? &heartbeat
                        : &last_heartbeat,
                    incident_count,
                    last_email_result,
                    last_restart_result,
                    "Restarting Smart Guard service"
                );

                last_restart_result =
                    restart_target_service(
                        config.target_service
                    );

                last_restart_time =
                    current_monotonic_ns();

                if (last_restart_result == 0)
                {
                    printf(
                        "Watchdog restart requested successfully: "
                        "service=%s\n",
                        config.target_service
                    );
                }
                else
                {
                    fprintf(
                        stderr,
                        "Watchdog service restart failed: "
                        "service=%s\n",
                        config.target_service
                    );
                }

                write_status(
                    &config,
                    last_restart_result == 0
                        ? "recovering"
                        : "error",
                    heartbeat_age_seconds,
                    heartbeat_result == 0
                        ? &heartbeat
                        : &last_heartbeat,
                    incident_count,
                    last_email_result,
                    last_restart_result,
                    last_restart_result == 0
                        ? "Waiting for new frames after restart"
                        : "Service restart command failed"
                );
            }
            else if (
                last_restart_time > 0 &&
                now >= last_restart_time &&
                (
                    now -
                    last_restart_time
                ) / 1000000000LL >=
                    config.restart_retry_seconds
            )
            {
                printf(
                    "Watchdog retrying service restart: "
                    "service=%s\n",
                    config.target_service
                );

                last_restart_result =
                    restart_target_service(
                        config.target_service
                    );

                last_restart_time =
                    current_monotonic_ns();

                write_status(
                    &config,
                    last_restart_result == 0
                        ? "recovering"
                        : "error",
                    heartbeat_age_seconds,
                    &last_heartbeat,
                    incident_count,
                    last_email_result,
                    last_restart_result,
                    last_restart_result == 0
                        ? "Restart retry completed"
                        : "Restart retry failed"
                );
            }
        }

        sleep_interruptibly(
            config.poll_seconds
        );
    }

    write_status(
        &config,
        "stopped",
        -1,
        &last_heartbeat,
        incident_count,
        last_email_result,
        last_restart_result,
        "Watchdog stopped"
    );

    if (config.email_enabled)
    {
        curl_global_cleanup();
    }

    printf(
        "Smart Guard watchdog stopped.\n"
    );

    return EXIT_SUCCESS;
}
