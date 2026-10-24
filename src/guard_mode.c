#define _POSIX_C_SOURCE 200809L

#include "guard_mode.h"
#include "email_alert.h"

#include <json-c/json.h>
#include <mosquitto.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GUARD_PATH_SIZE 512
#define MQTT_VALUE_SIZE 512

typedef struct
{
    int connected;
    int published;
    int failed;
} MqttPublishState;

static atomic_int guard_enabled = 0;

static char guard_state_file[
    GUARD_PATH_SIZE
];

static int mqtt_library_initialized = 0;

static void mqtt_connect_callback(
    struct mosquitto *client,
    void *userdata,
    int result
)
{
    (void)client;

    MqttPublishState *state =
        userdata;

    if (state == NULL)
    {
        return;
    }

    if (result == 0)
    {
        state->connected = 1;
    }
    else
    {
        state->failed = 1;
    }
}

static void mqtt_publish_callback(
    struct mosquitto *client,
    void *userdata,
    int message_id
)
{
    (void)client;
    (void)message_id;

    MqttPublishState *state =
        userdata;

    if (state != NULL)
    {
        state->published = 1;
    }
}

static int copy_environment(
    const char *name,
    char *destination,
    size_t destination_size
)
{
    const char *value =
        getenv(name);

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

static int save_guard_state(
    int enabled
)
{
    if (guard_state_file[0] == '\0')
    {
        return -1;
    }

    char temporary_path[
        GUARD_PATH_SIZE + 16
    ];

    int length = snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp",
        guard_state_file
    );

    if (
        length < 0 ||
        (size_t)length >=
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

    if (
        fprintf(
            file,
            "%d\n",
            enabled ? 1 : 0
        ) < 0
    )
    {
        fclose(file);
        remove(temporary_path);
        return -1;
    }

    if (fclose(file) != 0)
    {
        remove(temporary_path);
        return -1;
    }

    if (
        rename(
            temporary_path,
            guard_state_file
        ) != 0
    )
    {
        remove(temporary_path);
        return -1;
    }

    return 0;
}

static int publish_alarm_mqtt(
    int persons,
    const char *timestamp,
    double cpu_temperature_c
)
{
    if (!mqtt_library_initialized)
    {
        return -1;
    }

    char host[MQTT_VALUE_SIZE];
    char username[MQTT_VALUE_SIZE];
    char password[MQTT_VALUE_SIZE];
    char topic[MQTT_VALUE_SIZE];

    if (
        copy_environment(
            "MQTT_HOST",
            host,
            sizeof(host)
        ) != 0 ||
        copy_environment(
            "MQTT_USERNAME",
            username,
            sizeof(username)
        ) != 0 ||
        copy_environment(
            "MQTT_PASSWORD",
            password,
            sizeof(password)
        ) != 0 ||
        copy_environment(
            "MQTT_ALARM_TOPIC",
            topic,
            sizeof(topic)
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Guard MQTT configuration is incomplete.\n"
        );

        return -1;
    }

    int port = 1883;

    const char *port_value =
        getenv("MQTT_PORT");

    if (
        port_value != NULL &&
        port_value[0] != '\0'
    )
    {
        int parsed_port =
            atoi(port_value);

        if (
            parsed_port > 0 &&
            parsed_port <= 65535
        )
        {
            port = parsed_port;
        }
    }

    char client_id[128];

    snprintf(
        client_id,
        sizeof(client_id),
        "smart-guard-alarm-%ld-%ld",
        (long)getpid(),
        (long)time(NULL)
    );

    MqttPublishState state =
    {
        .connected = 0,
        .published = 0,
        .failed = 0
    };

    struct mosquitto *client =
        mosquitto_new(
            client_id,
            true,
            &state
        );

    if (client == NULL)
    {
        return -1;
    }

    if (
        mosquitto_username_pw_set(
            client,
            username,
            password
        ) != MOSQ_ERR_SUCCESS
    )
    {
        mosquitto_destroy(client);
        return -1;
    }

    mosquitto_connect_callback_set(
        client,
        mqtt_connect_callback
    );

    mosquitto_publish_callback_set(
        client,
        mqtt_publish_callback
    );

    int result = mosquitto_connect(
        client,
        host,
        port,
        10
    );

    if (result != MOSQ_ERR_SUCCESS)
    {
        fprintf(
            stderr,
            "Guard MQTT connection failed: %s\n",
            mosquitto_strerror(result)
        );

        mosquitto_destroy(client);
        return -1;
    }

    for (
        int attempt = 0;
        attempt < 50 &&
        !state.connected &&
        !state.failed;
        attempt++
    )
    {
        result = mosquitto_loop(
            client,
            100,
            1
        );

        if (result != MOSQ_ERR_SUCCESS)
        {
            state.failed = 1;
        }
    }

    if (
        !state.connected ||
        state.failed
    )
    {
        mosquitto_disconnect(client);
        mosquitto_destroy(client);
        return -1;
    }

    struct json_object *message =
        json_object_new_object();

    if (message == NULL)
    {
        mosquitto_disconnect(client);
        mosquitto_destroy(client);
        return -1;
    }

    json_object_object_add(
        message,
        "event",
        json_object_new_string(
            "guard_human_detected"
        )
    );

    json_object_object_add(
        message,
        "student_id",
        json_object_new_string(
            "device-001"
        )
    );

    json_object_object_add(
        message,
        "guard_enabled",
        json_object_new_boolean(1)
    );

    json_object_object_add(
        message,
        "persons",
        json_object_new_int(persons)
    );

    json_object_object_add(
        message,
        "cpu_temperature_c",
        json_object_new_double(
            cpu_temperature_c
        )
    );

    json_object_object_add(
        message,
        "timestamp",
        json_object_new_string(timestamp)
    );

    json_object_object_add(
        message,
        "qos",
        json_object_new_int(1)
    );

    const char *payload =
        json_object_to_json_string_ext(
            message,
            JSON_C_TO_STRING_PLAIN
        );

    int message_id = 0;

    result = mosquitto_publish(
        client,
        &message_id,
        topic,
        (int)strlen(payload),
        payload,
        1,
        false
    );

    if (result == MOSQ_ERR_SUCCESS)
    {
        for (
            int attempt = 0;
            attempt < 50 &&
            !state.published &&
            !state.failed;
            attempt++
        )
        {
            result = mosquitto_loop(
                client,
                100,
                1
            );

            if (result != MOSQ_ERR_SUCCESS)
            {
                state.failed = 1;
            }
        }
    }

    if (
        result == MOSQ_ERR_SUCCESS &&
        state.published
    )
    {
        printf(
            "Guard MQTT alarm published: topic=%s persons=%d qos=1\n",
            topic,
            persons
        );
    }
    else
    {
        fprintf(
            stderr,
            "Guard MQTT alarm failed.\n"
        );
    }

    json_object_put(message);

    mosquitto_disconnect(client);

    for (
        int attempt = 0;
        attempt < 5;
        attempt++
    )
    {
        mosquitto_loop(
            client,
            50,
            1
        );
    }

    mosquitto_destroy(client);

    return (
        result == MOSQ_ERR_SUCCESS &&
        state.published
    ) ? 0 : -1;
}

int guard_mode_initialize(
    const char *state_file
)
{
    if (
        state_file == NULL ||
        state_file[0] == '\0'
    )
    {
        return -1;
    }

    int length = snprintf(
        guard_state_file,
        sizeof(guard_state_file),
        "%s",
        state_file
    );

    if (
        length < 0 ||
        (size_t)length >=
            sizeof(guard_state_file)
    )
    {
        return -1;
    }

    int enabled = 0;

    FILE *file = fopen(
        guard_state_file,
        "r"
    );

    if (file != NULL)
    {
        int stored_value = 0;

        if (
            fscanf(
                file,
                "%d",
                &stored_value
            ) == 1
        )
        {
            enabled =
                stored_value != 0;
        }

        fclose(file);
    }
    else
    {
        if (save_guard_state(0) != 0)
        {
            return -1;
        }
    }

    atomic_store(
        &guard_enabled,
        enabled
    );

    if (
        mosquitto_lib_init() !=
        MOSQ_ERR_SUCCESS
    )
    {
        fprintf(
            stderr,
            "Guard MQTT library initialization failed.\n"
        );

        return -1;
    }

    mqtt_library_initialized = 1;

    printf(
        "Guard mode initialized: %s\n",
        enabled ? "enabled" : "disabled"
    );

    return 0;
}

int guard_mode_is_enabled(void)
{
    return atomic_load(
        &guard_enabled
    ) != 0;
}

int guard_mode_set_enabled(
    int enabled
)
{
    int normalized =
        enabled ? 1 : 0;

    if (
        save_guard_state(
            normalized
        ) != 0
    )
    {
        return -1;
    }

    atomic_store(
        &guard_enabled,
        normalized
    );

    printf(
        "Guard mode changed: %s\n",
        normalized ?
            "enabled" :
            "disabled"
    );

    return 0;
}

int guard_mode_handle_detection(
    int persons,
    const char *timestamp,
    double cpu_temperature_c,
    const unsigned char *jpeg_data,
    size_t jpeg_size
)
{
    if (!guard_mode_is_enabled())
    {
        return 1;
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

    int email_result =
        email_alert_queue_immediate(
            persons,
            timestamp,
            cpu_temperature_c,
            jpeg_data,
            jpeg_size
        );

    int mqtt_result =
        publish_alarm_mqtt(
            persons,
            timestamp,
            cpu_temperature_c
        );

    printf(
        "Guard alert processed: persons=%d email=%s mqtt=%s timestamp=%s\n",
        persons,
        email_result == 0 ?
            "queued" :
            "failed",
        mqtt_result == 0 ?
            "published" :
            "failed",
        timestamp
    );

    return (
        email_result == 0 &&
        mqtt_result == 0
    ) ? 0 : -1;
}

void guard_mode_shutdown(void)
{
    if (mqtt_library_initialized)
    {
        mosquitto_lib_cleanup();
        mqtt_library_initialized = 0;
    }
}
