BUILD_DIR := build
OUT_DIR   := out
PWSH      := powershell.exe


.PHONY: all clean run serve install

ifeq ($(OS),Windows_NT)

all:
	@$(PWSH) "if (!(Test-Path '$(BUILD_DIR)')) { New-Item -ItemType Directory -Path '$(BUILD_DIR)' }"
	@$(PWSH) "if (!(Test-Path '$(OUT_DIR)'))   { New-Item -ItemType Directory -Path '$(OUT_DIR)' }"
	@$(PWSH) "cmake --preset windows-default"
	@$(PWSH) "cmake --build build --preset windows-default"

run:
	@$(PWSH) "Copy-Item 'lib\\*.dll' '$(OUT_DIR)' -Force"
	@$(PWSH) "Copy-Item 'model_list.json' '$(OUT_DIR)' -Force"
	@$(PWSH) "Copy-Item 'xclbins' '$(OUT_DIR)' -Recurse -Force"
	@$(PWSH) "cd '$(OUT_DIR)'; .\\flm.exe run llama3.2:1b -a 0"

serve:
	@$(PWSH) "Copy-Item 'lib\\*.dll' '$(OUT_DIR)' -Force"
	@$(PWSH) "Copy-Item 'model_list.json' '$(OUT_DIR)' -Force"
	@$(PWSH) "Copy-Item 'xclbins' '$(OUT_DIR)' -Recurse -Force"
	@$(PWSH) "cd '$(OUT_DIR)'; .\\flm.exe serve qwen2.5vl-it -c 32768"

clean:
	@$(PWSH) "Remove-Item -Recurse -Force '$(BUILD_DIR)'"
	@$(PWSH) "Remove-Item -Recurse -Force '$(OUT_DIR)'"

install:
	@echo "Installing using CMake..."
	@cd $(BUILD_DIR) && cmake --install . --prefix $(PREFIX)

else

all:
	@mkdir -p $(OUT_DIR)
	@cmake --preset linux-default
	@cmake --build build
	@cp build/flm $(OUT_DIR)

run:
	@cp lib/*.so $(OUT_DIR)
	@cp model_list.json $(OUT_DIR)
	@cd $(OUT_DIR) && ./flm run gpt-oss -a 0

serve:
	@cp lib/*.so $(OUT_DIR)
	@cp model_list.json $(OUT_DIR)
	@cd $(OUT_DIR) && ./flm serve -a 1 -e 1

clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf $(OUT_DIR)

install:
	@echo "Installing using CMake..."
	@cd $(BUILD_DIR) && sudo cmake --install .

endif
