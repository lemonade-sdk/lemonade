/// \file profiler.hpp
/// \brief profiler class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This class is used to profile the code.
#pragma once

#include "utils/utils.hpp"

/// \brief profiler class
class profiler{
public:
    profiler(){
        this->start_time = time_utils::now();
        this->counter = 0;
        this->total_time = {0, "us"};
    }

    /// \brief start the profiler
    time_utils::time_point start(){
        this->start_time = time_utils::now();
        return this->start_time;
    }

    /// \brief stop the profiler
    /// \param elements the number of elements
    /// \param overwrite the overwrite flag
    time_utils::time_point stop(size_t elements, bool overwrite = false){
        time_utils::time_point end_time = time_utils::now();
        time_utils::time_with_unit duration = time_utils::duration_us(this->start_time, end_time);
        this->total_time.first += duration.first;
        if (overwrite){
            this->counter = elements;
        }
        else{
            this->counter += elements;
        }
        return end_time;
    }

    /// \brief reset the profiler
    void reset(){
        this->start_time = time_utils::now();
        this->counter = 0;
        this->total_time = {0, "us"};
    }

    /// \brief get the total time
    /// \return the total time
    time_utils::time_with_unit get_total_time(){
        time_utils::time_with_unit auto_time = time_utils::re_unit(this->total_time);
        return auto_time;
    }

    /// \brief get the average time
    /// \return the average time
    float get_average_time(){
        time_utils::time_with_unit time_in_second = time_utils::cast_to_s(this->total_time);
        return time_in_second.first / this->counter;
    }

    /// \brief get the average speed
    /// \return the average speed
    float get_average_speed(){
        time_utils::time_with_unit time_in_second = time_utils::cast_to_s(this->total_time);
        return this->counter / time_in_second.first;
    }

    /// \brief get the counter
    /// \return the counter
    size_t get_counter(){
        return this->counter;
    }
private:
    time_utils::time_point start_time;
    time_utils::time_point last_time;
    time_utils::time_with_unit total_time;
    size_t counter;
    std::vector<time_utils::time_with_unit> time_list; // time_list[0] is the total time, time_list[1] is the average time per element

};
