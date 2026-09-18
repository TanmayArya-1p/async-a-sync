# Makefile for async-a-sync video demos and analysis

SHELL := /usr/bin/env bash
REPO_DIR := $(shell pwd)
BUILD_DIR := $(REPO_DIR)/build/demos
OUT_DIR := $(REPO_DIR)/build/tests

FILC_ROOT ?= $(REPO_DIR)/vendor/filc-0.685-linux-x86_64
FILCC ?= $(FILC_ROOT)/build/bin/filcc
PATCHED_CC ?= $(REPO_DIR)/vendor/fil-c-src/build/bin/filcc

.PHONY: all help runtime demo-wordcount demo-plain demo-async demo-provenance all-demos disasm cfg clean

all: help

help:
	@echo " make demo-wordcount   : Run wordcount (GCC vs Fil-C Sync vs Implicit)"
	@echo " make demo-plain       : Run ergonomics showcase (demo_plain_io)"
	@echo " make demo-async       : Run kernel submission & DAG showcase (demo_async_io)"
	@echo " make demo-provenance  : Run token synchronization demo (demo_provenance)"
	@echo " make all-demos        : Run all 4 demos in sequence"
	@echo "------------------------------------------------------------------"
	@echo " make disasm           : Inspect disassembly (GCC raw load vs Fil-C hook)"
	@echo " make cfg              : Generate CFG graph (PNG image & AST dump)"
	@echo " make runtime          : Build the io_uring runtime extension"
	@echo " make clean            : Clean build artifacts"

runtime:
	@./runtime/build.sh

demo-wordcount: runtime
	@./demos/run_wordcount_3way.sh

demo-plain: runtime
	@mkdir -p $(OUT_DIR)
	@$(PATCHED_CC) -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
		-I$(REPO_DIR)/runtime/src -I$(REPO_DIR)/demos -L$(REPO_DIR)/runtime/build/lib \
		-o $(OUT_DIR)/demo_plain_io $(REPO_DIR)/demos/demo_plain_io.c
	@$(OUT_DIR)/demo_plain_io $(OUT_DIR)

demo-async: runtime
	@mkdir -p $(OUT_DIR)
	@$(PATCHED_CC) -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
		-I$(REPO_DIR)/runtime/src -I$(REPO_DIR)/demos -L$(REPO_DIR)/runtime/build/lib \
		-o $(OUT_DIR)/demo_async_io $(REPO_DIR)/demos/demo_async_io.c
	@$(OUT_DIR)/demo_async_io $(OUT_DIR)

demo-provenance: runtime
	@mkdir -p $(OUT_DIR)
	@$(PATCHED_CC) -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
		-I$(REPO_DIR)/runtime/src -I$(REPO_DIR)/demos -L$(REPO_DIR)/runtime/build/lib \
		-o $(OUT_DIR)/demo_provenance $(REPO_DIR)/demos/demo_provenance.c
	@$(OUT_DIR)/demo_provenance $(OUT_DIR)

all-demos: demo-wordcount demo-plain demo-async demo-provenance

disasm:
	@./demos/inspect_disasm_cfg.sh

cfg:
	@mkdir -p $(BUILD_DIR)
	@echo "1. Generating GCC tree CFG (.dot and .png)..."
	@cd $(BUILD_DIR) && gcc -O2 -I$(REPO_DIR)/demos -fdump-tree-cfg-graph $(REPO_DIR)/demos/demo_wordcount.c -o $(BUILD_DIR)/wc_gcc_cfg_bin
	@DOT_FILE=$$(find $(BUILD_DIR) -name "*demo_wordcount*.dot" | head -n 1); \
	if [ -n "$$DOT_FILE" ] && command -v dot >/dev/null 2>&1; then \
		dot -Tpng "$$DOT_FILE" -o $(BUILD_DIR)/cfg_gcc_wordcount.png; \
		echo "   -> GCC CFG image: $(BUILD_DIR)/cfg_gcc_wordcount.png"; \
	fi
	@echo "2. Generating Fil-C post-instrumentation CFG (.dot and .png)..."
	@$(PATCHED_CC) -O2 -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
		-I$(REPO_DIR)/runtime/src -I$(REPO_DIR)/demos \
		-emit-llvm -S $(REPO_DIR)/demos/demo_wordcount.c -o $(BUILD_DIR)/wc_implicit.ll
	@$(REPO_DIR)/vendor/fil-c-src/build/bin/opt -passes=dot-cfg -disable-output $(BUILD_DIR)/wc_implicit.ll >/dev/null 2>&1
	@if [ -f ".pizlonatedFIP1066_wordcount.dot" ] && command -v dot >/dev/null 2>&1; then \
		dot -Tpng .pizlonatedFIP1066_wordcount.dot -o $(BUILD_DIR)/cfg_filc_wordcount.png; \
		mv .*.dot $(BUILD_DIR)/ 2>/dev/null || true; \
		echo "   -> Fil-C CFG image: $(BUILD_DIR)/cfg_filc_wordcount.png"; \
	fi
	@echo "All CFGs generated successfully in $(BUILD_DIR)"

clean:
	rm -rf $(REPO_DIR)/build/demos $(REPO_DIR)/build/tests
