#!/bin/bash
# Bash script to stop server and rebuild lemon.cpp

echo "Checking for running server..."

# Try to stop the server if it's running
./build/lemonade stop 2>/dev/null

echo "Building lemon.cpp..."

# Navigate to build directory and build
cd build
cmake --build . --config Release
BUILD_RESULT=$?
cd ..

if [ $BUILD_RESULT -eq 0 ]; then
    echo -e "\nBuild successful! ✓"
    echo -e "\nYou can now run:"
    echo "  ./build/lemonade serve"
else
    echo -e "\nBuild failed! ✗"
    exit $BUILD_RESULT
fi

