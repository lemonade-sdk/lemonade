#include <gtest/gtest.h>
#include "lemon/thread_manager.h"

// Test basic thread manager functionality
TEST(ThreadManagerTest, TestThreadAssignment) {
    lemon::ThreadManager thread_manager;
    
    // Test default assignment
    auto assignment = thread_manager.assign_threads(-1, lemon::ThreadAffinityMode::NONE);
    
    // Should return at least 1 thread
    EXPECT_GT(assignment.threads, 0);
    EXPECT_FALSE(assignment.affinity_string.empty());
    
    // Test specific thread count
    assignment = thread_manager.assign_threads(4, lemon::ThreadAffinityMode::NONE);
    EXPECT_EQ(assignment.threads, 4);
}

TEST(ThreadManagerTest, TestAffinityModes) {
    lemon::ThreadManager thread_manager;
    
    // Test all affinity modes
    auto assignment = thread_manager.assign_threads(4, lemon::ThreadAffinityMode::NONE);
    EXPECT_EQ(assignment.threads, 4);
    
    assignment = thread_manager.assign_threads(4, lemon::ThreadAffinityMode::SPREAD);
    EXPECT_EQ(assignment.threads, 4);
    
    assignment = thread_manager.assign_threads(4, lemon::ThreadAffinityMode::COMPACT);
    EXPECT_EQ(assignment.threads, 4);
    
    assignment = thread_manager.assign_threads(4, lemon::ThreadAffinityMode::NUMA);
    EXPECT_EQ(assignment.threads, 4);
    
    assignment = thread_manager.assign_threads(4, lemon::ThreadAffinityMode::CACHE);
    EXPECT_EQ(assignment.threads, 4);
}

TEST(ThreadManagerTest, TestSystemTopology) {
    lemon::ThreadManager thread_manager;
    
    // Test system topology detection (should not crash)
    auto topology = thread_manager.get_system_topology();
    
    // Should return valid topology info
    EXPECT_GT(topology.numa_nodes.size(), 0);
}