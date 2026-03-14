#!/bin/bash
# Update debian/changelog by replacing @@VERSION@@ sentinel with current version

set -e

# Extract version from CMakeLists.txt
VERSION=$(grep -oP 'project\(lemon_cpp VERSION \K[^)]+' ../CMakeLists.txt)
if [ -z "$VERSION" ]; then
    echo "Error: Could not extract version from CMakeLists.txt"
    exit 1
fi

# Replace @@VERSION@@ sentinel in debian/changelog
sed -i "s/@@VERSION@@/$VERSION/g" debian/changelog

echo "Updated debian/changelog - replaced @@VERSION@@ with $VERSION"
