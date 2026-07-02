#pragma once

#include <cstdlib>
#include <iostream>

#define CHECK_TRUE(expression)                                                \
    do {                                                                      \
        if (!(expression)) {                                                  \
            std::cerr << __FILE__ << ':' << __LINE__                          \
                      << ": CHECK_TRUE(" #expression ") failed\n";             \
            return EXIT_FAILURE;                                              \
        }                                                                     \
    } while (false)

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        const auto check_actual = (actual);                                   \
        const auto check_expected = (expected);                               \
        if (!(check_actual == check_expected)) {                              \
            std::cerr << __FILE__ << ':' << __LINE__                          \
                      << ": CHECK_EQ(" #actual ", " #expected ") failed\n";    \
            return EXIT_FAILURE;                                              \
        }                                                                     \
    } while (false)
