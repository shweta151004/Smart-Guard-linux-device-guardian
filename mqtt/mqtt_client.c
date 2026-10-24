#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <json-c/json.h>
#include <mosquitto.h>

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STUDENT_ID "device-001"

typedef struct
{
    char *data;
    size_t size;
} HttpBuffer;

typedef struct
{
    const char *host;
    int port;
    const char *username;
    const char *password;
    const char *telemetry_topic;
    const char *persons_topic;
    const char *status_topic;
    const char *telemetry_url;
    const char *persons_url;
    const char *ca_file;
    int interval_seconds;
} MqttConfig;

static volatile sig_atomic_t keep_running = 1;
static atomic_bool mqtt_connected = false;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static const char *required_environment(
    const char *name
)
{
    const char *value = getenv(name);

    if (
        value == NULL ||
        value[0] == '\0'
    )
    {
        fprintf(
            stderr,
            "Missing environment variable: %s\n",
            name
        );

        return NULL;
    }

    return value;
}

static int environment_integer(
    const char *name,
    int default_value,
    int minimum,
    int maximum
)
{
    const char *text = getenv(name);

    if (
        text == NULL ||
        text[0] == '\0'
    )
    {
        return default_value;
    }

    char *end = NULL;
    long value = strtol(
        text,
        &end,
        10
    );

    if (
        end == text ||
        *end != '\0' ||
        value < minimum ||
        value > maximum
    )
    {
        return default_value;
    }

    return (int)value;
}

static void current_timestamp(
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

static size_t http_write_callback(
    void *contents,
    size_t size,
    size_t count,
    void *user_data
)
{
    size_t total_size = size * count;
    HttpBuffer *buffer = user_data;

    char *new_data = realloc(
        buffer->data,
        buffer->size + total_size + 1
    );

    if (new_data == NULL)
    {
        return 0;
    }

    buffer->data = new_data;

    memcpy(
        buffer->data + buffer->size,
        contents,
        total_size
    );

    buffer->size += total_size;
    buffer->data[buffer->size] = '\0';

    return total_size;
}

static int fetch_https(
    const char *url,
    const char *ca_file,
    HttpBuffer *buffer
)
{
    if (
        url == NULL ||
        ca_file == NULL ||
        buffer == NULL
    )
    {
        return -1;
    }

    buffer->data = NULL;
    buffer->size = 0;

    CURL *curl = curl_easy_init();

    if (curl == NULL)
    {
        return -1;
    }

    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        url
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        http_write_callback
    );

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        buffer
    );

    curl_easy_setopt(
        curl,
        CURLOPT_CONNECTTIMEOUT,
        4L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        6L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYPEER,
        1L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_SSL_VERIFYHOST,
        2L
    );

    curl_easy_setopt(
        curl,
        CURLOPT_CAINFO,
        ca_file
    );

    curl_easy_setopt(
        curl,
        CURLOPT_USERAGENT,
        "smart-guard-mqtt/1.0"
    );

    CURLcode result = curl_easy_perform(curl);

    long status_code = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &status_code
    );

    curl_easy_cleanup(curl);

    if (
        result != CURLE_OK ||
        status_code != 200 ||
        buffer->data == NULL
    )
    {
        free(buffer->data);
        buffer->data = NULL;
        buffer->size = 0;

        return -1;
    }

    return 0;
}

static int parse_temperature(
    const char *json_text,
    double *temperature
)
{
    if (
        json_text == NULL ||
        temperature == NULL
    )
    {
        return -1;
    }

    struct json_object *root =
        json_tokener_parse(json_text);

    if (root == NULL)
    {
        return -1;
    }

    struct json_object *temperature_object = NULL;

    if (
        !json_object_object_get_ex(
            root,
            "cpu_temperature_c",
            &temperature_object
        )
    )
    {
        json_object_put(root);
        return -1;
    }

    *temperature =
        json_object_get_double(
            temperature_object
        );

    json_object_put(root);

    return 0;
}

static int parse_person_count(
    const char *html,
    int *person_count
)
{
    if (
        html == NULL ||
        person_count == NULL
    )
    {
        return -1;
    }

    const char *marker =
        "<div class=\"value\">";

    const char *position = strstr(
        html,
        marker
    );

    if (position == NULL)
    {
        return -1;
    }

    position += strlen(marker);

    char *end = NULL;

    long value = strtol(
        position,
        &end,
        10
    );

    if (
        end == position ||
        value < 0 ||
        value > 1000
    )
    {
        return -1;
    }

    *person_count = (int)value;

    return 0;
}

static int publish_payload(
    struct mosquitto *mosquitto_client,
    const char *topic,
    const char *payload,
    bool retain
)
{
    int message_id = 0;

    int result = mosquitto_publish(
        mosquitto_client,
        &message_id,
        topic,
        (int)strlen(payload),
        payload,
        1,
        retain
    );

    if (result != MOSQ_ERR_SUCCESS)
    {
        fprintf(
            stderr,
            "MQTT publish failed on %s: %s\n",
            topic,
            mosquitto_strerror(result)
        );

        return -1;
    }

    printf(
        "Published QoS 1 message: topic=%s mid=%d\n",
        topic,
        message_id
    );

    return 0;
}

static void publish_online_status(
    struct mosquitto *mosquitto_client,
    const MqttConfig *config
)
{
    char timestamp[64];

    current_timestamp(
        timestamp,
        sizeof(timestamp)
    );

    struct json_object *root =
        json_object_new_object();

    if (root == NULL)
    {
        return;
    }

    json_object_object_add(
        root,
        "student_id",
        json_object_new_string(STUDENT_ID)
    );

    json_object_object_add(
        root,
        "status",
        json_object_new_string("online")
    );

    json_object_object_add(
        root,
        "timestamp",
        json_object_new_string(timestamp)
    );

    const char *payload =
        json_object_to_json_string_ext(
            root,
            JSON_C_TO_STRING_PLAIN
        );

    publish_payload(
        mosquitto_client,
        config->status_topic,
        payload,
        true
    );

    json_object_put(root);
}

static void on_connect(
    struct mosquitto *mosquitto_client,
    void *user_data,
    int result
)
{
    MqttConfig *config = user_data;

    if (result == 0)
    {
        atomic_store(
            &mqtt_connected,
            true
        );

        printf(
            "Connected to MQTT broker successfully.\n"
        );

        publish_online_status(
            mosquitto_client,
            config
        );
    }
    else
    {
        atomic_store(
            &mqtt_connected,
            false
        );

        fprintf(
            stderr,
            "MQTT connection failed: %s\n",
            mosquitto_connack_string(result)
        );
    }
}

static void on_disconnect(
    struct mosquitto *mosquitto_client,
    void *user_data,
    int result
)
{
    (void)mosquitto_client;
    (void)user_data;

    atomic_store(
        &mqtt_connected,
        false
    );

    fprintf(
        stderr,
        "MQTT disconnected: %s\n",
        mosquitto_strerror(result)
    );
}

static int publish_measurements(
    struct mosquitto *mosquitto_client,
    const MqttConfig *config
)
{
    HttpBuffer telemetry_buffer;
    HttpBuffer persons_buffer;

    if (
        fetch_https(
            config->telemetry_url,
            config->ca_file,
            &telemetry_buffer
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Could not read telemetry API.\n"
        );

        return -1;
    }

    if (
        fetch_https(
            config->persons_url,
            config->ca_file,
            &persons_buffer
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Could not read persons endpoint.\n"
        );

        free(telemetry_buffer.data);

        return -1;
    }

    double temperature = 0.0;
    int person_count = 0;

    int temperature_result =
        parse_temperature(
            telemetry_buffer.data,
            &temperature
        );

    int persons_result =
        parse_person_count(
            persons_buffer.data,
            &person_count
        );

    free(telemetry_buffer.data);
    free(persons_buffer.data);

    if (
        temperature_result != 0 ||
        persons_result != 0
    )
    {
        fprintf(
            stderr,
            "Could not parse API responses.\n"
        );

        return -1;
    }

    char timestamp[64];

    current_timestamp(
        timestamp,
        sizeof(timestamp)
    );

    struct json_object *telemetry =
        json_object_new_object();

    struct json_object *persons =
        json_object_new_object();

    if (
        telemetry == NULL ||
        persons == NULL
    )
    {
        if (telemetry != NULL)
        {
            json_object_put(telemetry);
        }

        if (persons != NULL)
        {
            json_object_put(persons);
        }

        return -1;
    }

    json_object_object_add(
        telemetry,
        "student_id",
        json_object_new_string(STUDENT_ID)
    );

    json_object_object_add(
        telemetry,
        "persons",
        json_object_new_int(person_count)
    );

    json_object_object_add(
        telemetry,
        "cpu_temperature_c",
        json_object_new_double(temperature)
    );

    json_object_object_add(
        telemetry,
        "timestamp",
        json_object_new_string(timestamp)
    );

    json_object_object_add(
        telemetry,
        "qos",
        json_object_new_int(1)
    );

    json_object_object_add(
        persons,
        "student_id",
        json_object_new_string(STUDENT_ID)
    );

    json_object_object_add(
        persons,
        "persons",
        json_object_new_int(person_count)
    );

    json_object_object_add(
        persons,
        "cpu_temperature_c",
        json_object_new_double(temperature)
    );

    json_object_object_add(
        persons,
        "timestamp",
        json_object_new_string(timestamp)
    );

    json_object_object_add(
        persons,
        "qos",
        json_object_new_int(1)
    );

    const char *telemetry_payload =
        json_object_to_json_string_ext(
            telemetry,
            JSON_C_TO_STRING_PLAIN
        );

    const char *persons_payload =
        json_object_to_json_string_ext(
            persons,
            JSON_C_TO_STRING_PLAIN
        );

    int telemetry_publish =
        publish_payload(
            mosquitto_client,
            config->telemetry_topic,
            telemetry_payload,
            false
        );

    int persons_publish =
        publish_payload(
            mosquitto_client,
            config->persons_topic,
            persons_payload,
            false
        );

    json_object_put(telemetry);
    json_object_put(persons);

    if (
        telemetry_publish != 0 ||
        persons_publish != 0
    )
    {
        return -1;
    }

    return 0;
}

static void sleep_interruptible(
    int seconds
)
{
    struct timespec request;

    request.tv_sec = seconds;
    request.tv_nsec = 0;

    while (
        keep_running &&
        nanosleep(
            &request,
            &request
        ) != 0 &&
        errno == EINTR
    )
    {
    }
}

int main(void)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    setvbuf(
        stdout,
        NULL,
        _IOLBF,
        0
    );

    MqttConfig config;

    config.host =
        required_environment("MQTT_HOST");

    config.username =
        required_environment("MQTT_USERNAME");

    config.password =
        required_environment("MQTT_PASSWORD");

    config.telemetry_topic =
        required_environment(
            "MQTT_TELEMETRY_TOPIC"
        );

    config.persons_topic =
        required_environment(
            "MQTT_PERSONS_TOPIC"
        );

    config.status_topic =
        required_environment(
            "MQTT_STATUS_TOPIC"
        );

    config.telemetry_url =
        required_environment(
            "MQTT_API_TELEMETRY_URL"
        );

    config.persons_url =
        required_environment(
            "MQTT_API_PERSONS_URL"
        );

    config.ca_file =
        required_environment(
            "MQTT_CA_FILE"
        );

    if (
        config.host == NULL ||
        config.username == NULL ||
        config.password == NULL ||
        config.telemetry_topic == NULL ||
        config.persons_topic == NULL ||
        config.status_topic == NULL ||
        config.telemetry_url == NULL ||
        config.persons_url == NULL ||
        config.ca_file == NULL
    )
    {
        return EXIT_FAILURE;
    }

    config.port =
        environment_integer(
            "MQTT_PORT",
            1883,
            1,
            65535
        );

    config.interval_seconds =
        environment_integer(
            "MQTT_INTERVAL_SECONDS",
            2,
            1,
            3600
        );

    if (
        curl_global_init(
            CURL_GLOBAL_DEFAULT
        ) != CURLE_OK
    )
    {
        return EXIT_FAILURE;
    }

    if (
        mosquitto_lib_init() !=
        MOSQ_ERR_SUCCESS
    )
    {
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    char client_id[128];

    snprintf(
        client_id,
        sizeof(client_id),
        "smart-guard-%s-%ld",
        STUDENT_ID,
        (long)getpid()
    );

    struct mosquitto *mosquitto_client =
        mosquitto_new(
            client_id,
            true,
            &config
        );

    if (mosquitto_client == NULL)
    {
        mosquitto_lib_cleanup();
        curl_global_cleanup();

        return EXIT_FAILURE;
    }

    if (
        mosquitto_username_pw_set(
            mosquitto_client,
            config.username,
            config.password
        ) != MOSQ_ERR_SUCCESS
    )
    {
        mosquitto_destroy(
            mosquitto_client
        );

        mosquitto_lib_cleanup();
        curl_global_cleanup();

        return EXIT_FAILURE;
    }

    char lwt_timestamp[64];

    current_timestamp(
        lwt_timestamp,
        sizeof(lwt_timestamp)
    );

    struct json_object *lwt_root =
        json_object_new_object();

    if (lwt_root == NULL)
    {
        mosquitto_destroy(
            mosquitto_client
        );

        mosquitto_lib_cleanup();
        curl_global_cleanup();

        return EXIT_FAILURE;
    }

    json_object_object_add(
        lwt_root,
        "student_id",
        json_object_new_string(STUDENT_ID)
    );

    json_object_object_add(
        lwt_root,
        "status",
        json_object_new_string("offline")
    );

    json_object_object_add(
        lwt_root,
        "timestamp",
        json_object_new_string(lwt_timestamp)
    );

    const char *lwt_payload =
        json_object_to_json_string_ext(
            lwt_root,
            JSON_C_TO_STRING_PLAIN
        );

    int will_result =
        mosquitto_will_set(
            mosquitto_client,
            config.status_topic,
            (int)strlen(lwt_payload),
            lwt_payload,
            1,
            true
        );

    json_object_put(lwt_root);

    if (will_result != MOSQ_ERR_SUCCESS)
    {
        mosquitto_destroy(
            mosquitto_client
        );

        mosquitto_lib_cleanup();
        curl_global_cleanup();

        return EXIT_FAILURE;
    }

    mosquitto_connect_callback_set(
        mosquitto_client,
        on_connect
    );

    mosquitto_disconnect_callback_set(
        mosquitto_client,
        on_disconnect
    );

    mosquitto_reconnect_delay_set(
        mosquitto_client,
        1,
        30,
        true
    );

    int connect_result =
        mosquitto_connect_async(
            mosquitto_client,
            config.host,
            config.port,
            30
        );

    if (connect_result != MOSQ_ERR_SUCCESS)
    {
        fprintf(
            stderr,
            "MQTT startup failed: %s\n",
            mosquitto_strerror(
                connect_result
            )
        );

        mosquitto_destroy(
            mosquitto_client
        );

        mosquitto_lib_cleanup();
        curl_global_cleanup();

        return EXIT_FAILURE;
    }

    if (
        mosquitto_loop_start(
            mosquitto_client
        ) != MOSQ_ERR_SUCCESS
    )
    {
        mosquitto_destroy(
            mosquitto_client
        );

        mosquitto_lib_cleanup();
        curl_global_cleanup();

        return EXIT_FAILURE;
    }

    printf(
        "Smart Guard MQTT client started.\n"
    );

    printf(
        "Telemetry topic: %s\n",
        config.telemetry_topic
    );

    printf(
        "Persons topic: %s\n",
        config.persons_topic
    );

    printf(
        "Status and LWT topic: %s\n",
        config.status_topic
    );

    while (keep_running)
    {
        if (
            atomic_load(
                &mqtt_connected
            )
        )
        {
            publish_measurements(
                mosquitto_client,
                &config
            );
        }

        sleep_interruptible(
            config.interval_seconds
        );
    }

    mosquitto_disconnect(
        mosquitto_client
    );

    mosquitto_loop_stop(
        mosquitto_client,
        true
    );

    mosquitto_destroy(
        mosquitto_client
    );

    mosquitto_lib_cleanup();
    curl_global_cleanup();

    return EXIT_SUCCESS;
}
