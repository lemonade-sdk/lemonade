#ifndef __error_measure_cpu__
#define __error_measure_cpu__

#include <type_traits> // Required for std::is_same_v
#include <stdfloat>
#include <algorithm> // For std::min
template<typename Ta,typename Tb >
float get_relativeL2(Ta* y, Tb* y_ref, int batch_size, int seq_len, int hidden_size, int seq_len_padded, int hidden_size_padded){
    // static_assert(std::is_same_v<Ta, float> || std::is_same_v<Ta, std::bfloat16_t>,
    //                 "Error: T must be either float or std::bfloat16_t.");
    float rmse = 0;
    float ref_sum = 0;
    for (int b = 0; b < batch_size; ++b) {
        for (int s = 0; s < seq_len; ++s) {
            for (int h = 0; h < hidden_size; ++h) {
                long long y_idx = (long long)b * seq_len_padded * hidden_size_padded + (long long)s * hidden_size_padded + h;
                long long y_ref_idx = (long long)b * seq_len * hidden_size + (long long)s * hidden_size + h;
                ref_sum += (float)y_ref[y_ref_idx] * (float)y_ref[y_ref_idx];
                rmse += ((float)y[y_idx] - (float)y_ref[y_ref_idx]) * ((float)y[y_idx] - (float)y_ref[y_ref_idx]);
            }
        }
    }
    int y_size = batch_size * seq_len * hidden_size;
    return sqrt(rmse / y_size) / sqrt(ref_sum / y_size);
}

// y is [batch_size, seq_len_padded, hidden_size_padded]
// y_ref is [batch_size, seq_len, hidden_size]
template<typename Ta,typename Tb >
float get_relativeL1(Ta* y, Tb *y_ref, int batch_size, int seq_len, int hidden_size, int seq_len_padded, int hidden_size_padded)
{
    float l1 = 0;
    float ref_sum = 0;
    for (int b = 0; b < batch_size; ++b) {
        for (int s = 0; s < seq_len; ++s) {
            for (int h = 0; h < hidden_size; ++h) {
                long long y_idx = (long long)b * seq_len_padded * hidden_size_padded + (long long)s * hidden_size_padded + h;
                long long y_ref_idx = (long long)b * seq_len * hidden_size + (long long)s * hidden_size + h;
                ref_sum += abs((float)y_ref[y_ref_idx]);
                l1 += abs((float)y[y_idx] - (float)y_ref[y_ref_idx]);
            }
        }
    }
    return l1 / ref_sum;
}

template<typename Ta, typename Tb>
float get_rmse(Ta *y, Tb *y_ref,
               int batch_size, int seq_len, int hidden_size,
               int seq_len_padded, int hidden_size_padded)
{
    double rmse = 0.0;
    for (int b = 0; b < batch_size; ++b) {
        for (int s = 0; s < seq_len; ++s) {
            for (int h = 0; h < hidden_size; ++h) {
                long long y_idx = (long long)b * seq_len_padded * hidden_size_padded
                                + (long long)s * hidden_size_padded + h;
                long long y_ref_idx = (long long)b * seq_len * hidden_size
                                    + (long long)s * hidden_size + h;
                float dy = static_cast<float>(y[y_idx]) -
                           static_cast<float>(y_ref[y_ref_idx]);
                rmse += dy * dy;
            }
        }
    }
    double y_size = (double)batch_size * seq_len * hidden_size;
    return static_cast<float>(sqrt(rmse / y_size));
}

// template<typename Ta,typename Tb >
// float get_rmse(Ta *y, Tb *y_ref, int batch_size, int seq_len, int hidden_size, int seq_len_padded, int hidden_size_padded)
// {
//     float rmse = 0;
//     for (int b = 0; b < batch_size; ++b) {
//         for (int s = 0; s < seq_len; ++s) {
//             for (int h = 0; h < hidden_size; ++h) {
//                 long long y_idx = (long long)b * seq_len_padded * hidden_size_padded + (long long)s * hidden_size_padded + h;
//                 long long y_ref_idx = (long long)b * seq_len * hidden_size + (long long)s * hidden_size + h;
//                 rmse += ((float)y[y_idx] - (float)y_ref[y_ref_idx]) * ((float)y[y_idx] - (float)y_ref[y_ref_idx]);
//             }
//         }
//     }
//     int y_size = batch_size * seq_len * hidden_size;
//     return sqrt(rmse / y_size);
// }

template<typename T>
float cal_rms_value(T* a, int a_size) {
    float sum_sq = 0.0f;
    for (int i = 0; i < a_size; ++i) {
        sum_sq += static_cast<float>(a[i]) * static_cast<float>(a[i]);
    }
    return std::sqrt(sum_sq / a_size);
}
template<typename Ta,typename Tb >
float get_cosine_similarity(Ta *y, Tb *y_ref, int batch_size, int seq_len, int hidden_size, int seq_len_padded, int hidden_size_padded)
{
    float dot_product = 0;
    float norm_y = 0;
    float norm_y_ref = 0;
    for (int b = 0; b < batch_size; ++b) {
        for (int s = 0; s < seq_len; ++s) {
            for (int h = 0; h < hidden_size; ++h) {
                long long y_idx = (long long)b * seq_len_padded * hidden_size_padded + (long long)s * hidden_size_padded + h;
                long long y_ref_idx = (long long)b * seq_len * hidden_size + (long long)s * hidden_size + h;
                dot_product += (float)y[y_idx] * (float)y_ref[y_ref_idx];
                norm_y += (float)y[y_idx] * (float)y[y_idx];
                norm_y_ref += (float)y_ref[y_ref_idx] * (float)y_ref[y_ref_idx];
            }
        }
    }
    return dot_product / (sqrt(norm_y) * sqrt(norm_y_ref));
}



template<typename Ta, typename Tb>
float get_max_abs_error(Ta *y, Tb *y_ref,
                         int batch_size, int seq_len, int hidden_size,
                         int seq_len_padded, int hidden_size_padded)
{
    float max_error = 0.0f;
    for (int b = 0; b < batch_size; ++b) {
        for (int s = 0; s < seq_len; ++s) {
            for (int h = 0; h < hidden_size; ++h) {
                long long y_idx = (long long)b * seq_len_padded * hidden_size_padded
                                + (long long)s * hidden_size_padded + h;
                long long y_ref_idx = (long long)b * seq_len * hidden_size
                                    + (long long)s * hidden_size + h;
                float error = std::abs(static_cast<float>(y[y_idx]) -
                                       static_cast<float>(y_ref[y_ref_idx]));
                max_error = std::max(max_error, error);
            }
        }
    }
    return max_error;
}

template<typename T_a, typename T_b>
void print_error_metrics(T_a*a, T_b*b, int batch_size, int seq_len, int hidden_size, int seq_len_padded, int hidden_size_padded){


    float rmse = get_rmse(a, b, batch_size, seq_len, hidden_size, seq_len_padded, hidden_size_padded);
    float relativeL1 = get_relativeL1(a, b, batch_size, seq_len, hidden_size, seq_len_padded, hidden_size_padded);
    float relativeL2 = get_relativeL2(a, b, batch_size, seq_len, hidden_size, seq_len_padded, hidden_size_padded);
    float cosine_similarity = get_cosine_similarity(a, b, batch_size, seq_len, hidden_size, seq_len_padded, hidden_size_padded);
    float max_error = get_max_abs_error(a, b, batch_size, seq_len, hidden_size, seq_len_padded, hidden_size_padded);



    header_print("info", "Relative L1: " << relativeL1);
    header_print("info", "Relative L2: " << relativeL2);
    header_print("info", "Cosine similarity: " << cosine_similarity);
    header_print("info", "RMSE: " << rmse << " | Max error: " << max_error);

}
#endif
