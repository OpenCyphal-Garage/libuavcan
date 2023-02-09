#
# Copyright 2023 Amazon.com, Inc. or its affiliates. All Rights Reserved.
#
# C, CXX, LD, and AS flags for native targets.
#

#
# Debug flags for C and C++
#
set(C_FLAG_SET )

#
# Diagnostics for C and C++
#
list(APPEND C_FLAG_SET
                "-pedantic"
                "-Wall"
                "-Wextra"
                "-Werror"
                "-Wfloat-equal"
                "-Wconversion"
                "-Wabi"
                "-Wunused-parameter"
                "-Wunused-variable"
                "-Wunused-value"
                "-Wcast-align"
                "-Wmissing-declarations"
                "-Wmissing-field-initializers"
                "-Wdouble-promotion"
                "-Wswitch-enum"
                "-Wtype-limits"
                "-Wno-error=array-bounds"
)

if (LIBCYPHAL_ENABLE_COVERAGE)
message(STATUS "Coverage is enabled. Instrumenting the code.")
list(APPEND C_FLAG_SET
                "-fprofile-arcs"
                "-ftest-coverage"
                "--coverage"
)
endif()

set(CXX_FLAG_SET ${C_FLAG_SET})
set(ASM_FLAG_SET ${C_FLAG_SET})

#
# General C++ only flags
#
list(APPEND CXX_FLAG_SET
                "-std=c++14"
)

#
# C++ only diagnostics
#
list(APPEND CXX_FLAG_SET
                "-Wsign-conversion"
                "-Wsign-promo"
                "-Wold-style-cast"
                "-Wzero-as-null-pointer-constant"
                "-Wnon-virtual-dtor"
                "-Woverloaded-virtual"
)

set(EXE_LINKER_FLAG_SET )
set(DEFINITIONS_SET )
