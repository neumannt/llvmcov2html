LLVM_CONFIG:=$(shell command -v llvm-config-18 || command -v llvm-config)
CXXFLAGS:=$(shell $(LLVM_CONFIG) --cxxflags) -std=c++20 -O3 -fno-exceptions -fno-rtti
LLVMLIBS:=$(shell $(LLVM_CONFIG) --libs coverage)

all: bin/llvmcov2html

bin/llvmcov2html: main.cpp
	@mkdir -p bin
	g++ -o$@ $(CXXFLAGS) -g $^ $(LLVMLIBS)

