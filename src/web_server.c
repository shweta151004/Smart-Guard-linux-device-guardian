#define _POSIX_C_SOURCE 200809L

#include "web_server.h"
#include "camera_stream.h"
#include "detection_history.h"
#include "guard_mode.h"
#include "image_processor.h"
#include "telemetry.h"
#include "thermal_manager.h"

#include <json-c/json.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STUDENT_ID "device-001"
#define MAX_REQUEST_BODY 4096

typedef struct
{
    unsigned char *buffer;
    size_t buffer_size;
    size_t buffer_offset;
    unsigned long long last_sequence;
} MjpegClient;

typedef struct
{
    char *body;
    size_t body_size;
    int too_large;
} RequestContext;

static char *tls_certificate_data = NULL;
static char *tls_private_key_data = NULL;
static unsigned short redirect_https_port = 0;

static void create_timestamp(
    char *buffer,
    size_t buffer_size
)
{
    time_t current_time = time(NULL);
    struct tm local_time;

    if (
        localtime_r(
            &current_time,
            &local_time
        ) == NULL
    )
    {
        snprintf(
            buffer,
            buffer_size,
            "unavailable"
        );

        return;
    }

    if (
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

static char *read_entire_file(
    const char *path,
    size_t *file_size
)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL)
    {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return NULL;
    }

    long length = ftell(file);

    if (length < 0)
    {
        fclose(file);
        return NULL;
    }

    rewind(file);

    char *buffer = malloc(
        (size_t)length + 1
    );

    if (buffer == NULL)
    {
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(
        buffer,
        1,
        (size_t)length,
        file
    );

    fclose(file);

    if (bytes_read != (size_t)length)
    {
        free(buffer);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    *file_size = bytes_read;

    return buffer;
}

static enum MHD_Result send_response(
    struct MHD_Connection *connection,
    unsigned int status_code,
    const char *content_type,
    const char *body
)
{
    struct MHD_Response *response =
        MHD_create_response_from_buffer(
            strlen(body),
            (void *)body,
            MHD_RESPMEM_MUST_COPY
        );

    if (response == NULL)
    {
        return MHD_NO;
    }

    MHD_add_response_header(
        response,
        "Content-Type",
        content_type
    );

    MHD_add_response_header(
        response,
        "Cache-Control",
        "no-store"
    );

    MHD_add_response_header(
        response,
        "X-Content-Type-Options",
        "nosniff"
    );

    enum MHD_Result result =
        MHD_queue_response(
            connection,
            status_code,
            response
        );

    MHD_destroy_response(response);

    return result;
}

static enum MHD_Result send_json_object(
    struct MHD_Connection *connection,
    unsigned int status_code,
    struct json_object *root
)
{
    const char *json_text =
        json_object_to_json_string_ext(
            root,
            JSON_C_TO_STRING_PLAIN
        );

    return send_response(
        connection,
        status_code,
        "application/json; charset=utf-8",
        json_text
    );
}

static enum MHD_Result send_index_page(
    struct MHD_Connection *connection,
    const char *index_file
)
{
    size_t file_size = 0;

    char *html = read_entire_file(
        index_file,
        &file_size
    );

    if (html == NULL)
    {
        return send_response(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "text/plain; charset=utf-8",
            "Could not read web/index.html\n"
        );
    }

    struct MHD_Response *response =
        MHD_create_response_from_buffer(
            file_size,
            html,
            MHD_RESPMEM_MUST_FREE
        );

    if (response == NULL)
    {
        free(html);
        return MHD_NO;
    }

    MHD_add_response_header(
        response,
        "Content-Type",
        "text/html; charset=utf-8"
    );

    MHD_add_response_header(
        response,
        "Cache-Control",
        "no-store"
    );

    enum MHD_Result result =
        MHD_queue_response(
            connection,
            MHD_HTTP_OK,
            response
        );

    MHD_destroy_response(response);

    return result;
}

static enum MHD_Result send_metric_page(
    struct MHD_Connection *connection,
    const char *value
)
{
    char body[1024];

    int length = snprintf(
        body,
        sizeof(body),
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta http-equiv=\"refresh\" content=\"2\">"
        "<style>"
        "html,body{margin:0;padding:0;background:transparent;"
        "color:#f9fafb;font-family:Arial,sans-serif;"
        "overflow:hidden;}"
        ".value{font-size:28px;font-weight:bold;"
        "padding-top:2px;white-space:nowrap;}"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"value\">%s</div>"
        "</body>"
        "</html>",
        value
    );

    if (
        length < 0 ||
        (size_t)length >= sizeof(body)
    )
    {
        return MHD_NO;
    }

    return send_response(
        connection,
        MHD_HTTP_OK,
        "text/html; charset=utf-8",
        body
    );
}

static enum MHD_Result send_temperature_view(
    struct MHD_Connection *connection
)
{
    double temperature = 0.0;
    char value[128];

    if (
        telemetry_read_cpu_temperature(
            &temperature
        ) != 0
    )
    {
        snprintf(
            value,
            sizeof(value),
            "-- °C"
        );
    }
    else
    {
        snprintf(
            value,
            sizeof(value),
            "%.1f °C",
            temperature
        );
    }

    return send_metric_page(
        connection,
        value
    );
}

static enum MHD_Result send_memory_view(
    struct MHD_Connection *connection
)
{
    unsigned long memory_kb = 0;
    char value[128];

    if (
        telemetry_read_memory_available(
            &memory_kb
        ) != 0
    )
    {
        snprintf(
            value,
            sizeof(value),
            "-- MB"
        );
    }
    else
    {
        snprintf(
            value,
            sizeof(value),
            "%.1f MB",
            (double)memory_kb / 1024.0
        );
    }

    return send_metric_page(
        connection,
        value
    );
}

static enum MHD_Result send_cpu_view(
    struct MHD_Connection *connection
)
{
    double cpu_usage = 0.0;
    char value[128];

    if (
        telemetry_read_cpu_usage(
            &cpu_usage
        ) != 0
    )
    {
        snprintf(
            value,
            sizeof(value),
            "-- %%"
        );
    }
    else
    {
        snprintf(
            value,
            sizeof(value),
            "%.1f %%",
            cpu_usage
        );
    }

    return send_metric_page(
        connection,
        value
    );
}

static enum MHD_Result send_persons_view(
    struct MHD_Connection *connection
)
{
    char value[64];

    snprintf(
        value,
        sizeof(value),
        "%d",
        image_processor_get_person_count()
    );

    return send_metric_page(
        connection,
        value
    );
}

static enum MHD_Result send_persons_api(
    struct MHD_Connection *connection
)
{
    char timestamp[64];

    create_timestamp(
        timestamp,
        sizeof(timestamp)
    );

    struct json_object *root =
        json_object_new_object();

    if (root == NULL)
    {
        return send_response(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "application/json; charset=utf-8",
            "{\"status\":\"error\"}"
        );
    }

    json_object_object_add(
        root,
        "status",
        json_object_new_string("ok")
    );

    json_object_object_add(
        root,
        "student_id",
        json_object_new_string(STUDENT_ID)
    );

    json_object_object_add(
        root,
        "persons",
        json_object_new_int(
            image_processor_get_person_count()
        )
    );

    json_object_object_add(
        root,
        "timestamp",
        json_object_new_string(timestamp)
    );

    enum MHD_Result result =
        send_json_object(
            connection,
            MHD_HTTP_OK,
            root
        );

    json_object_put(root);

    return result;
}

static enum MHD_Result send_telemetry_api(
    struct MHD_Connection *connection
)
{
    TelemetryData data;

    if (telemetry_read_all(&data) != 0)
    {
        return send_response(
            connection,
            MHD_HTTP_SERVICE_UNAVAILABLE,
            "application/json; charset=utf-8",
            "{\"status\":\"error\","
            "\"message\":\"Telemetry reading failed\"}"
        );
    }

    char timestamp[64];

    create_timestamp(
        timestamp,
        sizeof(timestamp)
    );

    struct json_object *root =
        json_object_new_object();

    if (root == NULL)
    {
        return send_response(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "application/json; charset=utf-8",
            "{\"status\":\"error\"}"
        );
    }

    json_object_object_add(
        root,
        "status",
        json_object_new_string("ok")
    );

    json_object_object_add(
        root,
        "student_id",
        json_object_new_string(STUDENT_ID)
    );

    json_object_object_add(
        root,
        "cpu_temperature_c",
        json_object_new_double(
            data.cpu_temperature_c
        )
    );

    json_object_object_add(
        root,
        "memory_available_kb",
        json_object_new_int64(
            (int64_t)data.memory_available_kb
        )
    );

    json_object_object_add(
        root,
        "memory_available_mb",
        json_object_new_double(
            (double)data.memory_available_kb /
            1024.0
        )
    );

    json_object_object_add(
        root,
        "cpu_usage_percent",
        json_object_new_double(
            data.cpu_usage_percent
        )
    );

    json_object_object_add(
        root,
        "timestamp",
        json_object_new_string(timestamp)
    );

    enum MHD_Result result =
        send_json_object(
            connection,
            MHD_HTTP_OK,
            root
        );

    json_object_put(root);

    return result;
}

static enum MHD_Result send_thermal_api(
    struct MHD_Connection *connection
)
{
    ThermalManagerStatus status;

    if (
        thermal_manager_get_status(
            &status
        ) != 0
    )
    {
        return send_response(
            connection,
            MHD_HTTP_SERVICE_UNAVAILABLE,
            "application/json; charset=utf-8",
            "{\"status\":\"error\","
            "\"message\":\"Thermal manager unavailable\"}"
        );
    }

    struct json_object *root =
        json_object_new_object();

    if (root == NULL)
    {
        return send_response(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "application/json; charset=utf-8",
            "{\"status\":\"error\"}"
        );
    }

    json_object_object_add(
        root,
        "status",
        json_object_new_string("ok")
    );

    json_object_object_add(
        root,
        "student_id",
        json_object_new_string(STUDENT_ID)
    );

    json_object_object_add(
        root,
        "thermal_status",
        json_object_new_string(
            status.status
        )
    );

    json_object_object_add(
        root,
        "enabled",
        json_object_new_boolean(
            status.enabled
        )
    );

    json_object_object_add(
        root,
        "sensor_available",
        json_object_new_boolean(
            status.sensor_available
        )
    );

    json_object_object_add(
        root,
        "throttled",
        json_object_new_boolean(
            status.throttled
        )
    );

    json_object_object_add(
        root,
        "cpu_temperature_c",
        json_object_new_double(
            status.current_temperature_c
        )
    );

    json_object_object_add(
        root,
        "high_threshold_c",
        json_object_new_double(
            status.high_threshold_c
        )
    );

    json_object_object_add(
        root,
        "recovery_threshold_c",
        json_object_new_double(
            status.recovery_threshold_c
        )
    );

    json_object_object_add(
        root,
        "normal_fps",
        json_object_new_int(
            status.normal_fps
        )
    );

    json_object_object_add(
        root,
        "reduced_fps",
        json_object_new_int(
            status.reduced_fps
        )
    );

    json_object_object_add(
        root,
        "target_fps",
        json_object_new_int(
            status.target_fps
        )
    );

    json_object_object_add(
        root,
        "incident_count",
        json_object_new_int64(
            (int64_t)status.incident_count
        )
    );

    json_object_object_add(
        root,
        "last_email_result",
        json_object_new_int(
            status.last_email_result
        )
    );

    json_object_object_add(
        root,
        "last_transition_timestamp",
        json_object_new_string(
            status.last_transition_timestamp
        )
    );

    json_object_object_add(
        root,
        "message",
        json_object_new_string(
            status.message
        )
    );

    enum MHD_Result result =
        send_json_object(
            connection,
            MHD_HTTP_OK,
            root
        );

    json_object_put(root);

    return result;
}

static enum MHD_Result send_thermal_view(
    struct MHD_Connection *connection
)
{
    ThermalManagerStatus status;

    if (
        thermal_manager_get_status(
            &status
        ) != 0
    )
    {
        return send_response(
            connection,
            MHD_HTTP_SERVICE_UNAVAILABLE,
            "text/html; charset=utf-8",
            "<html><body>Thermal manager unavailable"
            "</body></html>"
        );
    }

    char body[4096];

    snprintf(
        body,
        sizeof(body),
        "<!doctype html>"
        "<html><head>"
        "<meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"2\">"
        "<style>"
        "body{font-family:sans-serif;text-align:center;"
        "margin:8px;background:#111827;color:#f9fafb}"
        ".value{font-size:1.35rem;font-weight:700}"
        ".normal{color:#34d399}"
        ".throttled{color:#f87171}"
        ".small{font-size:.8rem;color:#cbd5e1}"
        "</style></head><body>"
        "<div class=\"value %s\">%s</div>"
        "<div>%.1f C</div>"
        "<div>Target processing: %d FPS</div>"
        "<div class=\"small\">High %.1f C | "
        "Recovery %.1f C | Incidents %llu</div>"
        "</body></html>",
        status.throttled
            ? "throttled"
            : "normal",
        status.throttled
            ? "THERMAL THROTTLING"
            : "NORMAL",
        status.current_temperature_c,
        status.target_fps,
        status.high_threshold_c,
        status.recovery_threshold_c,
        status.incident_count
    );

    return send_response(
        connection,
        MHD_HTTP_OK,
        "text/html; charset=utf-8",
        body
    );
}


static void add_history_report_json(
    struct json_object *target,
    const DetectionHistoryReport *report
)
{
    json_object_object_add(
        target,
        "total_detection_events",
        json_object_new_int64(
            report->total_detection_events
        )
    );

    json_object_object_add(
        target,
        "total_persons_detected",
        json_object_new_int64(
            report->total_persons_detected
        )
    );

    json_object_object_add(
        target,
        "first_detection_timestamp",
        report->has_first_detection
            ? json_object_new_string(
                report->
                first_detection_timestamp
            )
            : json_object_new_null()
    );

    json_object_object_add(
        target,
        "last_detection_timestamp",
        report->has_last_detection
            ? json_object_new_string(
                report->
                last_detection_timestamp
            )
            : json_object_new_null()
    );
}

static enum MHD_Result send_history_api(
    struct MHD_Connection *connection
)
{
    DetectionHistoryRecord records[
        DETECTION_HISTORY_LIMIT
    ];

    size_t record_count =
        detection_history_get(
            records,
            DETECTION_HISTORY_LIMIT
        );

    DetectionHistoryReport report;

    if (
        detection_history_get_report(
            &report
        ) != 0
    )
    {
        return send_response(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "application/json; charset=utf-8",
            "{\"status\":\"error\","
            "\"message\":\"History report unavailable\"}"
        );
    }

    struct json_object *root =
        json_object_new_object();

    struct json_object *history =
        json_object_new_array();

    struct json_object *report_json =
        json_object_new_object();

    if (
        root == NULL ||
        history == NULL ||
        report_json == NULL
    )
    {
        if (root != NULL)
        {
            json_object_put(root);
        }

        if (history != NULL)
        {
            json_object_put(history);
        }

        if (report_json != NULL)
        {
            json_object_put(report_json);
        }

        return send_response(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "application/json; charset=utf-8",
            "{\"status\":\"error\"}"
        );
    }

    for (
        size_t index = 0;
        index < record_count;
        index++
    )
    {
        struct json_object *entry =
            json_object_new_object();

        if (entry == NULL)
        {
            continue;
        }

        json_object_object_add(
            entry,
            "id",
            json_object_new_int64(
                records[index].id
            )
        );

        json_object_object_add(
            entry,
            "persons",
            json_object_new_int(
                records[index].persons
            )
        );

        json_object_object_add(
            entry,
            "new_persons",
            json_object_new_int(
                records[index].new_persons
            )
        );

        json_object_object_add(
            entry,
            "timestamp",
            json_object_new_string(
                records[index].timestamp
            )
        );

        json_object_array_add(
            history,
            entry
        );
    }

    add_history_report_json(
        report_json,
        &report
    );

    json_object_object_add(
        root,
        "status",
        json_object_new_string("ok")
    );

    json_object_object_add(
        root,
        "student_id",
        json_object_new_string(STUDENT_ID)
    );

    json_object_object_add(
        root,
        "storage",
        json_object_new_string(
            "sqlite+ring_buffer"
        )
    );

    json_object_object_add(
        root,
        "ring_buffer_capacity",
        json_object_new_int(
            DETECTION_HISTORY_LIMIT
        )
    );

    json_object_object_add(
        root,
        "count",
        json_object_new_int(
            (int)record_count
        )
    );

    json_object_object_add(
        root,
        "history",
        history
    );

    json_object_object_add(
        root,
        "report",
        report_json
    );

    enum MHD_Result result =
        send_json_object(
            connection,
            MHD_HTTP_OK,
            root
        );

    json_object_put(root);

    return result;
}

static enum MHD_Result send_history_report_api(
    struct MHD_Connection *connection
)
{
    DetectionHistoryReport report;

    if (
        detection_history_get_report(
            &report
        ) != 0
    )
    {
        return send_response(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "application/json; charset=utf-8",
            "{\"status\":\"error\","
            "\"message\":\"History report unavailable\"}"
        );
    }

    struct json_object *root =
        json_object_new_object();

    if (root == NULL)
    {
        return send_response(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "application/json; charset=utf-8",
            "{\"status\":\"error\"}"
        );
    }

    json_object_object_add(
        root,
        "status",
        json_object_new_string("ok")
    );

    json_object_object_add(
        root,
        "student_id",
        json_object_new_string(STUDENT_ID)
    );

    json_object_object_add(
        root,
        "storage",
        json_object_new_string("sqlite")
    );

    add_history_report_json(
        root,
        &report
    );

    enum MHD_Result result =
        send_json_object(
            connection,
            MHD_HTTP_OK,
            root
        );

    json_object_put(root);

    return result;
}

static int schedule_reboot(void)
{
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
            "--no-ask-password",
            "start",
            "--no-block",
            "smart-guard-reboot.service",
            (char *)NULL
        );

        _exit(127);
    }

    int status = 0;

    if (
        waitpid(
            child,
            &status,
            0
        ) < 0
    )
    {
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

static enum MHD_Result send_command_api(
    struct MHD_Connection *connection,
    const char *body
)
{
    if (
        body == NULL ||
        body[0] == '\0'
    )
    {
        return send_response(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "application/json; charset=utf-8",
            "{\"status\":\"error\","
            "\"message\":\"JSON body is required\"}"
        );
    }

    struct json_object *root =
        json_tokener_parse(body);

    if (
        root == NULL ||
        !json_object_is_type(
            root,
            json_type_object
        )
    )
    {
        if (root != NULL)
        {
            json_object_put(root);
        }

        return send_response(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "application/json; charset=utf-8",
            "{\"status\":\"error\","
            "\"message\":\"Invalid JSON body\"}"
        );
    }

    struct json_object *command_object = NULL;

    if (
        !json_object_object_get_ex(
            root,
            "cmd",
            &command_object
        ) ||
        !json_object_is_type(
            command_object,
            json_type_string
        )
    )
    {
        json_object_put(root);

        return send_response(
            connection,
            MHD_HTTP_BAD_REQUEST,
            "application/json; charset=utf-8",
            "{\"status\":\"error\","
            "\"message\":\"Field cmd is required\"}"
        );
    }

    const char *command =
        json_object_get_string(
            command_object
        );

    if (
        command == NULL ||
        strcmp(command, "reboot") != 0
    )
    {
        json_object_put(root);

        return send_response(
            connection,
            422,
            "application/json; charset=utf-8",
            "{\"status\":\"error\","
            "\"message\":\"Unsupported command\","
            "\"supported_commands\":[\"reboot\"]}"
        );
    }

    json_object_put(root);

    if (schedule_reboot() != 0)
    {
        return send_response(
            connection,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "application/json; charset=utf-8",
            "{\"status\":\"error\","
            "\"message\":\"Could not schedule reboot\"}"
        );
    }

    char timestamp[64];

    create_timestamp(
        timestamp,
        sizeof(timestamp)
    );

    struct json_object *response =
        json_object_new_object();

    if (response == NULL)
    {
        return send_response(
            connection,
            202,
            "application/json; charset=utf-8",
            "{\"status\":\"accepted\","
            "\"cmd\":\"reboot\"}"
        );
    }

    json_object_object_add(
        response,
        "status",
        json_object_new_string("accepted")
    );

    json_object_object_add(
        response,
        "cmd",
        json_object_new_string("reboot")
    );

    json_object_object_add(
        response,
        "message",
        json_object_new_string(
            "System reboot scheduled"
        )
    );

    json_object_object_add(
        response,
        "timestamp",
        json_object_new_string(timestamp)
    );

    enum MHD_Result result =
        send_json_object(
            connection,
            202,
            response
        );

    json_object_put(response);

    return result;
}

static ssize_t mjpeg_reader(
    void *cls,
    uint64_t position,
    char *output,
    size_t maximum
)
{
    MjpegClient *client = cls;

    (void)position;

    if (
        client->buffer_offset >=
        client->buffer_size
    )
    {
        free(client->buffer);

        client->buffer = NULL;
        client->buffer_size = 0;
        client->buffer_offset = 0;

        unsigned char *frame = NULL;
        size_t frame_size = 0;
        unsigned long long sequence = 0;

        if (
            camera_stream_wait_for_frame(
                client->last_sequence,
                &frame,
                &frame_size,
                &sequence,
                10000
            ) != 0
        )
        {
            return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        char header[256];

        int header_length = snprintf(
            header,
            sizeof(header),
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "\r\n",
            frame_size
        );

        if (
            header_length < 0 ||
            (size_t)header_length >=
                sizeof(header)
        )
        {
            free(frame);

            return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        client->buffer_size =
            (size_t)header_length +
            frame_size +
            2;

        client->buffer = malloc(
            client->buffer_size
        );

        if (client->buffer == NULL)
        {
            free(frame);

            return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        memcpy(
            client->buffer,
            header,
            (size_t)header_length
        );

        memcpy(
            client->buffer +
            (size_t)header_length,
            frame,
            frame_size
        );

        client->buffer[
            (size_t)header_length +
            frame_size
        ] = '\r';

        client->buffer[
            (size_t)header_length +
            frame_size +
            1
        ] = '\n';

        client->last_sequence = sequence;

        free(frame);
    }

    size_t remaining =
        client->buffer_size -
        client->buffer_offset;

    size_t amount =
        remaining < maximum
            ? remaining
            : maximum;

    memcpy(
        output,
        client->buffer +
        client->buffer_offset,
        amount
    );

    client->buffer_offset += amount;

    return (ssize_t)amount;
}

static void mjpeg_client_free(void *cls)
{
    MjpegClient *client = cls;

    if (client != NULL)
    {
        free(client->buffer);
        free(client);
    }
}

static enum MHD_Result send_mjpeg_stream(
    struct MHD_Connection *connection
)
{
    MjpegClient *client = calloc(
        1,
        sizeof(MjpegClient)
    );

    if (client == NULL)
    {
        return MHD_NO;
    }

    struct MHD_Response *response =
        MHD_create_response_from_callback(
            MHD_SIZE_UNKNOWN,
            65536,
            &mjpeg_reader,
            client,
            &mjpeg_client_free
        );

    if (response == NULL)
    {
        free(client);
        return MHD_NO;
    }

    MHD_add_response_header(
        response,
        "Content-Type",
        "multipart/x-mixed-replace; boundary=frame"
    );

    MHD_add_response_header(
        response,
        "Cache-Control",
        "no-store, no-cache, must-revalidate"
    );

    MHD_add_response_header(
        response,
        "Pragma",
        "no-cache"
    );

    enum MHD_Result result =
        MHD_queue_response(
            connection,
            MHD_HTTP_OK,
            response
        );

    MHD_destroy_response(response);

    return result;
}

static int append_request_body(
    RequestContext *context,
    const char *data,
    size_t data_size
)
{
    if (
        context == NULL ||
        data == NULL ||
        data_size == 0
    )
    {
        return 0;
    }

    if (
        context->body_size + data_size >
        MAX_REQUEST_BODY
    )
    {
        context->too_large = 1;
        return -1;
    }

    char *new_body = realloc(
        context->body,
        context->body_size +
        data_size +
        1
    );

    if (new_body == NULL)
    {
        return -1;
    }

    context->body = new_body;

    memcpy(
        context->body +
        context->body_size,
        data,
        data_size
    );

    context->body_size += data_size;
    context->body[context->body_size] = '\0';

    return 0;
}

static int is_command_path(const char *url)
{
    return
        strcmp(
            url,
            "/api/v1/command"
        ) == 0 ||
        strcmp(
            url,
            "/API/V1/COMMAND"
        ) == 0;
}


static enum MHD_Result send_guard_api(
    struct MHD_Connection *connection,
    const char *method
)
{
    if (
        strcmp(
            method,
            MHD_HTTP_METHOD_GET
        ) == 0
    )
    {
        char body[256];

        int enabled =
            guard_mode_is_enabled();

        snprintf(
            body,
            sizeof(body),
            "{\"status\":\"ok\","
            "\"student_id\":\"device-001\","
            "\"guard_enabled\":%s}",
            enabled ? "true" : "false"
        );

        return send_response(
            connection,
            MHD_HTTP_OK,
            "application/json; charset=utf-8",
            body
        );
    }

    if (
        strcmp(
            method,
            MHD_HTTP_METHOD_POST
        ) == 0
    )
    {
        const char *enabled_value =
            MHD_lookup_connection_value(
                connection,
                MHD_GET_ARGUMENT_KIND,
                "enabled"
            );

        if (
            enabled_value == NULL ||
            (
                strcmp(
                    enabled_value,
                    "1"
                ) != 0 &&
                strcmp(
                    enabled_value,
                    "0"
                ) != 0 &&
                strcmp(
                    enabled_value,
                    "true"
                ) != 0 &&
                strcmp(
                    enabled_value,
                    "false"
                ) != 0
            )
        )
        {
            return send_response(
                connection,
                MHD_HTTP_BAD_REQUEST,
                "application/json; charset=utf-8",
                "{\"status\":\"error\","
                "\"message\":\"Use enabled=1 or enabled=0\"}"
            );
        }

        int enabled =
            (
                strcmp(
                    enabled_value,
                    "1"
                ) == 0 ||
                strcmp(
                    enabled_value,
                    "true"
                ) == 0
            );

        if (
            guard_mode_set_enabled(
                enabled
            ) != 0
        )
        {
            return send_response(
                connection,
                MHD_HTTP_INTERNAL_SERVER_ERROR,
                "application/json; charset=utf-8",
                "{\"status\":\"error\","
                "\"message\":\"Guard state could not be saved\"}"
            );
        }

        char body[256];

        snprintf(
            body,
            sizeof(body),
            "{\"status\":\"ok\","
            "\"student_id\":\"device-001\","
            "\"guard_enabled\":%s,"
            "\"message\":\"Guard mode updated\"}",
            enabled ? "true" : "false"
        );

        return send_response(
            connection,
            MHD_HTTP_OK,
            "application/json; charset=utf-8",
            body
        );
    }

    return send_response(
        connection,
        MHD_HTTP_METHOD_NOT_ALLOWED,
        "application/json; charset=utf-8",
        "{\"status\":\"error\","
        "\"message\":\"Method not allowed\"}"
    );
}

static enum MHD_Result request_handler(
    void *cls,
    struct MHD_Connection *connection,
    const char *url,
    const char *method,
    const char *version,
    const char *upload_data,
    size_t *upload_data_size,
    void **connection_context
)
{

    if (
        strcmp(
            url,
            "/api/v1/guard"
        ) == 0 ||
        strcmp(
            url,
            "/API/V1/GUARD"
        ) == 0
    )
    {
        return send_guard_api(
            connection,
            method
        );
    }


    const char *index_file = cls;

    (void)version;

    if (*connection_context == NULL)
    {
        RequestContext *context = calloc(
            1,
            sizeof(RequestContext)
        );

        if (context == NULL)
        {
            return MHD_NO;
        }

        *connection_context = context;

        return MHD_YES;
    }

    RequestContext *context =
        *connection_context;

    if (is_command_path(url))
    {
        if (strcmp(method, "POST") != 0)
        {
            return send_response(
                connection,
                MHD_HTTP_METHOD_NOT_ALLOWED,
                "application/json; charset=utf-8",
                "{\"status\":\"error\","
                "\"message\":\"POST required\"}"
            );
        }

        if (*upload_data_size > 0)
        {
            append_request_body(
                context,
                upload_data,
                *upload_data_size
            );

            *upload_data_size = 0;

            return MHD_YES;
        }

        if (context->too_large)
        {
            return send_response(
                connection,
                413,
                "application/json; charset=utf-8",
                "{\"status\":\"error\","
                "\"message\":\"Request body too large\"}"
            );
        }

        return send_command_api(
            connection,
            context->body
        );
    }

    if (*upload_data_size > 0)
    {
        *upload_data_size = 0;
        return MHD_YES;
    }

    if (strcmp(method, "GET") != 0)
    {
        return send_response(
            connection,
            MHD_HTTP_METHOD_NOT_ALLOWED,
            "text/plain; charset=utf-8",
            "Method Not Allowed\n"
        );
    }

    if (
        strcmp(url, "/") == 0 ||
        strcmp(url, "/index.html") == 0
    )
    {
        return send_index_page(
            connection,
            index_file
        );
    }


    if (
        strcmp(url, "/api/v1/stream") == 0 ||
        strcmp(url, "/API/V1/STREAM") == 0
    )
    {
        return send_mjpeg_stream(
            connection
        );
    }

    if (
        strcmp(url, "/api/v1/persons") == 0 ||
        strcmp(url, "/API/V1/PERSONS") == 0 ||
        strcmp(url, "/api/v1/presons") == 0 ||
        strcmp(url, "/API/V1/PRESONS") == 0
    )
    {
        return send_persons_api(
            connection
        );
    }

    if (
        strcmp(url, "/api/v1/thermal") == 0 ||
        strcmp(url, "/API/V1/THERMAL") == 0
    )
    {
        return send_thermal_api(
            connection
        );
    }

    if (
        strcmp(url, "/api/v1/telemetry") == 0 ||
        strcmp(url, "/API/V1/TELEMETRY") == 0
    )
    {
        return send_telemetry_api(
            connection
        );
    }

    if (
        strcmp(
            url,
            "/api/v1/history/report"
        ) == 0 ||
        strcmp(
            url,
            "/API/V1/HISTORY/REPORT"
        ) == 0
    )
    {
        return send_history_report_api(
            connection
        );
    }

    if (
        strcmp(url, "/api/v1/history") == 0 ||
        strcmp(url, "/API/V1/HISTORY") == 0
    )
    {
        return send_history_api(
            connection
        );
    }

    if (
        strcmp(url, "/view/thermal") == 0
    )
    {
        return send_thermal_view(
            connection
        );
    }

    if (
        strcmp(url, "/view/temperature") == 0
    )
    {
        return send_temperature_view(
            connection
        );
    }

    if (
        strcmp(url, "/view/memory") == 0
    )
    {
        return send_memory_view(
            connection
        );
    }

    if (
        strcmp(url, "/view/cpu") == 0
    )
    {
        return send_cpu_view(
            connection
        );
    }

    if (
        strcmp(url, "/view/persons") == 0
    )
    {
        return send_persons_view(
            connection
        );
    }

    return send_response(
        connection,
        MHD_HTTP_NOT_FOUND,
        "text/plain; charset=utf-8",
        "404 - Not Found\n"
    );
}

static void request_completed(
    void *cls,
    struct MHD_Connection *connection,
    void **connection_context,
    enum MHD_RequestTerminationCode termination_code
)
{
    (void)cls;
    (void)connection;
    (void)termination_code;

    RequestContext *context =
        *connection_context;

    if (context != NULL)
    {
        free(context->body);
        free(context);
    }

    *connection_context = NULL;
}

static enum MHD_Result redirect_request_handler(
    void *cls,
    struct MHD_Connection *connection,
    const char *url,
    const char *method,
    const char *version,
    const char *upload_data,
    size_t *upload_data_size,
    void **connection_context
)
{
    (void)cls;
    (void)method;
    (void)version;
    (void)upload_data;
    (void)upload_data_size;
    (void)connection_context;

    char location[2048];

    int location_length = snprintf(
        location,
        sizeof(location),
        "https://127.0.0.1:%u%s",
        redirect_https_port,
        url
    );

    if (
        location_length < 0 ||
        (size_t)location_length >=
            sizeof(location)
    )
    {
        return MHD_NO;
    }

    static const char body[] =
        "Redirecting to HTTPS\n";

    struct MHD_Response *response =
        MHD_create_response_from_buffer(
            sizeof(body) - 1,
            (void *)body,
            MHD_RESPMEM_PERSISTENT
        );

    if (response == NULL)
    {
        return MHD_NO;
    }

    MHD_add_response_header(
        response,
        "Location",
        location
    );

    MHD_add_response_header(
        response,
        "Cache-Control",
        "no-store"
    );

    enum MHD_Result result =
        MHD_queue_response(
            connection,
            MHD_HTTP_MOVED_PERMANENTLY,
            response
        );

    MHD_destroy_response(response);

    return result;
}

struct MHD_Daemon *web_server_start_https(
    unsigned short port,
    const char *index_file,
    const char *certificate_file,
    const char *private_key_file
)
{
    if (
        index_file == NULL ||
        certificate_file == NULL ||
        private_key_file == NULL
    )
    {
        return NULL;
    }

    web_server_tls_cleanup();

    size_t certificate_size = 0;
    size_t private_key_size = 0;

    tls_certificate_data =
        read_entire_file(
            certificate_file,
            &certificate_size
        );

    tls_private_key_data =
        read_entire_file(
            private_key_file,
            &private_key_size
        );

    if (
        tls_certificate_data == NULL ||
        tls_private_key_data == NULL ||
        certificate_size == 0 ||
        private_key_size == 0
    )
    {
        web_server_tls_cleanup();
        return NULL;
    }

    struct MHD_Daemon *daemon =
        MHD_start_daemon(
            MHD_USE_INTERNAL_POLLING_THREAD |
            MHD_USE_THREAD_PER_CONNECTION |
            MHD_USE_SSL,
            port,
            NULL,
            NULL,
            &request_handler,
            (void *)index_file,
            MHD_OPTION_NOTIFY_COMPLETED,
            &request_completed,
            NULL,
            MHD_OPTION_HTTPS_MEM_KEY,
            tls_private_key_data,
            MHD_OPTION_HTTPS_MEM_CERT,
            tls_certificate_data,
            MHD_OPTION_END
        );

    if (daemon == NULL)
    {
        web_server_tls_cleanup();
    }

    return daemon;
}

struct MHD_Daemon *web_server_start_redirect(
    unsigned short http_port,
    unsigned short https_port
)
{
    if (
        http_port == 0 ||
        https_port == 0
    )
    {
        return NULL;
    }

    redirect_https_port = https_port;

    return MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD |
        MHD_USE_THREAD_PER_CONNECTION,
        http_port,
        NULL,
        NULL,
        &redirect_request_handler,
        NULL,
        MHD_OPTION_END
    );
}

void web_server_stop(
    struct MHD_Daemon *daemon
)
{
    if (daemon != NULL)
    {
        MHD_stop_daemon(daemon);
    }
}

void web_server_tls_cleanup(void)
{
    free(tls_certificate_data);
    free(tls_private_key_data);

    tls_certificate_data = NULL;
    tls_private_key_data = NULL;
}
