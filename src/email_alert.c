#define _POSIX_C_SOURCE 200809L

#include "email_alert.h"

#include <curl/curl.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define EMAIL_DEBOUNCE_SECONDS 30
#define EMAIL_TEXT_SIZE 1024
#define EMAIL_ADDRESS_SIZE 320
#define EMAIL_URL_SIZE 512
#define EMAIL_PASSWORD_SIZE 512
#define EMAIL_TIMESTAMP_SIZE 64

typedef struct
{
    int persons;
    char timestamp[EMAIL_TIMESTAMP_SIZE];
    double cpu_temperature_c;
    unsigned char *jpeg_data;
    size_t jpeg_size;
} EmailJob;

typedef struct
{
    int initialized;
    int enabled;
    int running;
    int worker_started;
    int has_pending;
    int has_last_accepted;
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct timespec last_accepted;
    EmailJob pending;
    char smtp_url[EMAIL_URL_SIZE];
    char username[EMAIL_ADDRESS_SIZE];
    char password[EMAIL_PASSWORD_SIZE];
    char from_address[EMAIL_ADDRESS_SIZE];
    char to_address[EMAIL_ADDRESS_SIZE];
} EmailState;

static EmailState email_state =
{
    .initialized = 0,
    .enabled = 0,
    .running = 0,
    .worker_started = 0,
    .has_pending = 0,
    .has_last_accepted = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER
};

static int is_enabled_value(const char *value)
{
    if (value == NULL)
    {
        return 0;
    }

    return
        strcmp(value, "1") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0;
}

static int copy_environment_value(
    const char *name,
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

static void free_job(EmailJob *job)
{
    if (job == NULL)
    {
        return;
    }

    free(job->jpeg_data);

    memset(
        job,
        0,
        sizeof(*job)
    );
}

static double elapsed_seconds(
    const struct timespec *start,
    const struct timespec *end
)
{
    double seconds =
        (double)(
            end->tv_sec -
            start->tv_sec
        );

    double nanoseconds =
        (double)(
            end->tv_nsec -
            start->tv_nsec
        ) / 1000000000.0;

    return seconds + nanoseconds;
}

static int send_email(const EmailJob *job)
{
    if (
        job == NULL ||
        job->jpeg_data == NULL ||
        job->jpeg_size == 0
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

    char envelope_from[EMAIL_ADDRESS_SIZE + 4];
    char envelope_to[EMAIL_ADDRESS_SIZE + 4];
    char from_header[EMAIL_ADDRESS_SIZE + 32];
    char to_header[EMAIL_ADDRESS_SIZE + 8];
    char subject_header[256];
    char body[EMAIL_TEXT_SIZE];

    snprintf(
        envelope_from,
        sizeof(envelope_from),
        "<%s>",
        email_state.from_address
    );

    snprintf(
        envelope_to,
        sizeof(envelope_to),
        "<%s>",
        email_state.to_address
    );

    snprintf(
        from_header,
        sizeof(from_header),
        "From: Smart Guard <%s>",
        email_state.from_address
    );

    snprintf(
        to_header,
        sizeof(to_header),
        "To: %s",
        email_state.to_address
    );

    snprintf(
        subject_header,
        sizeof(subject_header),
        "Subject: Smart Guard Alert - %d person(s)",
        job->persons
    );

    if (
        job->cpu_temperature_c > 0.0 &&
        job->cpu_temperature_c < 200.0
    )
    {
        snprintf(
            body,
            sizeof(body),
            "Smart Guard human detection alert\n"
            "\n"
            "Student ID: device-001\n"
            "Detected persons: %d\n"
            "Timestamp: %s\n"
            "CPU temperature: %.1f C\n"
            "\n"
            "The detection frame is attached to this email.\n",
            job->persons,
            job->timestamp,
            job->cpu_temperature_c
        );
    }
    else
    {
        snprintf(
            body,
            sizeof(body),
            "Smart Guard human detection alert\n"
            "\n"
            "Student ID: device-001\n"
            "Detected persons: %d\n"
            "Timestamp: %s\n"
            "CPU temperature: unavailable\n"
            "\n"
            "The detection frame is attached to this email.\n",
            job->persons,
            job->timestamp
        );
    }

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
        subject_header
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

    part = curl_mime_addpart(mime);

    if (part == NULL)
    {
        result = CURLE_OUT_OF_MEMORY;
        goto cleanup;
    }

    result = curl_mime_data(
        part,
        (const char *)job->jpeg_data,
        job->jpeg_size
    );

    if (result != CURLE_OK)
    {
        goto cleanup;
    }

    result = curl_mime_filename(
        part,
        "smart_guard_detection.jpg"
    );

    if (result != CURLE_OK)
    {
        goto cleanup;
    }

    result = curl_mime_type(
        part,
        "image/jpeg"
    );

    if (result != CURLE_OK)
    {
        goto cleanup;
    }

    result = curl_mime_encoder(
        part,
        "base64"
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
        email_state.smtp_url
    );

    SET_CURL_OPTION(
        CURLOPT_USERNAME,
        email_state.username
    );

    SET_CURL_OPTION(
        CURLOPT_PASSWORD,
        email_state.password
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
            "Email alert failed: %s\n",
            curl_easy_strerror(result)
        );
    }

    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    return result == CURLE_OK ? 0 : -1;
}

static void *email_worker(void *argument)
{
    (void)argument;

    while (1)
    {
        pthread_mutex_lock(
            &email_state.mutex
        );

        while (
            email_state.running &&
            !email_state.has_pending
        )
        {
            pthread_cond_wait(
                &email_state.condition,
                &email_state.mutex
            );
        }

        if (!email_state.running)
        {
            pthread_mutex_unlock(
                &email_state.mutex
            );

            break;
        }

        EmailJob job =
            email_state.pending;

        memset(
            &email_state.pending,
            0,
            sizeof(email_state.pending)
        );

        email_state.has_pending = 0;

        pthread_mutex_unlock(
            &email_state.mutex
        );

        if (send_email(&job) == 0)
        {
            printf(
                "Email alert sent successfully: persons=%d timestamp=%s\n",
                job.persons,
                job.timestamp
            );
        }

        free_job(&job);
    }

    return NULL;
}

int email_alert_initialize(void)
{
    if (email_state.initialized)
    {
        return 0;
    }

    email_state.initialized = 1;

    if (
        !is_enabled_value(
            getenv("EMAIL_ENABLED")
        )
    )
    {
        email_state.enabled = 0;

        printf(
            "Email alerts are disabled.\n"
        );

        return 0;
    }

    if (
        copy_environment_value(
            "EMAIL_SMTP_URL",
            email_state.smtp_url,
            sizeof(email_state.smtp_url)
        ) != 0 ||
        copy_environment_value(
            "EMAIL_USERNAME",
            email_state.username,
            sizeof(email_state.username)
        ) != 0 ||
        copy_environment_value(
            "EMAIL_PASSWORD",
            email_state.password,
            sizeof(email_state.password)
        ) != 0 ||
        copy_environment_value(
            "EMAIL_FROM",
            email_state.from_address,
            sizeof(email_state.from_address)
        ) != 0 ||
        copy_environment_value(
            "EMAIL_TO",
            email_state.to_address,
            sizeof(email_state.to_address)
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Email alert configuration is incomplete.\n"
        );

        return -1;
    }

    if (
        curl_global_init(
            CURL_GLOBAL_DEFAULT
        ) != CURLE_OK
    )
    {
        fprintf(
            stderr,
            "Email curl initialization failed.\n"
        );

        return -1;
    }

    email_state.enabled = 1;
    email_state.running = 1;

    if (
        pthread_create(
            &email_state.worker,
            NULL,
            email_worker,
            NULL
        ) != 0
    )
    {
        email_state.running = 0;
        email_state.enabled = 0;

        curl_global_cleanup();

        fprintf(
            stderr,
            "Email worker thread could not start.\n"
        );

        return -1;
    }

    email_state.worker_started = 1;

    printf(
        "Email alerts initialized: recipient=%s debounce=%d seconds\n",
        email_state.to_address,
        EMAIL_DEBOUNCE_SECONDS
    );

    return 0;
}

int email_alert_queue(
    int persons,
    const char *timestamp,
    double cpu_temperature_c,
    const unsigned char *jpeg_data,
    size_t jpeg_size
)
{
    if (
        !email_state.initialized ||
        !email_state.enabled ||
        !email_state.running
    )
    {
        return -1;
    }

    if (
        persons <= 0 ||
        timestamp == NULL ||
        jpeg_data == NULL ||
        jpeg_size == 0
    )
    {
        return -1;
    }

    unsigned char *jpeg_copy = malloc(
        jpeg_size
    );

    if (jpeg_copy == NULL)
    {
        return -1;
    }

    memcpy(
        jpeg_copy,
        jpeg_data,
        jpeg_size
    );

    struct timespec current_time;

    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &current_time
        ) != 0
    )
    {
        free(jpeg_copy);
        return -1;
    }

    pthread_mutex_lock(
        &email_state.mutex
    );

    if (
        email_state.has_pending ||
        (
            email_state.has_last_accepted &&
            elapsed_seconds(
                &email_state.last_accepted,
                &current_time
            ) < EMAIL_DEBOUNCE_SECONDS
        )
    )
    {
        pthread_mutex_unlock(
            &email_state.mutex
        );

        free(jpeg_copy);

        return 1;
    }

    email_state.pending.persons =
        persons;

    email_state.pending.cpu_temperature_c =
        cpu_temperature_c;

    email_state.pending.jpeg_data =
        jpeg_copy;

    email_state.pending.jpeg_size =
        jpeg_size;

    snprintf(
        email_state.pending.timestamp,
        sizeof(email_state.pending.timestamp),
        "%s",
        timestamp
    );

    email_state.last_accepted =
        current_time;

    email_state.has_last_accepted = 1;
    email_state.has_pending = 1;

    pthread_cond_signal(
        &email_state.condition
    );

    pthread_mutex_unlock(
        &email_state.mutex
    );

    printf(
        "Email alert queued: persons=%d timestamp=%s\n",
        persons,
        timestamp
    );

    return 0;
}


int email_alert_queue_immediate(
    int persons,
    const char *timestamp,
    double cpu_temperature_c,
    const unsigned char *jpeg_data,
    size_t jpeg_size
)
{
    if (
        !email_state.initialized ||
        !email_state.enabled ||
        !email_state.running
    )
    {
        return -1;
    }

    if (
        persons <= 0 ||
        timestamp == NULL ||
        jpeg_data == NULL ||
        jpeg_size == 0
    )
    {
        return -1;
    }

    unsigned char *jpeg_copy = malloc(
        jpeg_size
    );

    if (jpeg_copy == NULL)
    {
        return -1;
    }

    memcpy(
        jpeg_copy,
        jpeg_data,
        jpeg_size
    );

    struct timespec current_time;

    if (
        clock_gettime(
            CLOCK_MONOTONIC,
            &current_time
        ) != 0
    )
    {
        free(jpeg_copy);
        return -1;
    }

    pthread_mutex_lock(
        &email_state.mutex
    );

    if (email_state.has_pending)
    {
        pthread_mutex_unlock(
            &email_state.mutex
        );

        free(jpeg_copy);

        return 1;
    }

    email_state.pending.persons =
        persons;

    email_state.pending.cpu_temperature_c =
        cpu_temperature_c;

    email_state.pending.jpeg_data =
        jpeg_copy;

    email_state.pending.jpeg_size =
        jpeg_size;

    snprintf(
        email_state.pending.timestamp,
        sizeof(email_state.pending.timestamp),
        "%s",
        timestamp
    );

    email_state.last_accepted =
        current_time;

    email_state.has_last_accepted = 1;
    email_state.has_pending = 1;

    pthread_cond_signal(
        &email_state.condition
    );

    pthread_mutex_unlock(
        &email_state.mutex
    );

    printf(
        "Immediate guard email queued: persons=%d timestamp=%s\n",
        persons,
        timestamp
    );

    return 0;
}

void email_alert_shutdown(void)
{
    if (!email_state.initialized)
    {
        return;
    }

    if (email_state.worker_started)
    {
        pthread_mutex_lock(
            &email_state.mutex
        );

        email_state.running = 0;

        if (email_state.has_pending)
        {
            free_job(
                &email_state.pending
            );

            email_state.has_pending = 0;
        }

        pthread_cond_broadcast(
            &email_state.condition
        );

        pthread_mutex_unlock(
            &email_state.mutex
        );

        pthread_join(
            email_state.worker,
            NULL
        );

        email_state.worker_started = 0;
    }

    if (email_state.enabled)
    {
        curl_global_cleanup();
    }

    email_state.enabled = 0;
    email_state.running = 0;
}
