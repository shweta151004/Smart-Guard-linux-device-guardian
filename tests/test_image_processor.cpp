#include "image_processor.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main()
{
    const char *input_path =
        "tests/camera/test_frame.jpg";

    const char *output_path =
        "tests/camera/test_detection.jpg";

    const char *prototxt_path =
        "image_processing/models/"
        "MobileNetSSD_deploy.prototxt";

    const char *model_path =
        "image_processing/models/"
        "MobileNetSSD_deploy.caffemodel";

    std::ifstream input_file(
        input_path,
        std::ios::binary
    );

    if (!input_file)
    {
        std::cerr
            << "Input image could not be opened."
            << std::endl;

        return EXIT_FAILURE;
    }

    std::vector<unsigned char> input_data(
        std::istreambuf_iterator<char>(
            input_file
        ),
        std::istreambuf_iterator<char>()
    );

    if (
        image_processor_initialize(
            prototxt_path,
            model_path,
            "device-001"
        ) != 0
    )
    {
        std::cerr
            << "Image processor initialization failed."
            << std::endl;

        return EXIT_FAILURE;
    }

    unsigned char *output_data = nullptr;
    size_t output_size = 0;

    if (
        image_processor_process_jpeg(
            input_data.data(),
            input_data.size(),
            &output_data,
            &output_size
        ) != 0
    )
    {
        std::cerr
            << "Image processing failed."
            << std::endl;

        image_processor_shutdown();

        return EXIT_FAILURE;
    }

    std::ofstream output_file(
        output_path,
        std::ios::binary
    );

    if (!output_file)
    {
        std::cerr
            << "Output image could not be created."
            << std::endl;

        std::free(output_data);
        image_processor_shutdown();

        return EXIT_FAILURE;
    }

    output_file.write(
        reinterpret_cast<const char *>(
            output_data
        ),
        static_cast<std::streamsize>(
            output_size
        )
    );

    std::free(output_data);

    std::cout
        << "Detected persons: "
        << image_processor_get_person_count()
        << std::endl;

    std::cout
        << "Output image: "
        << output_path
        << std::endl;

    image_processor_shutdown();

    return EXIT_SUCCESS;
}
