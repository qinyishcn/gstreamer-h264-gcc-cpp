cmake_minimum_required(VERSION 3.10)
project(gstreamer-h264-gcc VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Find GStreamer
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
)

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${GST_INCLUDE_DIRS}
)

link_directories(${GST_LIBRARY_DIRS})
add_compile_options(${GST_CFLAGS_OTHER})

# ── GCC Controller Library ──
add_library(gcc_controller STATIC
    src/gcc_controller.cpp
)
target_link_libraries(gcc_controller ${GST_LIBRARIES})

# ── Sender ──
add_executable(sender src/sender.cpp)
target_link_libraries(sender ${GST_LIBRARIES} gcc_controller)

# ── Receiver ──
add_executable(receiver src/receiver.cpp)
target_link_libraries(receiver ${GST_LIBRARIES} gcc_controller)

# ── Demo ──
add_executable(demo src/demo.cpp)
target_link_libraries(demo ${GST_LIBRARIES} gcc_controller)

# ── Install ──
install(TARGETS sender receiver demo
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)

# ── Summary ──
message(STATUS "")
message(STATUS "=== Build Configuration ===")
message(STATUS "  C++ standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  GStreamer:     ${GST_VERSION}")
message(STATUS "  Install to:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "===========================")
message(STATUS "")
