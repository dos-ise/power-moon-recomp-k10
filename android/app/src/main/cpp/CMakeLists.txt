cmake_minimum_required(VERSION 3.22.1)
project(nativegen_jni)

# libnativegen from the repo's tools/nativegen (the C API in ngen.h; the CLI and tests are left out)
set(NATIVEGEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tools/nativegen)
set(NATIVEGEN_SRC
    ${NATIVEGEN_DIR}/nso.cpp
    ${NATIVEGEN_DIR}/ehframe.cpp
    ${NATIVEGEN_DIR}/lifter.cpp
    ${NATIVEGEN_DIR}/interp.cpp
    ${NATIVEGEN_DIR}/simd_c.cpp
    ${NATIVEGEN_DIR}/patches.cpp
    ${NATIVEGEN_DIR}/gen.cpp
    ${NATIVEGEN_DIR}/ngen.cpp
)
add_library(nativegen STATIC ${NATIVEGEN_SRC})
target_include_directories(nativegen PUBLIC ${NATIVEGEN_DIR})
target_compile_features(nativegen PUBLIC cxx_std_17)

add_library(nativegen_jni SHARED nativegen_jni.cpp)
target_link_libraries(nativegen_jni nativegen)
target_compile_features(nativegen_jni PUBLIC cxx_std_17)
