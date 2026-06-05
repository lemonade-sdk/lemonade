# =============================================================================
# Llama NPU Library Makefile
# =============================================================================
#
# Author: Zhenyu Xu (zxu3@clemson.edu)
# Created: 2024
#
# This Makefile is used to build the Llama NPU library components.
#
# Usage:
#   make        - Build all targets
#   make clean  - Remove all built files
#   make help   - Show this help message
#
# =============================================================================

-include ../common.mk

SOURCES += test.cpp
SOURCES += ../../common/AutoModel/automodel.cpp
SOURCES += ../../common/modules/sampler.cpp
SOURCES += ../../common/tokenizer/tokenizer.cpp
SOURCES += ../../common/whisper/modeling_whisper_audio.cpp
SOURCES += ../../common/whisper/modeling_whisper.cpp

HEADERS += ../../include/llama/whisper_npu.hpp
HEADERS += ../../include/llama/whisper_npu_sequence.hpp
HEADERS += ../../include/modules/embedding.hpp
HEADERS += ../../include/modules/lm_head.hpp
HEADERS += ../../include/modules/gemm.hpp
HEADERS += ../../include/modules/dequant.hpp

ifeq ($(WSL), 0)
# Linux build environment
# Use g++-13 directly without CMake

LDFLAGS += -lwhisper_npu
LDFLAGS += -lfftw3 -lfftw3f -lfftw3l
LDFLAGS += -lavformat -lavcodec -lavutil -lswscale -lswresample


TEST_DEPS := $(test.cpp:.cpp=.d)

all: directories $(BUILD_DIR)/test_whisper_npu

directories:
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_whisper_npu: $(SOURCES) $(TEST_DEPS)
	$(CXX) $(CXX_FLAGS) -o $@ $^ $(LDFLAGS) $(DEPENDENCY_LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

test: $(BUILD_DIR)/test_whisper_npu
	cp ../../model_list.json $(BUILD_DIR)/model_list.json
	cd $(BUILD_DIR) && export LD_LIBRARY_PATH=../../../lib:$(LD_LIBRARY_PATH) && ./test_whisper_npu -m whisper-v3 -p 0 -a test1.ogg -t 1 -r 0

-include $(TEST_DEPS)
.PHONY: all clean test directories

else

# WSL build environment
# Use CMake to invoke the Visual Studio
PWSH := powershell.exe

all: directories test

directories:
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_whisper_npu.exe: $(SOURCES)
	cd $(BUILD_DIR) && $(PWSH) -Command "cmake ../../../test/whisper_npu"
	cd $(BUILD_DIR) && $(PWSH) -Command "cmake --build . --config Release --target test_whisper_npu_target"

clean:
	rm -rf $(BUILD_DIR)


test: directories $(BUILD_DIR)/test_whisper_npu.exe
	cp ../../model_list.json $(BUILD_DIR)/model_list.json
	cd $(BUILD_DIR) && ${PWSH} -Command "\$$env:PATH = '..\..\..\lib;' + \$$env:PATH; .\test_whisper_npu.exe -m whisper-v3 -p 0 -a test1.ogg -t 1 -r 1"

.PHONY: all clean test directories

endif
