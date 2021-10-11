LLVM_CONFIG:=$(shell command -v llvm-config-12 || command -v llvm-config)
LLVMFLAGS:=$(shell $(LLVM_CONFIG) --cxxflags)
LLVMLIBS:=$(shell $(LLVM_CONFIG) --libs coverage)

all: bin/llvmcov2html

bin/llvmcov2html: main.cpp
	@mkdir -p bin
	g++ -o$@ $(LLVMFLAGS) -g $^ $(LLVMLIBS)

