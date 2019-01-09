# SOLLVE Common Release Repository

This repository contains all projects supported by the SOLLVE project. It is automatically merged by a bot. It contains the following projects:

 * `llvm`: LLVM and its subprojects, including SOLLVE extensions
   * LLVM
   * Clang
   * Polly
   * compiler-rt
   * libcxx
   * libcxxabi
   * openmp (merged into `llvm/projects/bolt`)
  * `sollve_vv`: OpenMP 4.5 offloading verification suite (https://bitbucket.org/crpl_cisc/sollve_vv)
  * `llvm/projects/bolt`: BOLT (http://www.bolt-omp.org/)
  * `llvm/projects/bolt/external/argobots`: Argobots (http://www.argobots.org)

## Instructions

### Prerequisites

```
- REQUIRED: C compiler (gcc (>= 4.8) is sufficient)
- REQUIRED: CMake
- OPTIONAL: CUDA environment (for offloading)
- OPTIONAL: libelf (for offloading, see https://directory.fsf.org/wiki/Libelf)
- OPTIONAL: individually installed Argobots
- OPTIONAL: python (for testing)
```

### Install SOLLVE LLVM

The following instructions assume that the tar is unpacked and the cursor is at the top level directory.
```
sollve$ ls
llvm     sollve_vv    README.md
```

To install the SOLLVE LLVM to `<LLVM_INSTALL_DIR>`, run the following command:
```
cd llvm
mkdir build
cd build
cmake ../ -DCMAKE_INSTALL_PREFIX=<LLVM_INSTALL_DIR> -DLIBOMP_USE_ARGOBOTS=on [other CMake options]
make -j install
```

Useful configuration options are listed:
```
# specify the build option.
-DCMAKE_BUILD_TYPE=<DEBUG|Release|RelWithDebInfo|MinSizeRel>
# use your own Argobots
-DLIBOMP_ARGOBOTS_INSTALL_DIR=<ARGOBOTS_INSTALL_DIR>
# enable offloading with CUDA (if LIBELF is not installed to the system)
-DLIBOMPTARGET_DEP_LIBELF_INCLUDE_DIRS=<LIBELF_INSTALL_DIR>
```

If `-DLIBOMP_USE_ARGOBOTS=on` is not set (e.g., `-DLIBOMP_USE_ARGOBOTS=off`), Pthreads-based OpenMP runtime system is used.

### Run a testsuite for CPUs.

It is assumed that all the necessary paths (including `PATH`, `CPATH`, `LIBRARY_PATH`, and `LD_LIBRARY_PATH`) to `<LLVM_INSTALL_DIR>` are properly set.
To run the OpenMP testsuite for CPU, go to the build directory (e.g., `sollve/llvm/build`) and type the following commands:
```
build $ ls
benchmarks  bin   cmake   CMakeCache.txt ...
build $ python bin/llvm-lit -j<N> projects/bolt/runtime/test
```
where `N` is the number of threads to run tests in parallel.

### Run a testsuite for accelerators (sollve_vv)

It is assumed that all the necessary paths (including `PATH`, `CPATH`, `LIBRARY_PATH`, and `LD_LIBRARY_PATH`) to `<LLVM_INSTALL_DIR>` and `<LIBELF_INSTALL_DIR>` are properly set.
sollve_vv can be run as follows:
```
sollve$ ls
llvm     sollve_vv    README.md
sollve$ cd sollve_vv
sollve$ make CC=clang CXX=clang++ SYSTEM=<SYSTEM_NAME> LOG=1 SOURCES=tests/*/*.c*
# to create a test summary
sollve$ make CC=clang CXX=clang++ SYSTEM=<SYSTEM_NAME> SOURCES=tests/*/*.c* report_json
sollve$ make CC=clang CXX=clang++ SYSTEM=<SYSTEM_NAME> SOURCES=tests/*/*.c* report_html
```

By default, the following systems are support as `<SYSTEM_NAME>`:
 * summit
 * summitdev
 * titan

You can add your own configuration. The following is an example to test the offloading features with CUDA:
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
A new `<SYSTEM_NAME>` is added (say `local`) by creating the file above (`local.def`) in `sollve/sollve_vv/sys/systems`.

