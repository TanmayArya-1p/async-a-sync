# Makefile for async-a-sync video demos and analysis

SHELL := /usr/bin/env bash
REPO_DIR := $(shell pwd)
BUILD_DIR := $(REPO_DIR)/build/demos
OUT_DIR := $(REPO_DIR)/build/tests

FILC_ROOT ?= $(REPO_DIR)/vendor/filc-0.685-linux-x86_64
FILCC ?= $(FILC_ROOT)/build/bin/filcc
PATCHED_CC ?= $(REPO_DIR)/vendor/fil-c-src/build/bin/filcc

.PHONY: all help runtime demo-wordcount demo-plain demo-async demo-provenance all-demos demo-pragma demo-rpc demo-rpc-counter demo-rpc-upload disasm cfg clean

all: help

help:
	@echo " make demo-wordcount   : Run wordcount (GCC vs Fil-C Sync vs Implicit)"
	@echo " make demo-plain       : Run ergonomics showcase (demo_plain_io)"
	@echo " make demo-async       : Run kernel submission & DAG showcase (demo_async_io)"
	@echo " make demo-provenance  : Run token synchronization demo (demo_provenance)"
	@echo " make all-demos        : Run all 4 demos in sequence"
	@echo "------------------------------------------------------------------"
	@echo " Pragma directive demos (patched compiler; ARGS=... passes arguments):"
	@echo " make demo-pragma            : Run all the pragma demos in order"
	@echo " make demo-pragma-hello      : One annotated call, step by step"
	@echo " make demo-pragma-lifecycle  : open, write, fsync, read, close as annotated calls"
	@echo " make demo-pragma-ordering   : r_dep/w_dep ordering vs independent calls batching"
	@echo " make demo-pragma-coldread   : Cold-cache reads: blocking vs annotated vs hand-written"
	@echo " make demo-pragma-scaling    : The same comparison for 1 to 2048 files"
	@echo " make demo-pragma-overlap    : Read + hash: blocking vs annotated, overlapped"
	@echo " make demo-rpc               : Run both rpc demos"
	@echo " make demo-rpc-counter       : Annotated calls on a custom runtime: TCP requests to a counter server"
	@echo " make demo-rpc-upload        : Two runtimes: read files with io_uring, upload them with rpc"
	@echo "------------------------------------------------------------------"
	@echo " make disasm           : Inspect disassembly (GCC raw load vs Fil-C hook)"
	@echo " make cfg              : Generate CFG graph (PNG image & AST dump)"
	@echo " make runtime          : Build the io_uring runtime extension"
	@echo " make clean            : Clean build artifacts"

runtime:
	@./runtime/build.sh

demo-wordcount: runtime
	@./demos/wordcount/run_wordcount.sh

demo-plain: runtime
	@mkdir -p $(OUT_DIR)
	@$(PATCHED_CC) -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
		-I$(REPO_DIR)/runtime/include -L$(REPO_DIR)/runtime/build/lib \
		-o $(OUT_DIR)/demo_plain_io $(REPO_DIR)/demos/explicit/demo_plain_io.c -lfilc_async_uring -lpizlo -lc
	@$(OUT_DIR)/demo_plain_io $(OUT_DIR)

demo-async: runtime
	@mkdir -p $(OUT_DIR)
	@$(PATCHED_CC) -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
		-I$(REPO_DIR)/runtime/include -L$(REPO_DIR)/runtime/build/lib \
		-o $(OUT_DIR)/demo_async_io $(REPO_DIR)/demos/explicit/demo_async_io.c -lfilc_async_uring -lpizlo -lc
	@$(OUT_DIR)/demo_async_io $(OUT_DIR)

demo-provenance: runtime
	@mkdir -p $(OUT_DIR)
	@$(PATCHED_CC) -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
		-I$(REPO_DIR)/runtime/include -L$(REPO_DIR)/runtime/build/lib \
		-o $(OUT_DIR)/demo_provenance $(REPO_DIR)/demos/explicit/demo_provenance.c -lfilc_async_uring -lpizlo -lc
	@$(OUT_DIR)/demo_provenance $(OUT_DIR)

all-demos: demo-wordcount demo-plain demo-async demo-provenance

PRAGMA_FLAGS := -O2 -static -Werror=pragma-clang-attribute -DFASYNC_IMPLICIT \
	-DFASYNC_COMPILER_INSERTS_CHECKS -I$(REPO_DIR)/runtime/include \
	-L$(REPO_DIR)/runtime/build/lib

PRAGMA_DEMOS := hello lifecycle ordering coldread scaling overlap

demo-pragma: $(addprefix demo-pragma-,$(PRAGMA_DEMOS))

# demo-pragma-<name> builds demos/pragma/demo_pragma_<name>.c and runs it on OUT_DIR.
# A pattern rule, so it is not listed in .PHONY (make skips those for them).
demo-pragma-%: runtime
	@mkdir -p $(OUT_DIR)
	@$(PATCHED_CC) $(PRAGMA_FLAGS) -o $(OUT_DIR)/demo_pragma_$* \
		$(REPO_DIR)/demos/pragma/demo_pragma_$*.c -lfilc_async_uring -lpizlo -lc
	@$(OUT_DIR)/demo_pragma_$* $(OUT_DIR) $(ARGS)

demo-rpc: demo-rpc-counter demo-rpc-upload

demo-rpc-counter demo-rpc-upload: demo-rpc-%: runtime
	@PATCHED_CC=$(PATCHED_CC) ./demos/rpc/run_rpc_demo.sh $* $(OUT_DIR)

disasm:
	@PATCHED_CC=$(PATCHED_CC) ./demos/wordcount/inspect_disasm.sh

cfg:
	@PATCHED_CC=$(PATCHED_CC) ./demos/wordcount/inspect_cfg.sh

clean:
	rm -rf $(REPO_DIR)/build/demos $(REPO_DIR)/build/tests
