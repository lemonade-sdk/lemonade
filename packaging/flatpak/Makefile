.DEFAULT_GOAL := help

.PHONY: help
help: ## Show this help message
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-20s\033[0m %s\n", $$1, $$2}'

.PHONY: build
build: ## Stage precompiled binaries and build Flatpak repository
	./flatpak-build.sh

.PHONY: lint
lint: ## Lint Flatpak manifest and AppStream metadata
	@if command -v aetherpak >/dev/null 2>&1; then \
		echo "=== Linting with AetherPak CLI ==="; \
		aetherpak lint --config aetherpak.yaml; \
	elif command -v flatpak-builder-lint >/dev/null 2>&1; then \
		echo "=== Linting with flatpak-builder-lint ==="; \
		flatpak-builder-lint manifest ai.lemonadeserver.app.yaml; \
		flatpak-builder-lint appstream ../../data/ai.lemonadeserver.app.metainfo.xml; \
	else \
		echo "Neither aetherpak nor flatpak-builder-lint found."; \
	fi

.PHONY: clean
clean: ## Clean Flatpak build and stage directories
	rm -rf ../../_flatpak_stage ../../_flatpak_build ../../dist/flatpak-repo ../../dist/*.flatpak
