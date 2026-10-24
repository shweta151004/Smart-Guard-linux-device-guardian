#define _POSIX_C_SOURCE 200809L

#include "detection_history.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static DetectionHistoryRecord history_records[
    DETECTION_HISTORY_LIMIT
];

static size_t history_count = 0;
static size_t history_next = 0;

static sqlite3 *history_database = NULL;
static sqlite3_stmt *history_insert_statement = NULL;

static int history_initialized = 0;

static pthread_mutex_t history_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static void create_timestamp(
    char *buffer,
    size_t buffer_size,
    time_t timestamp
)
{
    struct tm local_time;

    if (
        localtime_r(
            &timestamp,
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



static int execute_sql_locked(
    const char *sql
)
{
    char *error_message = NULL;

    int result = sqlite3_exec(
        history_database,
        sql,
        NULL,
        NULL,
        &error_message
    );

    if (result != SQLITE_OK)
    {
        fprintf(
            stderr,
            "SQLite error: %s\n",
            error_message != NULL
                ? error_message
                : sqlite3_errmsg(history_database)
        );

        sqlite3_free(error_message);

        return -1;
    }

    return 0;
}

static void append_ring_record_locked(
    long long id,
    int persons,
    int new_persons,
    const char *timestamp
)
{
    DetectionHistoryRecord *record =
        &history_records[history_next];

    record->id = id;
    record->persons = persons;
    record->new_persons = new_persons;

    snprintf(
        record->timestamp,
        sizeof(record->timestamp),
        "%s",
        timestamp
    );

    history_next =
        (history_next + 1) %
        DETECTION_HISTORY_LIMIT;

    if (history_count < DETECTION_HISTORY_LIMIT)
    {
        history_count++;
    }
}

static int load_recent_records_locked(void)
{
    const char *sql =
        "SELECT id, persons, new_persons, timestamp "
        "FROM ("
        "SELECT id, persons, new_persons, timestamp "
        "FROM detection_events "
        "ORDER BY id DESC "
        "LIMIT ?"
        ") "
        "ORDER BY id ASC;";

    sqlite3_stmt *statement = NULL;

    int result = sqlite3_prepare_v2(
        history_database,
        sql,
        -1,
        &statement,
        NULL
    );

    if (result != SQLITE_OK)
    {
        fprintf(
            stderr,
            "SQLite prepare error: %s\n",
            sqlite3_errmsg(history_database)
        );

        return -1;
    }

    sqlite3_bind_int(
        statement,
        1,
        DETECTION_HISTORY_LIMIT
    );

    history_count = 0;
    history_next = 0;

    while (
        (result = sqlite3_step(statement)) ==
        SQLITE_ROW
    )
    {
        long long id =
            (long long)sqlite3_column_int64(
                statement,
                0
            );

        int persons =
            sqlite3_column_int(
                statement,
                1
            );

        int new_persons =
            sqlite3_column_int(
                statement,
                2
            );

        const unsigned char *timestamp =
            sqlite3_column_text(
                statement,
                3
            );

        append_ring_record_locked(
            id,
            persons,
            new_persons,
            timestamp != NULL
                ? (const char *)timestamp
                : "unavailable"
        );
    }

    sqlite3_finalize(statement);

    if (result != SQLITE_DONE)
    {
        fprintf(
            stderr,
            "SQLite read error: %s\n",
            sqlite3_errmsg(history_database)
        );

        return -1;
    }

    return 0;
}

static int insert_detection_locked(
    int persons,
    int new_persons,
    const char *timestamp,
    time_t created_at_unix
)
{
    sqlite3_reset(history_insert_statement);
    sqlite3_clear_bindings(history_insert_statement);

    sqlite3_bind_int(
        history_insert_statement,
        1,
        persons
    );

    sqlite3_bind_int(
        history_insert_statement,
        2,
        new_persons
    );

    sqlite3_bind_text(
        history_insert_statement,
        3,
        timestamp,
        -1,
        SQLITE_TRANSIENT
    );

    sqlite3_bind_int64(
        history_insert_statement,
        4,
        (sqlite3_int64)created_at_unix
    );

    int result =
        sqlite3_step(
            history_insert_statement
        );

    if (result != SQLITE_DONE)
    {
        fprintf(
            stderr,
            "SQLite insert error: %s\n",
            sqlite3_errmsg(history_database)
        );

        return -1;
    }

    long long id =
        (long long)sqlite3_last_insert_rowid(
            history_database
        );

    append_ring_record_locked(
        id,
        persons,
        new_persons,
        timestamp
    );

    printf(
        "Detection history stored: "
        "id=%lld persons=%d new_persons=%d "
        "timestamp=%s\n",
        id,
        persons,
        new_persons,
        timestamp
    );

    return 0;
}

int detection_history_initialize(
    const char *database_path
)
{
    if (
        database_path == NULL ||
        database_path[0] == '\0'
    )
    {
        return -1;
    }

    pthread_mutex_lock(&history_mutex);

    if (history_initialized)
    {
        pthread_mutex_unlock(&history_mutex);
        return 0;
    }

    int result = sqlite3_open_v2(
        database_path,
        &history_database,
        SQLITE_OPEN_READWRITE |
        SQLITE_OPEN_CREATE |
        SQLITE_OPEN_FULLMUTEX,
        NULL
    );

    if (result != SQLITE_OK)
    {
        fprintf(
            stderr,
            "SQLite open error: %s\n",
            history_database != NULL
                ? sqlite3_errmsg(history_database)
                : "unknown error"
        );

        if (history_database != NULL)
        {
            sqlite3_close(history_database);
            history_database = NULL;
        }

        pthread_mutex_unlock(&history_mutex);
        return -1;
    }

    sqlite3_busy_timeout(
        history_database,
        3000
    );

    if (
        execute_sql_locked(
            "PRAGMA journal_mode=WAL;"
        ) != 0 ||
        execute_sql_locked(
            "PRAGMA synchronous=NORMAL;"
        ) != 0 ||
        execute_sql_locked(
            "PRAGMA foreign_keys=ON;"
        ) != 0 ||
        execute_sql_locked(
            "CREATE TABLE IF NOT EXISTS "
            "detection_events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "persons INTEGER NOT NULL "
            "CHECK(persons > 0),"
            "new_persons INTEGER NOT NULL "
            "CHECK(new_persons > 0),"
            "timestamp TEXT NOT NULL,"
            "created_at_unix INTEGER NOT NULL"
            ");"
        ) != 0 ||
        execute_sql_locked(
            "CREATE INDEX IF NOT EXISTS "
            "idx_detection_events_created_at "
            "ON detection_events(created_at_unix);"
        ) != 0
    )
    {
        sqlite3_close(history_database);
        history_database = NULL;

        pthread_mutex_unlock(&history_mutex);
        return -1;
    }

    const char *insert_sql =
        "INSERT INTO detection_events "
        "(persons, new_persons, timestamp, "
        "created_at_unix) "
        "VALUES (?, ?, ?, ?);";

    result = sqlite3_prepare_v2(
        history_database,
        insert_sql,
        -1,
        &history_insert_statement,
        NULL
    );

    if (result != SQLITE_OK)
    {
        fprintf(
            stderr,
            "SQLite insert prepare error: %s\n",
            sqlite3_errmsg(history_database)
        );

        sqlite3_close(history_database);
        history_database = NULL;

        pthread_mutex_unlock(&history_mutex);
        return -1;
    }

    if (load_recent_records_locked() != 0)
    {
        sqlite3_finalize(
            history_insert_statement
        );

        history_insert_statement = NULL;

        sqlite3_close(history_database);
        history_database = NULL;

        pthread_mutex_unlock(&history_mutex);
        return -1;
    }

    history_initialized = 1;

    printf(
        "Detection history initialized: "
        "%s ring_capacity=%d loaded=%zu\n",
        database_path,
        DETECTION_HISTORY_LIMIT,
        history_count
    );

    pthread_mutex_unlock(&history_mutex);

    return 0;
}

void detection_history_shutdown(void)
{
    pthread_mutex_lock(&history_mutex);

    if (history_insert_statement != NULL)
    {
        sqlite3_finalize(
            history_insert_statement
        );

        history_insert_statement = NULL;
    }

    if (history_database != NULL)
    {
        sqlite3_close(history_database);
        history_database = NULL;
    }

    history_initialized = 0;
    history_count = 0;
    history_next = 0;

    pthread_mutex_unlock(&history_mutex);
}

int detection_history_record_event(
    int persons,
    int new_persons
)
{
    if (
        persons <= 0 ||
        new_persons <= 0 ||
        new_persons > persons
    )
    {
        return -1;
    }

    time_t current_time =
        time(NULL);

    if (current_time == (time_t)-1)
    {
        return -1;
    }

    char timestamp[
        DETECTION_TIMESTAMP_SIZE
    ];

    create_timestamp(
        timestamp,
        sizeof(timestamp),
        current_time
    );

    pthread_mutex_lock(&history_mutex);

    if (!history_initialized)
    {
        pthread_mutex_unlock(&history_mutex);
        return -1;
    }

    int result =
        insert_detection_locked(
            persons,
            new_persons,
            timestamp,
            current_time
        );

    pthread_mutex_unlock(&history_mutex);

    return result;
}

size_t detection_history_get(
    DetectionHistoryRecord *records,
    size_t capacity
)
{
    if (
        records == NULL ||
        capacity == 0
    )
    {
        return 0;
    }

    pthread_mutex_lock(&history_mutex);

    size_t result_count =
        history_count < capacity
            ? history_count
            : capacity;

    for (
        size_t index = 0;
        index < result_count;
        index++
    )
    {
        size_t source_index =
            (
                history_next +
                DETECTION_HISTORY_LIMIT -
                1 -
                index
            ) %
            DETECTION_HISTORY_LIMIT;

        records[index] =
            history_records[source_index];
    }

    pthread_mutex_unlock(&history_mutex);

    return result_count;
}

int detection_history_get_report(
    DetectionHistoryReport *report
)
{
    if (report == NULL)
    {
        return -1;
    }

    memset(
        report,
        0,
        sizeof(*report)
    );

    pthread_mutex_lock(&history_mutex);

    if (!history_initialized)
    {
        pthread_mutex_unlock(&history_mutex);
        return -1;
    }

    const char *sql =
        "SELECT "
        "COUNT(*),"
        "COALESCE(SUM(new_persons), 0),"
        "(SELECT timestamp "
        "FROM detection_events "
        "ORDER BY id ASC LIMIT 1),"
        "(SELECT timestamp "
        "FROM detection_events "
        "ORDER BY id DESC LIMIT 1) "
        "FROM detection_events;";

    sqlite3_stmt *statement = NULL;

    int result = sqlite3_prepare_v2(
        history_database,
        sql,
        -1,
        &statement,
        NULL
    );

    if (result != SQLITE_OK)
    {
        fprintf(
            stderr,
            "SQLite report prepare error: %s\n",
            sqlite3_errmsg(history_database)
        );

        pthread_mutex_unlock(&history_mutex);
        return -1;
    }

    result = sqlite3_step(statement);

    if (result != SQLITE_ROW)
    {
        fprintf(
            stderr,
            "SQLite report read error: %s\n",
            sqlite3_errmsg(history_database)
        );

        sqlite3_finalize(statement);
        pthread_mutex_unlock(&history_mutex);

        return -1;
    }

    report->total_detection_events =
        (long long)sqlite3_column_int64(
            statement,
            0
        );

    report->total_persons_detected =
        (long long)sqlite3_column_int64(
            statement,
            1
        );

    if (
        sqlite3_column_type(
            statement,
            2
        ) != SQLITE_NULL
    )
    {
        const unsigned char *value =
            sqlite3_column_text(
                statement,
                2
            );

        snprintf(
            report->first_detection_timestamp,
            sizeof(
                report->
                first_detection_timestamp
            ),
            "%s",
            value != NULL
                ? (const char *)value
                : "unavailable"
        );

        report->has_first_detection = 1;
    }

    if (
        sqlite3_column_type(
            statement,
            3
        ) != SQLITE_NULL
    )
    {
        const unsigned char *value =
            sqlite3_column_text(
                statement,
                3
            );

        snprintf(
            report->last_detection_timestamp,
            sizeof(
                report->
                last_detection_timestamp
            ),
            "%s",
            value != NULL
                ? (const char *)value
                : "unavailable"
        );

        report->has_last_detection = 1;
    }

    sqlite3_finalize(statement);

    pthread_mutex_unlock(&history_mutex);

    return 0;
}
