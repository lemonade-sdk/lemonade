#include "lemon/thread_manager.h"
#include "lemon/system_info.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <unistd.h>
#else
#include <unistd.h>
#include <numa.h>
#endif

namespace lemon {

using json = nlohmann::json;

// ============================================================================
// System Topology Detection
// ============================================================================

SystemTopology ThreadManager::detect_topology() {
    SystemTopology topology;
    
    // Get system info
    auto system_info = create_system_info();
    auto cpu_info = system_info->get_cpu_device();
    
    topology.num_threads = cpu_info.threads;
    topology.num_cores = cpu_info.cores;
    topology.threads_per_core = (topology.num_cores > 0) ? 
                                (topology.num_threads / topology.num_cores) : 1;
    
    // Detect NUMA nodes
#ifdef _WIN32
    // Windows NUMA detection
    DWORD num_numa_nodes = 0;
    GetNumaHighestNodeNumber(&num_numa_nodes);
    topology.num_numa_nodes = num_numa_nodes + 1;  // Nodes are 0-indexed
    
    // Get processor mask for each NUMA node
    for (WORD node = 0; node <= num_numa_nodes; node++) {
        ULONG64 mask = 0;
        USHORT processor_count = 0;
        GetNumaNodeProcessorMask(node, &mask);
        
        // Count processors in mask
        int count = 0;
        for (int i = 0; i < 64; i++) {
            if (mask & (1ULL << i)) count++;
        }
        topology.numa_cores.push_back(count);
        topology.numa_threads.push_back(count * topology.threads_per_core);
    }
#elif defined(__APPLE__)
    // macOS - no NUMA support
    topology.num_numa_nodes = 1;
    topology.numa_cores.push_back(topology.num_cores);
    topology.numa_threads.push_back(topology.num_threads);
#else
    // Linux NUMA detection
    if (numa_available() >= 0) {
        topology.num_numa_nodes = numa_max_node() + 1;
        
        for (int node = 0; node <= numa_max_node(); node++) {
            if (numa_node_to_cpus(node) != nullptr) {
                std::string cpuset = numa_node_to_cpus(node);
                // Parse CPU set to count cores
                int core_count = 0;
                std::istringstream iss(cpuset);
                std::string range;
                while (std::getline(iss, range, ',')) {
                    size_t dash = range.find('-');
                    if (dash != std::string::npos) {
                        int start = std::stoi(range.substr(0, dash));
                        int end = std::stoi(range.substr(dash + 1));
                        core_count += (end - start + 1) / topology.threads_per_core;
                    } else {
                        core_count++;
                    }
                }
                topology.numa_cores.push_back(core_count);
                topology.numa_threads.push_back(core_count * topology.threads_per_core);
            }
        }
    } else {
        topology.num_numa_nodes = 1;
        topology.numa_cores.push_back(topology.num_cores);
        topology.numa_threads.push_back(topology.num_threads);
    }
#endif
    
    // Detect CCDs (for AMD CPUs)
    // CCDs are typically groups of 2-4 cores with shared L3 cache
    // For AMD Ryzen/EPYC: CCD = 4 cores (2 physical + 2 virtual with SMT)
    int cores_per_ccd = 4;  // Typical for AMD Ryzen
    if (topology.num_cores > 0) {
        topology.num_ccds = (topology.num_cores + cores_per_ccd - 1) / cores_per_ccd;
        
        for (int i = 0; i < topology.num_ccds; i++) {
            int cores_in_ccd = std::min(cores_per_ccd, 
                                        topology.num_cores - i * cores_per_ccd);
            topology.ccd_cores.push_back(cores_in_ccd);
            topology.ccd_threads.push_back(cores_in_ccd * topology.threads_per_core);
        }
    }
    
    // Get cache sizes
#ifdef _WIN32
    // Windows cache detection via CPUID
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* buffer = nullptr;
    DWORD buffer_size = 0;
    
    GetLogicalProcessorInformationEx(RelationProcessorCore, buffer, &buffer_size);
    
    if (buffer_size > 0) {
        buffer = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)malloc(buffer_size);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, buffer, &buffer_size)) {
            char* ptr = (char*)buffer;
            while (ptr < (char*)buffer + buffer_size) {
                auto info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)ptr;
                
                if (info->Relationship == RelationProcessorCore) {
                    // L1/L2 caches are per-core
                    for (int i = 0; i < info->Processor.GroupCount; i++) {
                        for (USHORT j = 0; j < info->Processor.GroupMask[i].Mask; j++) {
                            if (info->Processor.Flags == 1) {  // Hyperthreaded
                                topology.l1_cache_size_kb = 32;  // Typical AMD L1
                                topology.l2_cache_size_kb = 512; // Typical AMD L2
                            }
                        }
                    }
                }
                ptr += info->Size;
            }
        }
        free(buffer);
    }
    
    // L3 cache (shared)
    GetLogicalProcessorInformationEx(RelationCache, buffer, &buffer_size);
    if (buffer_size > 0) {
        buffer = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)malloc(buffer_size);
        if (GetLogicalProcessorInformationEx(RelationCache, buffer, &buffer_size)) {
            char* ptr = (char*)buffer;
            while (ptr < (char*)buffer + buffer_size) {
                auto info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)ptr;
                if (info->Relationship == RelationCache) {
                    if (info->Cache.Level == 3) {
                        topology.l3_cache_size_mb = info->Cache.Size / (1024 * 1024);
                    }
                }
                ptr += info->Size;
            }
        }
        free(buffer);
    }
#elif defined(__APPLE__)
    // macOS cache detection via sysctl
    size_t len = sizeof(int);
    int l1d_size = 0, l2_size = 0, l3_size = 0;
    
    sysctlbyname("hw.l1icachesize", &l1d_size, &len, nullptr, 0);
    sysctlbyname("hw.l2cachesize", &l2_size, &len, nullptr, 0);
    sysctlbyname("hw.l3cachesize", &l3_size, &len, nullptr, 0);
    
    topology.l1_cache_size_kb = l1d_size / 1024;
    topology.l2_cache_size_kb = l2_size / 1024;
    topology.l3_cache_size_mb = l3_size / (1024 * 1024);
#else
    // Linux cache detection via sysfs
    std::string base_path = "/sys/devices/system/cpu/cpu0/cache/";
    
    auto read_cache_size = [](const std::string& path) -> int {
        std::ifstream file(path);
        if (!file.is_open()) return 0;
        int size;
        file >> size;
        return size;
    };
    
    topology.l1_cache_size_kb = read_cache_size(base_path + "level1/data/cache_size_kb");
    topology.l2_cache_size_kb = read_cache_size(base_path + "level2/cache_size_kb");
    topology.l3_cache_size_mb = read_cache_size(base_path + "level3/cache_size_kb") / 1024;
    
    // Fallback values if sysfs not available
    if (topology.l1_cache_size_kb == 0) topology.l1_cache_size_kb = 32;
    if (topology.l2_cache_size_kb == 0) topology.l2_cache_size_kb = 512;
    if (topology.l3_cache_size_mb == 0) topology.l3_cache_size_mb = 16;
#endif
    
    return topology;
}

// ============================================================================
// Thread Assignment Logic
// ============================================================================

ThreadAffinityMode ThreadManager::parse_mode(const std::string& mode_str) {
    if (mode_str == "none") return ThreadAffinityMode::NONE;
    if (mode_str == "spread") return ThreadAffinityMode::SPREAD;
    if (mode_str == "compact") return ThreadAffinityMode::COMPACT;
    if (mode_str == "numa") return ThreadAffinityMode::NUMA;
    if (mode_str == "cache") return ThreadAffinityMode::CACHE;
    return ThreadAffinityMode::NONE;
}

std::string ThreadManager::mode_to_string(ThreadAffinityMode mode) {
    switch (mode) {
        case ThreadAffinityMode::NONE: return "none";
        case ThreadAffinityMode::SPREAD: return "spread";
        case ThreadAffinityMode::COMPACT: return "compact";
        case ThreadAffinityMode::NUMA: return "numa";
        case ThreadAffinityMode::CACHE: return "cache";
        default: return "none";
    }
}

ThreadAssignment ThreadManager::assign_threads(int requested_threads,
                                              const SystemTopology& topology,
                                              ThreadAffinityMode mode,
                                              int num_models) {
    ThreadAssignment assignment;
    
    // Auto-detect thread count if not specified
    int total_threads = requested_threads;
    if (total_threads <= 0 || total_threads > topology.num_threads) {
        total_threads = topology.num_threads;
    }
    
    // If running multiple models, divide threads among them
    if (num_models > 1) {
        total_threads = std::max(1, total_threads / num_models);
    }
    
    assignment.num_threads = total_threads;
    
    switch (mode) {
        case ThreadAffinityMode::NUMA: {
            // NUMA-aware assignment (primary strategy)
            // Distribute threads across NUMA nodes
            
            if (topology.num_numa_nodes > 1) {
                // Calculate threads per NUMA node
                int threads_per_node = total_threads / topology.num_numa_nodes;
                int remainder = total_threads % topology.num_numa_nodes;
                
                for (int node = 0; node < topology.num_numa_nodes; node++) {
                    int node_threads = threads_per_node + (node < remainder ? 1 : 0);
                    
                    // Assign cores from this NUMA node
                    int cores_available = topology.numa_cores[node];
                    for (int i = 0; i < node_threads && i < cores_available; i++) {
                        // Calculate actual core ID
                        int core_id = node * topology.num_cores / topology.num_numa_nodes + i;
                        assignment.core_ids.push_back(core_id);
                        assignment.numa_nodes.push_back(node);
                    }
                }
            } else {
                // Single NUMA node - assign sequentially
                for (int i = 0; i < total_threads; i++) {
                    assignment.core_ids.push_back(i);
                    assignment.numa_nodes.push_back(0);
                }
            }
            break;
        }
        
        case ThreadAffinityMode::CACHE: {
            // Cache-aware assignment (secondary strategy)
            // Group threads within same CCD (shared L3 cache)
            
            for (int ccd = 0; ccd < topology.num_ccds && assignment.core_ids.size() < total_threads; ccd++) {
                int ccd_threads = std::min(topology.ccd_threads[ccd], 
                                          total_threads - (int)assignment.core_ids.size());
                
                for (int i = 0; i < ccd_threads; i++) {
                    int core_offset = ccd * 4;  // Assuming 4 cores per CCD
                    assignment.core_ids.push_back(core_offset + i);
                    assignment.ccd_ids.push_back(ccd);
                }
            }
            
            // If still need more threads, spill to other CCDs
            if (assignment.core_ids.size() < total_threads) {
                for (int i = 0; i < total_threads - (int)assignment.core_ids.size(); i++) {
                    assignment.core_ids.push_back(assignment.core_ids.size());
                    assignment.ccd_ids.push_back(i / 4);
                }
            }
            break;
        }
        
        case ThreadAffinityMode::SPREAD: {
            // Spread threads across cores (one thread per core)
            for (int i = 0; i < total_threads; i++) {
                assignment.core_ids.push_back(i);
                assignment.numa_nodes.push_back(0);
            }
            break;
        }
        
        case ThreadAffinityMode::COMPACT: {
            // Compact threads on fewer cores (hyperthreading friendly)
            for (int i = 0; i < total_threads; i++) {
                assignment.core_ids.push_back(i % topology.num_cores);
                assignment.numa_nodes.push_back(0);
            }
            break;
        }
        
        case ThreadAffinityMode::NONE:
        default: {
            // No affinity - let OS schedule
            break;
        }
    }
    
    return assignment;
}

// ============================================================================
// Argument Generation
// ============================================================================

std::vector<std::string> ThreadManager::generate_affinity_args(const ThreadAssignment& assignment,
                                                               const SystemTopology& topology) {
    std::vector<std::string> args;
    
    if (assignment.num_threads <= 0) {
        return args;
    }
    
    // Add -np argument for number of threads
    args.push_back("-np");
    args.push_back(std::to_string(assignment.num_threads));
    
    // Add thread affinity mask (platform-specific)
    // For Windows: use processor affinity masks
    // For Linux: use cpu-list format
    
#ifdef _WIN32
    // Windows uses hex bitmask
    if (!assignment.core_ids.empty()) {
        ULONG64 mask = 0;
        for (int core : assignment.core_ids) {
            mask |= (1ULL << core);
        }
        
        std::ostringstream oss;
        oss << std::hex << mask;
        args.push_back("--affinity-mask");
        args.push_back(oss.str());
    }
#else
    // Linux uses comma-separated CPU list
    if (!assignment.core_ids.empty()) {
        std::ostringstream oss;
        for (size_t i = 0; i < assignment.core_ids.size(); i++) {
            if (i > 0) oss << ",";
            oss << assignment.core_ids[i];
        }
        args.push_back("--cpu-list");
        args.push_back(oss.str());
    }
#endif
    
    return args;
}

// ============================================================================
// Thread Binding (Optional)
// ============================================================================

void ThreadManager::bind_threads(const ThreadAssignment& assignment) {
#ifdef _WIN32
    // Windows thread binding
    HANDLE thread = GetCurrentThread();
    
    if (!assignment.core_ids.empty()) {
        // Set affinity to first assigned core
        ULONG_PTR mask = 1ULL << assignment.core_ids[0];
        SetThreadAffinityMask(thread, mask);
    }
#else
    // Linux thread binding
    if (!assignment.core_ids.empty() && numa_available() >= 0) {
        // Bind to specific CPU
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(assignment.core_ids[0], &mask);
        sched_setaffinity(0, sizeof(mask), &mask);
    }
#endif
}

} // namespace lemon
