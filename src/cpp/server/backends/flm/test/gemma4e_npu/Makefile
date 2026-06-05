# =============================================================================
# Qwen NPU Library Makefile
# =============================================================================
#
# Author: Zhenyu Xu (zxu3@clemson.edu)
# Created: 2024
#
# This Makefile is used to build the Qwen NPU library components.
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
SOURCES += ../../common/AutoModel/modeling_gemma4e.cpp
SOURCES += ../../common/AutoModel/modeling_gemma4e_image.cpp
SOURCES += ../../common/AutoModel/modeling_gemma4e_audio.cpp
SOURCES += ../../common/image_process_utils/imageproc.cpp
SOURCES += ../../common/image_process_utils/imageprocAVX512.cpp
SOURCES += ../../common/audio_process_utils/audioproc.cpp
SOURCES += ../../common/audio_process_utils/audioprocAVX512.cpp
SOURCES += ../../common/image/image_reader.cpp
SOURCES += ../../common/audio/audio_reader.cpp
SOURCES += ../../common/tokenizer/tokenizer.cpp
SOURCES += ../../common/modules/sampler.cpp

HEADERS += ../../include/gemma4e/gemma4e_npu_sequence.hpp
HEADERS += ../../include/gemma4e/gemma4e_npu.hpp


ifeq ($(WSL), 0)

LDFLAGS += -lgemma4e_npu
LDFLAGS += -lavformat -lavcodec -lavutil -lswscale -lswresample
LDFLAGS += -lfftw3f

TEST_DEPS := $(test.cpp:.cpp=.d)

all: directories $(BUILD_DIR)/test_gemma4e_npu

directories:
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_gemma4e_npu: $(SOURCES) $(TEST_DEPS)
	$(CXX) $(CXX_FLAGS) -o $@ $^ $(LDFLAGS) $(DEPENDENCY_LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

test: $(BUILD_DIR)/
	cp ../../model_list.json $(BUILD_DIR)/model_list.json
	cd $(BUILD_DIR) && export LD_LIBRARY_PATH=../../../lib && ./test_gemma4e_npu --model gemma4-it:e2b -s 1

-include $(TEST_DEPS)
.PHONY: all clean test directories

else

# WSL build environment
# Use CMake to invoke the Visual Studio
PWSH := powershell.exe

all: directories test

directories:
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_gemma4e_npu.exe: $(SOURCES)
	cd $(BUILD_DIR) && $(PWSH) -Command "cmake ../../../test/gemma4e_npu"
	cd $(BUILD_DIR) && $(PWSH) -Command "cmake --build . --config Release --target test_gemma4e_npu"

clean:
	rm -rf $(BUILD_DIR)


test: directories $(BUILD_DIR)/test_gemma4e_npu.exe
	cp ../../model_list.json $(BUILD_DIR)/model_list.json
	cd $(BUILD_DIR) && ${PWSH} -Command "\$$env:PATH = '..\..\..\lib;' + \$$env:PATH; .\test_gemma4e_npu.exe -m gemma4-it:e2b -s 1 -p 0"

.PHONY: all clean test directories

endif
