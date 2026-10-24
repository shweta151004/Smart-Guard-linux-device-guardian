#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

int image_processor_initialize(
    const char *prototxt_path,
    const char *model_path,
    const char *student_id
);

int image_processor_process_jpeg(
    const unsigned char *input_data,
    size_t input_size,
    unsigned char **output_data,
    size_t *output_size
);

int image_processor_get_person_count(void);

double image_processor_get_fps(void);

void image_processor_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
