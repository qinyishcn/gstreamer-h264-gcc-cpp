cmake_minimum_required(VERSION 3.10)
project(gstreamer-h264-gcc VERSION 4.0.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

option(
    ENABLE_SANITIZERS
    "Enable address and undefined behavior sanitizers"
    OFF
)
option(ENABLE_TSAN "Enable thread sanitizer" OFF)

if(ENABLE_SANITIZERS AND ENABLE_TSAN)
    message(FATAL_ERROR
        "ENABLE_SANITIZERS and ENABLE_TSAN cannot be enabled together")
endif()

if(ENABLE_SANITIZERS OR ENABLE_TSAN)
    if(WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" OR
       NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "Requested sanitizers require GNU or Clang on a non-Windows host")
    endif()

    if(ENABLE_SANITIZERS)
        set(SANITIZER_COMPILE_FLAG "-fsanitize=address,undefined")
        set(SANITIZER_LINK_FLAG "-fsanitize=address,undefined")
    else()
        set(SANITIZER_COMPILE_FLAG "-fsanitize=thread")
        set(SANITIZER_LINK_FLAG "-fsanitize=thread")
    endif()
endif()

add_library(project_sanitizers INTERFACE)
if(ENABLE_SANITIZERS OR ENABLE_TSAN)
    target_compile_options(project_sanitizers INTERFACE
        ${SANITIZER_COMPILE_FLAG}
        -fno-omit-frame-pointer
    )
    target_link_libraries(project_sanitizers INTERFACE
        ${SANITIZER_LINK_FLAG}
    )
endif()
link_libraries(project_sanitizers)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

# ── Razor GCC Library (vendored) ──
file(GLOB RAZOR_CC     vendor/razor/cc/*.c)
file(GLOB RAZOR_EST    vendor/razor/estimator/*.c)
file(GLOB RAZOR_PACE   vendor/razor/pacing/*.c)
file(GLOB RAZOR_COMMON vendor/razor/common/*.c)
file(GLOB RAZOR_POSIX  vendor/razor/common/platform/posix/*.c)
file(GLOB RAZOR_BBR    vendor/razor/bbr/*.c)
file(GLOB RAZOR_REMB   vendor/razor/remb/*.c)

add_library(razor STATIC
    ${RAZOR_CC} ${RAZOR_EST} ${RAZOR_PACE}
    ${RAZOR_COMMON} ${RAZOR_POSIX}
    ${RAZOR_BBR} ${RAZOR_REMB}
)
target_include_directories(razor PUBLIC
    vendor/razor/cc vendor/razor/estimator vendor/razor/pacing
    vendor/razor/common vendor/razor/common/platform/posix
    vendor/razor/bbr vendor/razor/remb
)
target_compile_options(razor PRIVATE -Wno-unused-variable -Wno-unused-function -Wno-missing-braces -Wno-unused-but-set-variable)
target_link_libraries(razor Threads::Threads)

# ── Main project ──
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(vendor/razor/cc)
link_directories(${GST_LIBRARY_DIRS})
add_compile_options(${GST_CFLAGS_OTHER})

add_library(gcc_controller STATIC src/gcc_controller.cpp)
target_include_directories(gcc_controller PUBLIC ${GST_INCLUDE_DIRS})
target_link_libraries(gcc_controller ${GST_LIBRARIES})

add_library(gcc_support STATIC src/rtp_packet.cpp)
target_include_directories(gcc_support PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_sources(gcc_support PRIVATE
    src/feedback_packet.cpp
    src/probe_budget.cpp
    src/probe_controller.cpp
    src/probe_delivery_estimator.cpp
    src/outer_loss_backoff.cpp
    src/rate_diagnostics.cpp
    src/rate_policy.cpp
    src/transport_sender.cpp
)
target_link_libraries(gcc_support PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(gcc_support PUBLIC ws2_32)
endif()

if(BUILD_TESTING)
    add_executable(test_feedback_packet tests/test_feedback_packet.cpp)
    target_include_directories(test_feedback_packet PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(test_feedback_packet gcc_support)
    add_test(NAME feedback_packet COMMAND test_feedback_packet)

    add_executable(test_rtp_packet tests/test_rtp_packet.cpp)
    target_include_directories(test_rtp_packet PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(test_rtp_packet gcc_support)
    add_test(NAME rtp_packet COMMAND test_rtp_packet)

    add_executable(test_transport_sender tests/test_transport_sender.cpp)
    target_include_directories(test_transport_sender PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(test_transport_sender gcc_support)
    add_test(NAME transport_sender COMMAND test_transport_sender)

    add_executable(test_rate_policy tests/test_rate_policy.cpp)
    target_include_directories(test_rate_policy PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(test_rate_policy gcc_support)
    add_test(NAME rate_policy COMMAND test_rate_policy)

    add_executable(
        test_outer_loss_backoff tests/test_outer_loss_backoff.cpp
    )
    target_include_directories(
        test_outer_loss_backoff PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(test_outer_loss_backoff gcc_support)
    add_test(NAME outer_loss_backoff COMMAND test_outer_loss_backoff)

    add_executable(test_probe_controller tests/test_probe_controller.cpp)
    target_include_directories(test_probe_controller PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(test_probe_controller gcc_support)
    add_test(NAME probe_controller COMMAND test_probe_controller)

    add_executable(
        test_probe_delivery_estimator
        tests/test_probe_delivery_estimator.cpp
    )
    target_include_directories(
        test_probe_delivery_estimator PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(test_probe_delivery_estimator gcc_support)
    add_test(
        NAME probe_delivery_estimator
        COMMAND test_probe_delivery_estimator
    )

    add_executable(test_probe_budget tests/test_probe_budget.cpp)
    target_include_directories(test_probe_budget PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    add_test(NAME probe_budget COMMAND test_probe_budget)
    target_link_libraries(test_probe_budget gcc_support)

    add_executable(test_rate_diagnostics tests/test_rate_diagnostics.cpp)
    target_include_directories(test_rate_diagnostics PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    add_test(NAME rate_diagnostics COMMAND test_rate_diagnostics)
    target_link_libraries(test_rate_diagnostics gcc_support)

    add_executable(test_razor_feedback tests/test_razor_feedback.cpp)
    target_include_directories(test_razor_feedback PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(test_razor_feedback razor Threads::Threads)
    add_test(
        NAME razor_feedback
        COMMAND test_razor_feedback
                ${CMAKE_SOURCE_DIR}/vendor/razor/cc/sender_congestion_controller.c
                ${CMAKE_SOURCE_DIR}/include/razor_wrapper.h
    )

    add_executable(
        test_demo_source_contract
        tests/test_demo_source_contract.cpp
    )
    target_include_directories(
        test_demo_source_contract PRIVATE ${CMAKE_SOURCE_DIR}/tests
    )
    add_test(
        NAME demo_source_contract
        COMMAND test_demo_source_contract ${CMAKE_SOURCE_DIR}/src/demo.cpp
    )

    add_executable(test_project_contract tests/test_project_contract.cpp)
    target_include_directories(
        test_project_contract PRIVATE ${CMAKE_SOURCE_DIR}/tests
    )
    add_test(
        NAME documentation_build_contract
        COMMAND test_project_contract
                ${CMAKE_SOURCE_DIR}/CMakeLists.txt
                ${CMAKE_SOURCE_DIR}/README.md
    )

    add_executable(test_demo_runtime tests/test_demo_runtime.cpp)
    target_include_directories(
        test_demo_runtime PRIVATE ${CMAKE_SOURCE_DIR}/tests
    )
    if(WIN32)
        target_link_libraries(test_demo_runtime ws2_32)
    endif()
    add_dependencies(test_demo_runtime demo)
    add_test(
        NAME demo_runtime
        COMMAND test_demo_runtime $<TARGET_FILE:demo>
    )
    set_tests_properties(
        demo_runtime PROPERTIES TIMEOUT 20 LABELS "runtime"
    )
    if(ENABLE_TSAN)
        set_tests_properties(
            demo_runtime PROPERTIES
            DISABLED TRUE
            LABELS "runtime;tsan_external_glib"
        )
        message(STATUS
            "TSan: demo_runtime disabled (uninstrumented GLib/GStreamer)")
    endif()

    add_executable(test_cli_runtime tests/test_cli_runtime.cpp)
    target_include_directories(
        test_cli_runtime PRIVATE ${CMAKE_SOURCE_DIR}/tests
    )
    add_dependencies(test_cli_runtime sender demo)
    add_test(
        NAME cli_runtime
        COMMAND test_cli_runtime $<TARGET_FILE:sender> $<TARGET_FILE:demo>
    )
    set_tests_properties(cli_runtime PROPERTIES TIMEOUT 30 LABELS "runtime")
endif()

add_executable(sender src/sender.cpp)
target_include_directories(sender PRIVATE ${GST_INCLUDE_DIRS})
target_link_libraries(sender ${GST_LIBRARIES} razor gcc_support Threads::Threads)

add_executable(receiver src/receiver.cpp)
target_include_directories(receiver PRIVATE ${GST_INCLUDE_DIRS})
target_link_libraries(receiver ${GST_LIBRARIES} razor gcc_support Threads::Threads)

add_executable(demo src/demo.cpp)
target_include_directories(demo PRIVATE ${GST_INCLUDE_DIRS})
target_link_libraries(demo ${GST_LIBRARIES} razor gcc_support Threads::Threads)

install(TARGETS sender receiver demo RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

message(STATUS "")
message(STATUS "=== Build Configuration ===")
message(STATUS "  C++ standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  GStreamer:     ${GST_VERSION}")
message(STATUS "  ASan + UBSan:  ${ENABLE_SANITIZERS}")
message(STATUS "  TSan:          ${ENABLE_TSAN}")
message(STATUS "  Version:       4.0.0 (razor GCC + pacing)")
message(STATUS "===========================")
message(STATUS "")
