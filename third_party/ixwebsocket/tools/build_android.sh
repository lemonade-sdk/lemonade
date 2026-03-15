#!/bin/sh

#
# Executable : ${HOME}/Android/Sdk/cmake/3.6.3155560/bin/cmake
# arguments :
# -H${HOME}/Dev/github-projects/googlesamples/android-ndk/hello-jni/app/src/main/cpp
# -B${HOME}/Dev/github-projects/googlesamples/android-ndk/hello-jni/app/.cxx/cmake/arm7Debug/armeabi-v7a
# -GAndroid Gradle - Ninja
# -DANDROID_ABI=armeabi-v7a
# -DANDROID_NDK=${HOME}/Android/Sdk/ndk-bundle
# -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${HOME}/Dev/github-projects/googlesamples/android-ndk/hello-jni/app/build/intermediates/cmake/arm7/debug/obj/armeabi-v7a
# -DCMAKE_BUILD_TYPE=Debug
# -DCMAKE_MAKE_PROGRAM=${HOME}/Android/Sdk/cmake/3.6.3155560/bin/ninja
# -DCMAKE_TOOLCHAIN_FILE=${HOME}/Android/Sdk/ndk-bundle/build/cmake/android.toolchain.cmake
# -DANDROID_NATIVE_API_LEVEL=23
# -DANDROID_TOOLCHAIN=clang
# jvmArgs :
#
# To use a specific version of openSSL, add the arguments:
# -DOPENSSL_VERSION="[version]"
# -DOPENSSL_FOUND=1
# -DOPENSSL_INCLUDE_DIR=[include path]
# -DOPENSSL_LIBRARIES="[lib path]/libssl.a;[lib path]/libcrypto.a"
#

CMAKE_TOOLCHAIN_FILE=/tools/android/android-ndk-r20-darwin/build/cmake/android.toolchain.cmake
ANDROID_HOME=/tools/android/android-sdk-darwin
ANDROID_NDK=${ANDROID_HOME}/ndk-bundle
CMAKE_DIR=${ANDROID_HOME}/cmake/3.10.2.4988404/bin
CMAKE=${CMAKE_DIR}/cmake
NINJA=${CMAKE_DIR}/ninja

${CMAKE} \
    .. \
    -DANDROID_NATIVE_API_LEVEL=23 \
    -DANDROID_ABI=armeabi-v7a \
    -DANDROID_TOOLCHAIN=clang \
    -DANDROID_NDK=${ANDROID_NDK} \
    -G'Unix Makefiles' \
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} \
    -DCMAKE_MAKE_PROGRAM=make \
    -DUSE_TLS=1
