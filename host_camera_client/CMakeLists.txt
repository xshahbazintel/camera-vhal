set(TARGET_OS "linux")
set(CMAKE_TOOLCHAIN_FILE "clang.toolchain.cmake")

cmake_minimum_required( VERSION 3.7 )
project(HCC)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g -ggdb")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
set(EXECUTABLE_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/bin/${TARGET_OS}")
set(LIBRARY_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/${TARGET_OS}")

find_library(PTHREAD_LIBRARY pthread "pthread library")

aux_source_directory(src host_camera_client_src)

add_executable(host_camera_client
    ${host_camera_client_src}
)
target_link_libraries(host_camera_client
    pthread
)