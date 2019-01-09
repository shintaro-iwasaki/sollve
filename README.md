# SOLLVE Common Release Repository

This repository contains all projects supported by the SOLLVE project. It is automatically merged by a bot. It contains the following projects:

 * `llvm`: LLVM and its subprojects, including SOLLVE extensions
   * LLVM
   * Clang
   * Polly
   * compiler-rt
   * libcxx
   * libcxxabi
   * openmp
  * `sollve_vv`: OpenMP 4.5 offloading verification suite (https://bitbucket.org/crpl_cisc/sollve_vv)
  * `llvm/projects/openmp`: BOLT (http://www.bolt-omp.org/)
  * `llvm/projects/openmp/external/argobots`: Argobots (http://www.argobots.org)

## Installation

### Prerequisites

```
- REQUIRED: C/C++ compilers (gcc/g++ are sufficient)
- REQUIRED: CMake
- OPTIONAL: CUDA environment (to test the offloading features for NVIDIA GPUs)
- OPTIONAL: libelf (for offloading, see https://directory.fsf.org/wiki/Libelf, installed in `<LIBELF_INSTALL_DIR>`)
- OPTIONAL: your own Argobots, installed in `<ARGOBOTS_INSTALL_DIR>`
- OPTIONAL: python (for testing)
```

### Install SOLLVE LLVM

First, go to the top level directory. You can find three files as follows:
```
sollve$ ls
llvm     sollve_vv    README.md
```

To build and install the SOLLVE LLVM to `<LLVM_INSTALL_DIR>`, run the following commands:
```
cd llvm
mkdir build
cd build
cmake ../ -DCMAKE_INSTALL_PREFIX=<LLVM_INSTALL_DIR> -DLIBOMP_USE_ARGOBOTS=on [other CMake options]
make -j install
```

Useful CMake configuration options are as follows:
```
# specify the build option.
-DCMAKE_BUILD_TYPE=<DEBUG|Release|RelWithDebInfo|MinSizeRel>
# use your own Argobots
-DLIBOMP_ARGOBOTS_INSTALL_DIR=<ARGOBOTS_INSTALL_DIR>
# enable offloading with CUDA (if libelf is not installed in your system)
-DLIBOMPTARGET_DEP_LIBELF_INCLUDE_DIRS=<LIBELF_INSTALL_DIR>
```

If `-DLIBOMP_USE_ARGOBOTS=on` is not set (e.g., `-DLIBOMP_USE_ARGOBOTS=off`), the Pthreads-based OpenMP runtime system is installed.

## Running Tests

### Run the LLVM OpenMP testsuite (mainly for CPUs)

It is assumed that SOLLVE LLVM is properly installed to your system (i.e., all the environmental variables, including `PATH`, `CPATH`, `LIBRARY_PATH`, and `LD_LIBRARY_PATH`, are properly set to `<LLVM_INSTALL_DIR>/bin` etc).
To run the LLVM OpenMP testsuite, go to the build directory (e.g., `sollve/llvm/build`) and type as follows:
```
build$ ls
benchmarks  bin   cmake   CMakeCache.txt ...
build$ python bin/llvm-lit -j<N> projects/bolt/runtime/test
```
where `N` is the number of threads to run tests in parallel.
All tests are expected to be passed (not failed.)

### Run the SOLLVE OpenMP 4.5 V&V testsuite (primarily for the offloading features)

It is assumed that all the necessary paths (e.g., `PATH`, `CPATH`, `LIBRARY_PATH`, and `LD_LIBRARY_PATH`) to `<LLVM_INSTALL_DIR>` and `<LIBELF_INSTALL_DIR>` are properly set.
The SOLLVE OpenMP 4.5 V&V testsuite can be run as follows:
```
sollve$ ls
llvm     sollve_vv    README.md
sollve$ cd sollve_vv
sollve$ make CC=clang CXX=clang++ SYSTEM=<SYSTEM_NAME> LOG=1 SOURCES=tests/*/*.c*
# to create a test summary
sollve$ make CC=clang CXX=clang++ SYSTEM=<SYSTEM_NAME> SOURCES=tests/*/*.c* report_json
sollve$ make CC=clang CXX=clang++ SYSTEM=<SYSTEM_NAME> SOURCES=tests/*/*.c* report_html
```
Note that the current SOLLVE LLVM might not pass all of the tests.

By default, the following systems are support as `<SYSTEM_NAME>`:
 * summit
 * summitdev
 * titan

You can add your own configuration. The following is an example to test the offloading features with CUDA 9.1.85:
```
# Definitions for the local system

$(info "Including local.def file")

JSRUN_COMMAND =
BATCH_SCHEDULER =

# Your CUDA version must be properly set.
# The following is an example to use CUDA 9.1.85.
CUDA_MODULE ?= cuda/9.1.85

CCOMPILERS="clang"
ifeq ($(CC), clang)
  C_COMPILER_MODULE = llvm
  C_VERSION = clang -dumpversion
endif
CXXCOMPILERS="clang++"
ifeq ($(CXX), clang++)
  CXX_COMPILER_MODULE = llvm
  CXX_VERSION = clang++ -dumpversion
endif
```
You can add a new `<SYSTEM_NAME>` (say `local`) by creating the file (`local.def`) in `sollve/sollve_vv/sys/systems`.

