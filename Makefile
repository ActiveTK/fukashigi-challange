NVCC ?= nvcc
CXX ?= g++
COMMIT := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)$(shell git diff --quiet HEAD 2>/dev/null || echo -dirty)
ARCH ?= -gencode arch=compute_80,code=sm_80 -gencode arch=compute_86,code=sm_86 -gencode arch=compute_90,code=sm_90 -gencode arch=compute_90,code=compute_90

all: build/fukashigi-cpu

gpu: build/fukashigi-gpu

build/fukashigi-cpu: src/cpu.cpp src/core.hpp src/host.hpp
	mkdir -p build
	$(CXX) -O3 -march=native -fopenmp -std=c++17 -Wall -Wextra -Wno-unknown-pragmas -DFK_COMMIT=\"$(COMMIT)\" -o $@ src/cpu.cpp

build/fukashigi-gpu: src/gpu.cu src/core.hpp src/host.hpp
	mkdir -p build
	$(NVCC) -O3 -std=c++17 -DFK_COMMIT=\"$(COMMIT)\" $(ARCH) -Xptxas -v -o $@ src/gpu.cu

test-cpu: build/fukashigi-cpu
	tests/check.sh build/fukashigi-cpu 1 14 -l 8 -b 16
	tests/check.sh build/fukashigi-cpu 1 13 -l 8 -b 32

test-gpu: build/fukashigi-gpu
	tests/check.sh build/fukashigi-gpu 1 19 -l 8 -b 16
	tests/check.sh build/fukashigi-gpu 1 18 -l 8 -b 32

clean:
	rm -rf build

.PHONY: all gpu test-cpu test-gpu clean
