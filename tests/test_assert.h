/* Minimal test helpers: a failed check prints file:line and exits 1. */
#ifndef TEST_ASSERT_H
#define TEST_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                    __LINE__, #cond);                                   \
            exit(1);                                                    \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                  \
    do {                                                                \
        long long a_ = (long long)(a);                                  \
        long long b_ = (long long)(b);                                  \
        if (a_ != b_) {                                                 \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s "         \
                    "(%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,     \
                    a_, b_);                                            \
            exit(1);                                                    \
        }                                                               \
    } while (0)

#define RUN_TEST(fn)                     \
    do {                                 \
        fn();                            \
        printf("  ok  %s\n", #fn);       \
    } while (0)

#endif /* TEST_ASSERT_H */
