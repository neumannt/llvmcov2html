llvm-coverage-to-html converter
===============================

The clang compiler supports source based coverage tracking, but the default
reporting options are very basic. This tools allows for converting a llvm
coverage file into a nice HTML report.

Usage
-----

First compile your program with coverage tracking:

    clang -O0 -fprofile-instr-generate -fcoverage-mapping test/switch.cpp -o test/switch

Now execute the program and write the coverage for that run into a file (here: rc.profraw):

    LLVM_PROFILE_FILE="rc.profraw" test/switch

The coverage from one or more runs can be finalized with `llvm-profdata`:

    llvm-profdata merge -sparse rc.profraw -o rc.profdata

The finalized coverage file is now converted into an HTML report using `llvmcov2html`:

    mkdir -p tmp
    bin/llvmcov2html tmp test/switch rc.profdata

The coverage percentage is printed to stdout and the HTML files are written into
the `tmp` directory. Start with `index.html` to get an overview.

Building
--------

`llvmcov2html` requires [LLVM 12](https://llvm.org) and a C++17 compiler.
Both plain `make` and `cmake` are supported.
