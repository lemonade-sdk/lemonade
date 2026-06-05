/// \file debug_utils.hpp
/// \brief debug_utils class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This file contains the debug utilities for the FastFlowLM project.
#pragma once
#include <iostream>
#include <iomanip>
#include <string>

#ifndef VERBOSE
#define VERBOSE 0
#endif

/// \brief General logging macro
/// \param level the level of the log
/// \param msg the message to log
#if VERBOSE >= 1
    #define LOG_VERBOSE(level, msg) if (level <= VERBOSE) { std::cout << "[log" << level <<"] " << msg << std::endl; }
#else
    #define LOG_VERBOSE(level, msg) ((void)0) // No-op
#endif

/// \brief Conditional verbose logging macro
/// \param level the level of the log
/// \param condition the condition to log
/// \param msg the message to log
#if VERBOSE >= 1
    #define LOG_VERBOSE_IF(level, condition, msg) \
        if ((level <= VERBOSE) && (condition)) { std::cout << "[log" << level << "] " << msg << std::endl; }
#else
    #define LOG_VERBOSE_IF(level, condition, msg) ((void)0) // No-op
#endif

/// \brief Conditional verbose logging macro with else
/// \param level the level of the log
/// \param condition the condition to log
/// \param msg_true the message to log if the condition is true
/// \param msg_false the message to log if the condition is false
#if VERBOSE >= 1
    #define LOG_VERBOSE_IF_ELSE(level, condition, msg_true, msg_false) \
        if ((level <= VERBOSE) && (condition)) { std::cout << "[log" << level << "] " << msg_true << std::endl; } \
        else if ((level <= VERBOSE)) { std::cout << "[log" << level << "] " << msg_false << std::endl; }
#else
    #define LOG_VERBOSE_IF_ELSE(level, condition, msg_true, msg_false) ((void)0) // No-op
#endif

/// \brief DO_VERBOSE macro
/// \param level the level of the log
/// \param operations the operations to perform
#define DO_VERBOSE(level, operations) if (level <= VERBOSE) { operations }

/// \brief MSG_HLINE macro
/// \param box_width the width of the box
#define MSG_HLINE(box_width) std::cout << std::string(box_width, '-') << std::endl;

/// \brief MSG_BONDLINE macro
/// \param box_width the width of the box
#define MSG_BONDLINE(box_width) std::cout << '+' << std::string(box_width - 2, '-') << '+' << std::endl;

/// \brief MSG_BOX_LINE macro
/// \param box_width the width of the box
/// \param msg the message to log
#define MSG_BOX_LINE(box_width, msg)                          \
    do {                                                 \
        std::ostringstream oss;                          \
        oss << msg;                                      \
        std::cout << "| " << std::left                   \
                  << std::setw(box_width - 4) << oss.str() \
                  << " |" << std::endl;                  \
    } while (0)

/// \brief MSG_BOX macro
/// \param box_width the width of the box
/// \param msg the message stream
#define MSG_BOX(box_width, msg)                          \
    do {                                                 \
        std::ostringstream oss;                          \
        oss << msg;                                      \
        MSG_BONDLINE(box_width);                        \
        std::cout << "| " << std::left                   \
                  << std::setw(box_width - 4) << oss.str() \
                  << " |" << std::endl;                  \
        MSG_BONDLINE(box_width);                        \
    } while (0)

/// \brief HEADER_PRINT macro
/// \param header the header of the message
/// \param msg the message to log
#define HEADER_PRINT(header, msg) \
    do { \
        std::ostringstream oss; \
        oss << msg; \
        std::cout << '[' << header << "]  " << oss.str() << std::endl; \
    } while (0)

/// \brief OSTREAM2STRING macro
/// \param os the ostream to convert
#define OSTREAM2STRING(os) \
    do { \
        std::ostringstream oss; \
        oss << os; \
        return oss.str(); \
    } while (0)

/// \brief header_print macro
/// \param header the header of the message
/// \param msg the message to log
#define header_print(header, msg) \
    do { \
        std::ostringstream oss; \
        oss << msg; \
        std::cout << '[' << header << "]  " << oss.str() << std::endl; \
    } while (0)

/// \brief header_print_r macro, in red color
/// \param header the header of the message
/// \param msg the message to log
#define header_print_r(header, msg) \
    do { \
        std::ostringstream oss; \
        oss << msg; \
        std::cerr << "\033[31m[" << header << "]  " << oss.str() << "\033[0m" << std::endl; \
    } while (0)


/// \brief header_print_g macro, in green color
/// \param header the header of the message
/// \param msg the message to log
#define header_print_g(header, msg) \
    do { \
        std::ostringstream oss; \
        oss << msg; \
        std::cout << "\033[32m[" << header << "]  " << oss.str() << "\033[0m" << std::endl; \
    } while (0)


/// \brief header_print_g macro, in green color
/// \param header the header of the message
/// \param msg the message to log
#define header_print_b(header, msg) \
    do { \
        std::ostringstream oss; \
        oss << msg; \
        std::cout << "\033[34m[" << header << "]  " << oss.str() << "\033[0m" << std::endl; \
    } while (0)

/// \brief box_print macro
/// \param msg the message to log
/// \param width the width of the box
inline void box_print(std::string msg, int width = 40){
    MSG_BOX(width, msg);
}

/// \brief box_print_bound macro
/// \param width the width of the box
inline void box_print_bound(int width = 40){
    MSG_BONDLINE(width);
}

/// \brief box_print_line macro
/// \param msg the message to log
/// \param width the width of the box
inline void box_print_line(std::string msg, int width = 40){
    MSG_BOX_LINE(width, msg);
}

/// \brief size_t_to_string macro
/// \param size the size to convert
/// \return the string representation of the size
inline std::string size_t_to_string(size_t size){
    if (size /  1024 == 0) {
        return std::to_string(size);
    } else if (size / (1024 * 1024) == 0) {
        return std::to_string(size / 1024) + "K";
    } else if (size / (1024 * 1024) == 0){
        return std::to_string(size / (1024 * 1024)) + "M";
    } else {
        return std::to_string(size / (1024 * 1024 * 1024)) + "G";
    }
}
