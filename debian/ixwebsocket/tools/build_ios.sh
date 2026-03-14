#!/bin/sh

#
# This script creates static libraries for macOS and iOS devices
#
# To use a specific OpenSSL version add the arguments:
# -DOPENSSL_VERSION="[version]" \
# -DOPENSSL_FOUND=1 \
# -DOPENSSL_INCLUDE_DIR=[include path] \
# -DOPENSSL_LIBRARIES="[lib path]/libssl.a;[lib path]/libcrypto.a" \
#

DEPLOYMENT_TARGET_IOS='12.0'
DEPLOYMENT_TARGET_MAC='14.0'
CMAKE_DIR=/Applications/CMake.app/Contents/bin
CMAKE=${CMAKE_DIR}/cmake

${CMAKE} \
    .. \
    -GXcode \
    -DUSE_TLS=1

mkdir -p ios sim mac

xcodebuild -project 'ixwebsocket.xcodeproj' -target "ixwebsocket" -sdk iphoneos ARCHS='arm64' TARGET_BUILD_DIR='$(PWD)/ios' BUILT_PRODUCTS_DIR='$(PWD)/ios' TARGET_NAME='ixwebsocket' IPHONEOS_DEPLOYMENT_TARGET=${DEPLOYMENT_TARGET_IOS}
xcodebuild -project 'ixwebsocket.xcodeproj' -target "ixwebsocket" -sdk iphonesimulator ARCHS='x86_64' TARGET_BUILD_DIR='$(PWD)/sim' BUILT_PRODUCTS_DIR='$(PWD)/sim' TARGET_NAME='ixwebsocket' IPHONEOS_DEPLOYMENT_TARGET=${DEPLOYMENT_TARGET_IOS}
xcodebuild -project 'ixwebsocket.xcodeproj' -target "ixwebsocket" -destination 'platform=OS X' ARCHS='x86_64 arm64' TARGET_BUILD_DIR='$(PWD)/mac' BUILT_PRODUCTS_DIR='$(PWD)/mac' TARGET_NAME='ixwebsocket' MACOSX_DEPLOYMENT_TARGET=${DEPLOYMENT_TARGET_MAC}

rm -rf ./out/*.a
rm -rf ./out/ios/*.a
rm -rf ./out/mac/*.a
mkdir -p out
mkdir -p ./out/ios ./out/mac
lipo -create "./ios/libixwebsocket.a" "./sim/libixwebsocket.a" -output ./out/ios/libixwebsocket.a
cp "./mac/libixwebsocket.a" ./out/mac/libixwebsocket.a
