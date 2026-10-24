#ifndef DETECTION_HISTORY_H
#define DETECTION_HISTORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DETECTION_HISTORY_LIMIT 20
#define DETECTION_TIMESTAMP_SIZE 40

typedef struct
{
    long long id;
    int persons;
    int new_persons;
    char timestamp[DETECTION_TIMESTAMP_SIZE];
} DetectionHistoryRecord;

typedef struct
{
    long long total_detection_events;
    long long total_persons_detected;
    int has_first_detection;
    int has_last_detection;
    char first_detection_timestamp[
        DETECTION_TIMESTAMP_SIZE
    ];
    char last_detection_timestamp[
        DETECTION_TIMESTAMP_SIZE
    ];
} DetectionHistoryReport;

int detection_history_initialize(
    const char *database_path
);

void detection_history_shutdown(void);

int detection_history_record_event(
    int persons,
    int new_persons
);

size_t detection_history_get(
    DetectionHistoryRecord *records,
    size_t capacity
);

int detection_history_get_report(
    DetectionHistoryReport *report
);

#ifdef __cplusplus
}
#endif

#endif
