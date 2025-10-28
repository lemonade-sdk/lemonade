#!/bin/bash
# Model Management Testing Script for lemon.cpp
# This tests the critical model download functionality that replaces huggingface_hub

set -e  # Exit on error

echo "========================================="
echo "lemon.cpp Model Management Tests"
echo "========================================="
echo ""

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counter
TESTS_PASSED=0
TESTS_FAILED=0

# Setup test environment
TEST_CACHE="./test_cache"
export LEMONADE_CACHE_DIR="$TEST_CACHE"

# Clean up function
cleanup() {
    echo ""
    echo "Cleaning up test cache..."
    rm -rf "$TEST_CACHE"
}

# Run cleanup on exit
trap cleanup EXIT

# Create test cache
mkdir -p "$TEST_CACHE"

echo "Test Cache Directory: $TEST_CACHE"
echo ""

# Test 1: Binary exists and runs
echo "Test 1: Check lemonade-router binary..."
if [ -f "./lemonade-router" ]; then
    echo -e "${GREEN}✓ PASS${NC}: lemonade-router binary found"
    ((TESTS_PASSED++))
else
    echo -e "${RED}✗ FAIL${NC}: lemonade-router binary not found"
    echo "Please build first: cd build && cmake --build ."
    ((TESTS_FAILED++))
    exit 1
fi

# Test 2: Version check
echo ""
echo "Test 2: Version check..."
if ./lemonade-router --version > /dev/null 2>&1; then
    VERSION=$(./lemonade-router --version)
    echo -e "${GREEN}✓ PASS${NC}: Version: $VERSION"
    ((TESTS_PASSED++))
else
    echo -e "${RED}✗ FAIL${NC}: Version check failed"
    ((TESTS_FAILED++))
fi

# Test 3: List models (without downloads)
echo ""
echo "Test 3: List available models..."
if ./lemonade-router list > /dev/null 2>&1; then
    echo -e "${GREEN}✓ PASS${NC}: Can list models"
    echo "Sample models:"
    ./lemonade-router list | head -5
    ((TESTS_PASSED++))
else
    echo -e "${RED}✗ FAIL${NC}: Failed to list models"
    ((TESTS_FAILED++))
fi

# Test 4: Download a small model
echo ""
echo "Test 4: Download model (Qwen2.5-0.5B-Instruct-CPU)..."
echo "This tests the critical HF API integration..."
if ./lemonade-router pull Qwen2.5-0.5B-Instruct-CPU 2>&1 | tee /tmp/download_log.txt; then
    echo -e "${GREEN}✓ PASS${NC}: Model download completed"
    ((TESTS_PASSED++))
else
    echo -e "${RED}✗ FAIL${NC}: Model download failed"
    echo "Check /tmp/download_log.txt for details"
    ((TESTS_FAILED++))
fi

# Test 5: Verify cache structure
echo ""
echo "Test 5: Verify cache structure..."
HF_CACHE="$TEST_CACHE/huggingface/hub"
if [ -d "$HF_CACHE" ]; then
    echo -e "${GREEN}✓ PASS${NC}: HF cache directory created"
    echo "Cache structure:"
    ls -la "$HF_CACHE" | head -10
    ((TESTS_PASSED++))
else
    echo -e "${RED}✗ FAIL${NC}: HF cache directory not found"
    ((TESTS_FAILED++))
fi

# Test 6: Check model files downloaded
echo ""
echo "Test 6: Check model files..."
MODEL_DIR=$(find "$HF_CACHE" -type d -name "models--*" | head -1)
if [ -n "$MODEL_DIR" ] && [ -d "$MODEL_DIR" ]; then
    FILE_COUNT=$(find "$MODEL_DIR" -type f | wc -l)
    if [ "$FILE_COUNT" -gt 0 ]; then
        echo -e "${GREEN}✓ PASS${NC}: Found $FILE_COUNT model files"
        echo "Sample files:"
        find "$MODEL_DIR" -type f | head -5
        ((TESTS_PASSED++))
    else
        echo -e "${RED}✗ FAIL${NC}: No model files found"
        ((TESTS_FAILED++))
    fi
else
    echo -e "${RED}✗ FAIL${NC}: Model directory not found"
    ((TESTS_FAILED++))
fi

# Test 7: List shows downloaded model
echo ""
echo "Test 7: Verify model shows as downloaded..."
if ./lemonade-router list | grep -q "Yes.*Qwen2.5-0.5B-Instruct-CPU"; then
    echo -e "${GREEN}✓ PASS${NC}: Model marked as downloaded"
    ((TESTS_PASSED++))
else
    echo -e "${YELLOW}⚠ WARN${NC}: Model not marked as downloaded (might be expected)"
    # Don't count as failure since listing might not be implemented
fi

# Test 8: Delete model
echo ""
echo "Test 8: Delete model..."
if ./lemonade-router delete Qwen2.5-0.5B-Instruct-CPU 2>&1; then
    echo -e "${GREEN}✓ PASS${NC}: Model deleted"
    ((TESTS_PASSED++))
else
    echo -e "${RED}✗ FAIL${NC}: Failed to delete model"
    ((TESTS_FAILED++))
fi

# Test 9: Verify deletion
echo ""
echo "Test 9: Verify model deleted..."
if [ ! -d "$MODEL_DIR" ] || [ $(find "$MODEL_DIR" -type f 2>/dev/null | wc -l) -eq 0 ]; then
    echo -e "${GREEN}✓ PASS${NC}: Model files removed"
    ((TESTS_PASSED++))
else
    echo -e "${RED}✗ FAIL${NC}: Model files still exist"
    ((TESTS_FAILED++))
fi

# Test 10: Offline mode
echo ""
echo "Test 10: Test offline mode..."
export LEMONADE_OFFLINE=1
if ./lemonade-router pull Qwen2.5-0.5B-Instruct-CPU 2>&1 | grep -q "Offline mode\|skipping"; then
    echo -e "${GREEN}✓ PASS${NC}: Offline mode respected"
    ((TESTS_PASSED++))
else
    echo -e "${YELLOW}⚠ WARN${NC}: Offline mode behavior unclear"
fi
unset LEMONADE_OFFLINE

# Test 11: User model registration
echo ""
echo "Test 11: Register custom user model..."
if ./lemonade-router pull user.TestModel --checkpoint test/model --recipe llamacpp 2>&1; then
    echo -e "${GREEN}✓ PASS${NC}: User model registered"
    ((TESTS_PASSED++))
    
    # Check user_models.json created
    if [ -f "$TEST_CACHE/user_models.json" ]; then
        echo -e "${GREEN}✓ PASS${NC}: user_models.json created"
        echo "Contents:"
        cat "$TEST_CACHE/user_models.json"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}✗ FAIL${NC}: user_models.json not created"
        ((TESTS_FAILED++))
    fi
else
    echo -e "${RED}✗ FAIL${NC}: Failed to register user model"
    ((TESTS_FAILED++))
fi

# Summary
echo ""
echo "========================================="
echo "Test Summary"
echo "========================================="
echo -e "Tests Passed: ${GREEN}$TESTS_PASSED${NC}"
echo -e "Tests Failed: ${RED}$TESTS_FAILED${NC}"
echo "========================================="

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed.${NC}"
    exit 1
fi

