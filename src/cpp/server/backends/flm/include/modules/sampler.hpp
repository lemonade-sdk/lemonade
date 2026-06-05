/// \file sampler.hpp
/// \brief sampler class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This class is used to sample the tokens.
#pragma once

#include "typedef.hpp"
#include <cstdint>
#include <deque>
#include <random>
#include <unordered_map>

/// \brief sampler config
/// \param temperature the temperature
/// \param top_k the top k
/// \param top_p the top p
/// \param rep_penalty the rep penalty
/// \param freq_penalty the freq penalty
/// \param rep_penalty_window the rep penalty window
typedef struct sampler_config_{
    int top_k = 40;
    float top_p = 0.9f;
    float min_p = 0.1f;
    float temperature = 0.8f;
    float rep_penalty = 1.0f;
    float freq_penalty = 0.0f;
    float pre_penalty = 0.0f;
    int rep_penalty_window = 64;
    int freq_penalty_window = 64;  // Window size for frequency penalty
    int repeat_last_n = 64;
    bool use_optimized_sampling = true;
    bool has_rng_seed = false;
    uint64_t rng_seed = 0;
} sampler_config;

//typedef std::pair<float, int> logits_t;

typedef struct {
    float logits;
    int token_id;
    float prob;
} logits_t;

typedef std::vector<logits_t> logits_list_t;

class Sampler{
public:
    std::vector<float> logits;
    int in_features;
    std::vector<int> counters;
    logits_list_t top_k_logits;
    float rep_penalty;
    float freq_penalty;
    float pre_penalty;
    int top_k;
    float top_p;
    float min_p;
    float temperature;
    int total_tokens;
    std::vector<int> token_positions;

    // Ring buffer for frequency tracking
    std::deque<int> token_history;
    size_t freq_penalty_window;
    size_t rep_penalty_window;
    size_t repeat_last_n;

    std::unordered_map<int, int> token_counts_sparse;

    std::mt19937_64 rng_{};
    std::uniform_real_distribution<float> uniform_dist_{0.0f, 1.0f};
    bool use_optimized_sampling = true;

    /// \brief Constructor
    /// \param in_features the input features
    /// \param config the configuration
    Sampler() = default;
    Sampler(int in_features, sampler_config& config);
    void set_seed(uint64_t seed);

    /// \brief Reset the penalties
    /// \note The function will reset the penalties
    /// \note The function will reset the token positions
    /// \note The function will reset the token history
    /// \note The function will reset the total tokens
    /// \note The function will reset the token positions
    void reset_penalties();

    void softmax_inplace();
    void softmax_with_topp_minp(float top_p_threshold, float min_p_threshold);

    void sampler_penalty_apply();
    void sampler_penalty_apply_sparse();

    void sampler_topk_apply(int k);
    void sampler_topp_apply(float p);
    void sampler_minp_apply(float p);
    void sampler_temp_apply(float temp);
    int sample_from_probs();
    void ring_buffer_update(int sampled_index);
    void ring_buffer_update_sparse(int sampled_index);

    /// \brief Sample the token
    /// \param x the input buffer
    /// \return the sampled token
    int sample(buffer<bf16>& x);
};
