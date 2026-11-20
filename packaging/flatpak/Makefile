.DEFAULT_GOAL := help

FLATPAK_DIR := $(CURDIR)
REPO_ROOT := $(FLATPAK_DIR)/../..

.PHONY: help
help: ## Show this help message
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' "$(MAKEFILE_LIST)" | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-20s\033[0m %s\n", $$1, $$2}'

# Run from this directory (or `make -C packaging/flatpak`). aetherpak resolves
# aetherpak.yaml paths against the working directory, and they are repo-relative.
.PHONY: build
build: ## Build Flatpak repository from source
	@if command -v aetherpak >/dev/null 2>&1; then \
		echo "=== Building Flatpak with AetherPak CLI ==="; \
		cd "$(REPO_ROOT)" && aetherpak build \
			--config packaging/flatpak/aetherpak.yaml \
			--repo-path packaging/flatpak/_repo \
			--state-dir packaging/flatpak/.state \
			--ccache-dir packaging/flatpak/.ccache; \
	elif command -v flatpak-builder >/dev/null 2>&1; then \
		echo "=== Running flatpak-builder build ==="; \
		cd "$(FLATPAK_DIR)" && flatpak-builder --force-clean --user --install-deps-from=flathub --repo=_repo .state/build-ai.lemonadeserver.app ai.lemonadeserver.app.yaml; \
	else \
		echo "Neither aetherpak nor flatpak-builder found."; exit 1; \
	fi

.PHONY: lint
lint: ## Lint Flatpak manifest and AppStream metadata
	@command -v flatpak-builder-lint >/dev/null 2>&1 || { echo "flatpak-builder-lint not found."; exit 1; }
	@echo "=== Linting Flatpak manifest ==="
	@cd "$(FLATPAK_DIR)" && flatpak-builder-lint manifest ai.lemonadeserver.app.yaml
	@echo "=== Linting AppStream metadata ==="
	@cd "$(FLATPAK_DIR)" && flatpak-builder-lint appstream ../../data/ai.lemonadeserver.app.metainfo.xml

.PHONY: clean
clean: ## Clean Flatpak build and stage directories
	rm -rf "$(FLATPAK_DIR)/_repo" "$(FLATPAK_DIR)/.state" "$(FLATPAK_DIR)/.ccache" "$(FLATPAK_DIR)/.flatpak-builder" "$(FLATPAK_DIR)"/dist/*.flatpak
