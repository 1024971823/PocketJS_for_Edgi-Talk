.DEFAULT_GOAL := help

PROJECT_DIR := projects/Edgi_Talk_M55_PocketJS
PYTHON ?= python3
ARGS ?=

.PHONY: help check source-check test build build-no-ui flash preview clean release-check

help:
	@printf '%s\n' \
	  'PocketJS for Edgi-Talk engineering entrypoints:' \
	  '' \
	  '  make check          run source-safe CI checks (uses full BSP when available)' \
	  '  make source-check   run checks without external PocketJS/BSP inputs' \
	  '  make test           run Python companion tests and host synth test' \
	  '  make build ARGS=... build the firmware (for example ARGS=-j8)' \
	  '  make build-no-ui ARGS=... build firmware while reusing the existing .pocket' \
	  '  make flash ARGS=m55 flash through the configured Edgi-Talk tool' \
	  '  make preview ARGS=perfect  run the desktop PocketJS preview' \
	  '  make clean          remove local build and Python cache output' \
	  '  make release-check  validate release metadata after the normal checks'

check:
	@$(PROJECT_DIR)/tools/ci.sh

source-check:
	@$(PROJECT_DIR)/tools/ci.sh --source-only

test:
	@$(PROJECT_DIR)/tools/ci.sh --test-only

build:
	@$(PROJECT_DIR)/tools/build.sh $(ARGS)

build-no-ui:
	@$(PROJECT_DIR)/tools/build.sh --no-ui $(ARGS)

flash:
	@$(PROJECT_DIR)/tools/flash.sh $(ARGS)

preview:
	@$(PROJECT_DIR)/tools/preview/build.sh $(ARGS)

clean:
	@rm -rf $(PROJECT_DIR)/build $(PROJECT_DIR)/Debug
	@find . -type d -name __pycache__ -prune -exec rm -rf {} +

release-check: check
	@$(PYTHON) $(PROJECT_DIR)/tools/release_check.py
