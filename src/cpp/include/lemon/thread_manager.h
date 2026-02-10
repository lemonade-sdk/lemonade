#pragma once

#include <string>
#include <vector>

namespace lemon {

// Thread assignment modes
enum class ThreadAffinityMode {
    NONE,      // No specific affinity - let OS schedule freely
    SPREAD,    // Spread threads across cores (one thread per core)
    COMPACT,   // Compact threads on fewer cores (hyperthreading friendly)
    NUMA,      // NUMA-aware assignment (primary strategy)
    CACHE      // Cache-aware assignment (secondary strategy)
};

// System topology information
struct SystemTopology {
    int num_numa_nodes = 0;
    int num_cores = 0;
    int num_threads = 0;
    int num_ccds = 0;  // CCDs per socket (for AMD CPUs)
    int threads_per_core = 1;
    
    // Per-CCD information
    std::vector<int> ccd_cores;     // Cores per CCD
    std::vector<int> ccd_threads;   // Threads per CCD
    
    // Per-NUMA node information
    std::vector<int> numa_cores;    // Cores per NUMA node
    std::vector<int> numa_threads;  // Threads per NUMA node
    
    // Cache hierarchy
    int l1_cache_size_kb = 0;
    int l2_cache_size_kb = 0;
    int l3_cache_size_mb = 0;
};

// Thread assignment result
struct ThreadAssignment {
    int num_threads = 0;
    std::vector<int> core_ids;      // Assigned core IDs
    std::vector<int> numa_nodes;    // Assigned NUMA nodes
    std::vector<int> ccd_ids;       // Assigned CCDs
    
    // Affinity mask (platform-specific)
    std::string affinity_mask;
};

// Thread management utilities
class ThreadManager {
public:
    // Detect system topology
    static SystemTopology detect_topology();
    
    // Calculate optimal thread assignment based on mode
    static ThreadAssignment assign_threads(int requested_threads,
                                          const SystemTopology& topology,
                                          ThreadAffinityMode mode,
                                          int num_models = 1);
    
    // Get mode from string
    static ThreadAffinityMode parse_mode(const std::string& mode_str);
    
    // Get mode as string
    static std::string mode_to_string(ThreadAffinityMode mode);
    
    // Generate llamacpp arguments for thread assignment
    static std::vector<std::string> generate_affinity_args(const ThreadAssignment& assignment,
                                                          const SystemTopology& topology);
    
    // Platform-specific thread binding (optional)
    static void bind_threads(const ThreadAssignment& assignment);
};

} // namespace lemon
