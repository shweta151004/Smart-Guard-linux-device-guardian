#include "image_processor.h"
#include "detection_history.h"
#include "email_alert.h"
#include "guard_mode.h"
#include "telemetry.h"

#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    std::mutex processor_mutex;
    cv::dnn::Net network;
    std::string configured_student_id;
    std::atomic<int> current_person_count{0};
    std::atomic<double> current_fps{0.0};
    bool initialized = false;

    std::chrono::steady_clock::time_point fps_window_start;
    unsigned long long fps_frame_count = 0;

    std::string current_timestamp()
    {
        std::time_t now = std::time(nullptr);
        struct tm local_time;
        char buffer[64];

        if (localtime_r(&now, &local_time) == nullptr)
        {
            return "Time unavailable";
        }

        if (
            std::strftime(
                buffer,
                sizeof(buffer),
                "%Y-%m-%d %H:%M:%S",
                &local_time
            ) == 0
        )
        {
            return "Time unavailable";
        }

        return std::string(buffer);
    }

    void update_fps()
    {
        fps_frame_count++;

        const auto now =
            std::chrono::steady_clock::now();

        const double elapsed =
            std::chrono::duration<double>(
                now - fps_window_start
            ).count();

        if (elapsed >= 1.0)
        {
            current_fps.store(
                static_cast<double>(
                    fps_frame_count
                ) / elapsed
            );

            fps_frame_count = 0;
            fps_window_start = now;
        }
    }

    cv::Rect create_valid_rectangle(
        int left,
        int top,
        int right,
        int bottom,
        int frame_width,
        int frame_height
    )
    {
        left = std::clamp(
            left,
            0,
            frame_width - 1
        );

        top = std::clamp(
            top,
            0,
            frame_height - 1
        );

        right = std::clamp(
            right,
            0,
            frame_width - 1
        );

        bottom = std::clamp(
            bottom,
            0,
            frame_height - 1
        );

        if (right <= left || bottom <= top)
        {
            return cv::Rect();
        }

        return cv::Rect(
            left,
            top,
            right - left,
            bottom - top
        );
    }

    void draw_information(
        cv::Mat &frame,
        int person_count
    )
    {
        const int panel_height = 92;

        cv::rectangle(
            frame,
            cv::Point(0, 0),
            cv::Point(frame.cols, panel_height),
            cv::Scalar(0, 0, 0),
            cv::FILLED
        );

        cv::putText(
            frame,
            "Student ID: " +
                configured_student_id,
            cv::Point(12, 26),
            cv::FONT_HERSHEY_SIMPLEX,
            0.65,
            cv::Scalar(255, 255, 255),
            2,
            cv::LINE_AA
        );

        cv::putText(
            frame,
            current_timestamp(),
            cv::Point(12, 54),
            cv::FONT_HERSHEY_SIMPLEX,
            0.60,
            cv::Scalar(255, 255, 255),
            2,
            cv::LINE_AA
        );

        char status[128];

        std::snprintf(
            status,
            sizeof(status),
            "Persons: %d   FPS: %.2f",
            person_count,
            current_fps.load()
        );

        cv::putText(
            frame,
            status,
            cv::Point(12, 82),
            cv::FONT_HERSHEY_SIMPLEX,
            0.60,
            cv::Scalar(255, 255, 255),
            2,
            cv::LINE_AA
        );
    }
}

extern "C"
int image_processor_initialize(
    const char *prototxt_path,
    const char *model_path,
    const char *student_id
)
{
    if (
        prototxt_path == nullptr ||
        model_path == nullptr ||
        student_id == nullptr
    )
    {
        return -1;
    }

    std::lock_guard<std::mutex> lock(
        processor_mutex
    );

    try
    {
        network =
            cv::dnn::readNetFromCaffe(
                prototxt_path,
                model_path
            );

        if (network.empty())
        {
            return -1;
        }

        network.setPreferableBackend(
            cv::dnn::DNN_BACKEND_OPENCV
        );

        network.setPreferableTarget(
            cv::dnn::DNN_TARGET_CPU
        );

        configured_student_id =
            student_id;

        current_person_count.store(0);
        current_fps.store(0.0);

        fps_frame_count = 0;

        fps_window_start =
            std::chrono::steady_clock::now();

        initialized = true;

        return 0;
    }
    catch (const cv::Exception &)
    {
        initialized = false;
        return -1;
    }
}

extern "C"
int image_processor_process_jpeg(
    const unsigned char *input_data,
    size_t input_size,
    unsigned char **output_data,
    size_t *output_size
)
{
    if (
        input_data == nullptr ||
        input_size == 0 ||
        output_data == nullptr ||
        output_size == nullptr
    )
    {
        return -1;
    }

    std::lock_guard<std::mutex> lock(
        processor_mutex
    );

    if (!initialized)
    {
        return -1;
    }

    try
    {
        std::vector<unsigned char> input_buffer(
            input_data,
            input_data + input_size
        );

        cv::Mat frame = cv::imdecode(
            input_buffer,
            cv::IMREAD_COLOR
        );

        if (frame.empty())
        {
            return -1;
        }

        cv::Mat blob =
            cv::dnn::blobFromImage(
                frame,
                0.007843,
                cv::Size(300, 300),
                cv::Scalar(
                    127.5,
                    127.5,
                    127.5
                ),
                false,
                false
            );

        network.setInput(blob);

        cv::Mat detection_output =
            network.forward();

        if (
            detection_output.dims != 4 ||
            detection_output.size[3] != 7
        )
        {
            return -1;
        }

        cv::Mat detections(
            detection_output.size[2],
            detection_output.size[3],
            CV_32F,
            detection_output.ptr<float>()
        );

        std::vector<cv::Rect> boxes;
        std::vector<float> confidences;

        for (
            int index = 0;
            index < detections.rows;
            index++
        )
        {
            const int class_id =
                static_cast<int>(
                    detections.at<float>(
                        index,
                        1
                    )
                );

            const float confidence =
                detections.at<float>(
                    index,
                    2
                );

            if (
                class_id != 15 ||
                confidence < 0.35F
            )
            {
                continue;
            }

            const int left =
                static_cast<int>(
                    detections.at<float>(
                        index,
                        3
                    ) * frame.cols
                );

            const int top =
                static_cast<int>(
                    detections.at<float>(
                        index,
                        4
                    ) * frame.rows
                );

            const int right =
                static_cast<int>(
                    detections.at<float>(
                        index,
                        5
                    ) * frame.cols
                );

            const int bottom =
                static_cast<int>(
                    detections.at<float>(
                        index,
                        6
                    ) * frame.rows
                );

            cv::Rect box =
                create_valid_rectangle(
                    left,
                    top,
                    right,
                    bottom,
                    frame.cols,
                    frame.rows
                );

            if (
                box.width < 10 ||
                box.height < 10
            )
            {
                continue;
            }

            boxes.push_back(box);
            confidences.push_back(confidence);
        }

        std::vector<int> selected_indices;

        cv::dnn::NMSBoxes(
            boxes,
            confidences,
            0.35F,
            0.30F,
            selected_indices
        );

        for (int selected : selected_indices)
        {
            const cv::Rect &box =
                boxes.at(
                    static_cast<size_t>(
                        selected
                    )
                );

            const float confidence =
                confidences.at(
                    static_cast<size_t>(
                        selected
                    )
                );

            cv::rectangle(
                frame,
                box,
                cv::Scalar(0, 255, 0),
                2,
                cv::LINE_AA
            );

            char label[64];

            std::snprintf(
                label,
                sizeof(label),
                "Person %.2f",
                confidence
            );

            const int label_y =
                std::max(
                    box.y - 8,
                    105
                );

            cv::putText(
                frame,
                label,
                cv::Point(
                    box.x,
                    label_y
                ),
                cv::FONT_HERSHEY_SIMPLEX,
                0.55,
                cv::Scalar(0, 255, 0),
                2,
                cv::LINE_AA
            );
        }

        const int person_count =
            static_cast<int>(
                selected_indices.size()
            );

        current_person_count.store(
            person_count
        );

        update_fps();

        draw_information(
            frame,
            person_count
        );

        std::vector<unsigned char>
            encoded_frame;

        const std::vector<int>
            encoding_parameters =
            {
                cv::IMWRITE_JPEG_QUALITY,
                85
            };

        if (
            !cv::imencode(
                ".jpg",
                frame,
                encoded_frame,
                encoding_parameters
            )
        )
        {
            return -1;
        }

        static int stable_person_count = 0;
        static int candidate_person_count = -1;
        static int previous_guard_enabled = 0;

        static std::chrono::steady_clock::time_point
            candidate_since;

        const int guard_enabled =
            guard_mode_is_enabled();

        const auto detection_time =
            std::chrono::steady_clock::now();

        int stable_increase = 0;

        if (person_count == stable_person_count)
        {
            candidate_person_count = -1;
        }
        else if (
            candidate_person_count != person_count
        )
        {
            candidate_person_count =
                person_count;

            candidate_since =
                detection_time;
        }
        else
        {
            const long long stable_milliseconds =
                std::chrono::duration_cast<
                    std::chrono::milliseconds
                >(
                    detection_time -
                    candidate_since
                ).count();

            const long long required_stability_milliseconds =
                candidate_person_count == 0
                    ? 5000
                    : 1000;

            if (
                stable_milliseconds >=
                required_stability_milliseconds
            )
            {
                const int previous_stable_count =
                    stable_person_count;

                stable_person_count =
                    candidate_person_count;

                candidate_person_count = -1;

                if (
                    stable_person_count > 0 &&
                    stable_person_count >
                        previous_stable_count
                )
                {
                    const int new_persons =
                        stable_person_count -
                        previous_stable_count;

                    (void)detection_history_record_event(
                        stable_person_count,
                        new_persons
                    );

                    stable_increase = 1;
                }
            }
        }

        const int guard_activated_with_person =
            guard_enabled &&
            previous_guard_enabled == 0 &&
            stable_person_count > 0;

        if (
            guard_enabled &&
            (
                stable_increase ||
                guard_activated_with_person
            )
        )
        {
            double cpu_temperature_c = -1.0;

            telemetry_read_cpu_temperature(
                &cpu_temperature_c
            );

            const std::string alert_timestamp =
                current_timestamp();

            guard_mode_handle_detection(
                stable_person_count,
                alert_timestamp.c_str(),
                cpu_temperature_c,
                encoded_frame.data(),
                encoded_frame.size()
            );
        }

        previous_guard_enabled =
            guard_enabled;

        unsigned char *result =
            static_cast<unsigned char *>(
                std::malloc(
                    encoded_frame.size()
                )
            );

        if (result == nullptr)
        {
            return -1;
        }

        std::memcpy(
            result,
            encoded_frame.data(),
            encoded_frame.size()
        );

        *output_data = result;
        *output_size = encoded_frame.size();

        return 0;
    }
    catch (const cv::Exception &)
    {
        return -1;
    }
    catch (...)
    {
        return -1;
    }
}

extern "C"
int image_processor_get_person_count(void)
{
    return current_person_count.load();
}

extern "C"
double image_processor_get_fps(void)
{
    return current_fps.load();
}

extern "C"
void image_processor_shutdown(void)
{
    std::lock_guard<std::mutex> lock(
        processor_mutex
    );

    network = cv::dnn::Net();
    configured_student_id.clear();

    current_person_count.store(0);
    current_fps.store(0.0);

    fps_frame_count = 0;
    initialized = false;
}
