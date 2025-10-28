#!/bin/bash
# Test lemon.cpp with existing Hugging Face cache

echo "========================================="
echo "Testing lemon.cpp with Existing HF Cache"
echo "========================================="
echo ""

# Check if lemonade-router binary exists
if [ ! -f "./lemonade-router" ]; then
    echo "Error: lemonade-router binary not found"
    echo "Please build first: cd build && cmake --build ."
    exit 1
fi

echo "1. Checking HF cache location..."
HF_CACHE="$HOME/.cache/huggingface/hub"
if [ -d "$HF_CACHE" ]; then
    echo "✓ HF cache found at: $HF_CACHE"
    MODEL_COUNT=$(ls -1 "$HF_CACHE" | grep "^models--" | wc -l)
    echo "  Found $MODEL_COUNT model directories"
    echo ""
    echo "  Sample models in cache:"
    ls -1 "$HF_CACHE" | grep "^models--" | head -5
else
    echo "✗ HF cache not found at: $HF_CACHE"
    exit 1
fi

echo ""
echo "2. Testing lemonade-router list command..."
echo "   This should detect your existing downloaded models"
echo ""
./lemonade-router list

echo ""
echo "3. Check if lemonade-router detected any downloaded models..."
echo "   Look for 'Yes' in the Downloaded column above"
echo ""

echo "4. Testing model info lookup..."
# Try to get info on a common model (adjust if needed)
echo "   Attempting to get info on first model in cache..."
FIRST_MODEL=$(ls -1 "$HF_CACHE" | grep "^models--" | head -1)
if [ -n "$FIRST_MODEL" ]; then
    echo "   Cache dir: $FIRST_MODEL"
    echo "   Files in model:"
    find "$HF_CACHE/$FIRST_MODEL" -type f | head -5
fi

echo ""
echo "========================================="
echo "Test Complete"
echo "========================================="
echo ""
echo "VERIFICATION CHECKLIST:"
echo "[ ] Does 'lemonade-router list' run without errors?"
echo "[ ] Does it show models you've already downloaded?"
echo "[ ] Are the models marked with 'Yes' in Downloaded column?"
echo ""
echo "If the answers are YES, the cache compatibility is working!"

