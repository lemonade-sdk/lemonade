/// \file utils.hpp
/// \brief utils class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This file contains some utility functions for the FastFlowLM project.
#pragma once

#include "typedef.hpp"
#include "buffer.hpp"
#include "debug_utils.hpp"
#include <chrono>
#include <iostream>
#include <fstream>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <limits.h>
#include <cstdlib>
#endif

namespace time_utils {

typedef std::chrono::high_resolution_clock::time_point time_point;
typedef std::pair<float, std::string> time_with_unit;

/// \brief now
/// \return the current time
inline time_point now(){
    return std::chrono::high_resolution_clock::now();
}

/// \brief duration in nanoseconds
/// \param start the start time
/// \param stop the stop time
/// \return the duration in nanoseconds
inline time_with_unit duration_ns(time_point start, time_point stop){
    return std::make_pair(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count(), "ns");
}

/// \brief duration in microseconds
/// \param start the start time
/// \param stop the stop time
/// \return the duration in microseconds
inline time_with_unit duration_us(time_point start, time_point stop){
    return std::make_pair(std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count(), "us");
}

/// \brief duration in milliseconds
/// \param start the start time
/// \param stop the stop time
/// \return the duration in milliseconds
inline time_with_unit duration_ms(time_point start, time_point stop){
    return std::make_pair(std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count(), "ms");
}

/// \brief duration in seconds
/// \param start the start time
/// \param stop the stop time
/// \return the duration in seconds
inline time_with_unit duration_s(time_point start, time_point stop){
    return std::make_pair(std::chrono::duration_cast<std::chrono::seconds>(stop - start).count(), "s");
}

/// \brief cast to microseconds
/// \param time the time
/// \return the time in microseconds
inline time_with_unit cast_to_us(time_with_unit time){
    if (time.second == "ns"){
        return std::make_pair(time.first / 1000, "us");
    }
    if (time.second == "us"){
        return time;
    }
    if (time.second == "ms"){
        return std::make_pair(time.first * 1000, "us");
    }
    if (time.second == "s"){
        return std::make_pair(time.first * 1000000, "us");
    }
    else{
        return time;
    }
}

/// \brief cast to milliseconds
/// \param time the time
/// \return the time in milliseconds
inline time_with_unit cast_to_ms(time_with_unit time){
    if (time.second == "ns"){
        return std::make_pair(time.first / 1000000, "ms");
    }
    if (time.second == "ms"){
        return time;
    }
    if (time.second == "us"){
        return std::make_pair(time.first / 1000, "ms");
    }
    if (time.second == "s"){
        return std::make_pair(time.first / 1000000, "ms");
    }
    else{
        return time;
    }
}

/// \brief cast to seconds
/// \param time the time
/// \return the time in seconds
inline time_with_unit cast_to_s(time_with_unit time){
    if (time.second == "ns"){
        return std::make_pair(time.first / 1000000000, "s");
    }
    if (time.second == "s"){
        return time;
    }
    if (time.second == "us"){
        return std::make_pair(time.first / 1000000, "s");
    }
    if (time.second == "ms"){
        return std::make_pair(time.first / 1000, "s");
    }
    else{
        return time;
    }
}

/// \brief re-unit the time
/// \param time the time
/// \return the re-unit time
inline time_with_unit re_unit(time_with_unit time){
    time = cast_to_us(time);
    float time_us = time.first;
    std::string time_unit = time.second;
    if (time_us > 1000000){
        time_us /= 1000000;
        time_unit = "s";
    }
    else if (time_us > 1000){
        time_us /= 1000;
        time_unit = "ms";
    }
    return std::make_pair(time_us, time_unit);
}

} // end of namespace time_utils

namespace utils {

#ifdef _WIN32
inline void enable_ansi_on_windows_once() {
    static bool done = false;
    if (done) return;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);
    done = true;
}
#else
inline void enable_ansi_on_windows_once() {}
#endif

/// \brief get a random float
/// \param min the minimum value
/// \param max the maximum value
/// \return the random float
inline float getRand(float min = 0, float max = 1){
    return (float)(rand()) / (float)(RAND_MAX) * (max - min) + min;
}

/// \brief get a random integer
/// \param min the minimum value
/// \param max the maximum value
/// \return the random integer
inline int getRandInt(int min = 0, int max = 6){
    return (int)(rand()) % (max - min) + min;
}

/// \brief print the progress bar
/// \param os the output stream
/// \param progress the progress
/// \param len the length of the progress bar
inline void print_progress_bar(std::ostream &os, double progress, int len = 75) {
    os  << "\r" << std::string((int)(progress * len), '|')
        << std::string(len - (int)(progress * len), ' ') << std::setw(4)
        << std::fixed << std::setprecision(0) << progress * 100 << "%"
        << "\r";
}

/// \brief check if the file exists
/// \param name the name of the file
inline void check_arg_file_exists(std::string name) {
    // Attempt to open the file
    std::ifstream file(name);

    // Check if the file was successfully opened
    if (!file) {
        throw std::runtime_error("Error: File '" + name + "' does not exist or cannot be opened.");
    }

    // Optionally, close the file (not strictly necessary, as ifstream will close on destruction)
    file.close();
}


/// \brief print the npu profile
/// \param npu_time the npu time
/// \param op the operation
/// \param n_iter the number of iterations
inline void print_npu_profile(time_utils::time_with_unit npu_time, float op, int n_iter = 1){
    npu_time.first /= n_iter;
    time_utils::time_with_unit time_united = time_utils::re_unit(npu_time);
    time_united = time_utils::cast_to_s(time_united);


    float ops = op / (time_united.first);
    float speed = ops / 1000000;
    std::string ops_unit = "Mops";
    std::string speed_unit = "Mops/s";
    if (speed > 1000){
        speed /= 1000;
        speed_unit = "Gops/s";
    }
    if (speed > 1000000){
        speed /= 1000000;
        speed_unit = "Tops/s";
    }

    time_united = time_utils::re_unit(npu_time);
    MSG_BONDLINE(40);
    MSG_BOX_LINE(40, "NPU time : " << time_united.first << " " << time_united.second);
    MSG_BOX_LINE(40, "NPU speed: " << speed << " " << speed_unit);
    MSG_BONDLINE(40);
}

/// \brief compare the vectors
/// \param y the y
/// \param y_ref the y reference
/// \param print_errors the number of errors to print
/// \param abs_tol the absolute tolerance
/// \param rel_tol the relative tolerance
/// \return the number of errors
template <typename T>
inline int compare_vectors(buffer<T>& y, buffer<T>& y_ref, int print_errors = 16, float abs_tol = 1e-1, float rel_tol = 1e-1){
    int total_errors = 0;
    for (int i = 0; i < y.size(); i++){
        if (std::abs(y[i] - y_ref[i]) > abs_tol && std::abs(y[i] - y_ref[i]) / std::abs(y_ref[i]) > rel_tol){
            if (total_errors < print_errors){
                header_print("err ", "Error: y[" << i << "] = " << y[i] << " != y_ref[" << i << "] = " << y_ref[i]);
            }
            total_errors++;
        }
    }
    if (total_errors > 0){
        header_print("err ", "Total errors: " << total_errors);
    }
    return total_errors;
}

/// \brief print the matrix
/// \param matrix the matrix
/// \param n_cols the number of columns
/// \param n_printable_rows the number of printable rows
/// \param n_printable_cols the number of printable columns
/// \param ostream the output stream
/// \param col_sep the column separator
/// \param elide_sym the elide symbol
/// \param w the width
template <typename T>
inline void print_matrix(const buffer<T> matrix, int n_cols,
                  int n_printable_rows = 10, int n_printable_cols = 10,
                  std::ostream &ostream = std::cout,
                  const char col_sep[] = "  ", const char elide_sym[] = " ... ",
                  int w = -1) {
  assert(matrix.size() % n_cols == 0);

  if (w == -1) {
    w = 6;
  }
  int n_rows = matrix.size() / n_cols;
  DO_VERBOSE(1, {
    header_print("info", "Matrix size: " << "[" << n_rows << ", " << n_cols << "]");
  });

  n_printable_rows = std::min(n_rows, n_printable_rows);
  n_printable_cols = std::min(n_cols, n_printable_cols);

  const bool elide_rows = n_printable_rows < n_rows;
  const bool elide_cols = n_printable_cols < n_cols;

  if (elide_rows || elide_cols) {
    w = std::max((int)w, (int)strlen(elide_sym));
  }

  w += 3; // for decimal point and two decimal digits
  ostream << std::fixed << std::setprecision(2);

#define print_row(what)                                                        \
  for (int col = 0; col < (n_printable_cols + 1) / 2; col++) {                 \
    ostream << std::right << std::setw(w) << std::scientific << std::setprecision(2) << (what);                           \
    ostream << std::setw(0) << col_sep;                                        \
  }                                                                            \
  if (elide_cols) {                                                            \
    ostream << std::setw(0) << elide_sym;                                      \
  }                                                                            \
  for (int i = 0; i < n_printable_cols / 2; i++) {                             \
    [[maybe_unused]]int col = n_cols - n_printable_cols / 2 + i;                               \
    ostream << std::right << std::setw(w) << std::scientific << std::setprecision(2) << (what);                           \
    ostream << std::setw(0) << col_sep;                                        \
  }

  for (int row = 0; row < (n_printable_rows + 1) / 2; row++) {
    print_row(matrix[row * n_cols + col]);
    ostream << std::endl;
  }
  if (elide_rows) {
    print_row(elide_sym);
    ostream << std::endl;
  }
  for (int i = 0; i < n_printable_rows / 2; i++) {
    int row = n_rows - n_printable_rows / 2 + i;
    print_row(matrix[row * n_cols + col]);
    ostream << std::endl;
  }

#undef print_row
}

/// \brief check if the file exists
/// \param name the name of the file
/// \return true if the file exists, false otherwise
inline bool check_file_exists(std::string name) {
    std::ifstream file(name);
    return file.good();
}


/// \brief get the user's Documents directory on Windows or config directory on Linux
/// \return the user's Documents directory path on Windows, ~/.config on Linux
std::string get_user_directory();

///@brief get_executable_directory gets the directory where the executable is located
///@return the executable directory path
std::string get_executable_directory();

template<typename... fileName>
inline std::string path_join(fileName&&... args){
#ifdef _WIN32
    constexpr const char* sep = "\\";
#else
    constexpr const char* sep = "/";
#endif
    std::string path;
    ((path += std::string(args) + sep), ...);
    if (!path.empty()){
        path.pop_back(); // remove the last separator
    }
    return path;
}

///@brief find and return the path to model_list.json
///@return path to model_list.json
std::string find_model_list();

///@brief get the path to the xclbin directory
///@return path to the xclbin directory
std::string find_xclbin_path();

///@brief get_server_port gets the server port from environment variable FLM_SERVE_PORT
///@return the server port, default is 52625 if environment variable is not set
int get_server_port(int user_port);

///@brief get_models_directory gets the models directory from environment variable or defaults to Documents/flm/models on Windows or ~/.config/flm on Linux
///@return the models directory path
std::string get_models_directory();

} // end of namespace utils
