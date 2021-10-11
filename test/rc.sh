#!/usr/bin/env bash
set -euo pipefail

LLVM_VERSION=11
LLVM_CONFIG=$(command -v llvm-config-$LLVM_VERSION || command -v llvm-config)
LLVM_BINDIR=$(eval $LLVM_CONFIG --bindir)
LLVM_PROFDATA=$(command -v llvm-profdata-$LLVM_VERSION || command -v $LLVM_BINDIR/llvm-profdata || command -v llvm-profdata) || { echo "need llvm-profdata"; exit 1; }
CLANG=$(command -v clang++-$LLVM_VERSION || command -v $LLVM_BINDIR/clang++ || command -v clang++) || { echo "need clang++"; exit 1; }


"$CLANG" -O0 -fprofile-instr-generate -fcoverage-mapping test/switch.cpp -o test/switch
LLVM_PROFILE_FILE="rc.profraw" test/switch
"$LLVM_PROFDATA" merge -sparse rc.profraw -o rc.profdata

mkdir -p tmp
bin/llvmcov2html tmp test/switch rc.profdata
