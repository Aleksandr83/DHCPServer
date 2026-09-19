/**
 * @file test_restsenderlimits.cpp
 * @brief Host test for the shared limits of the five REST senders (rule 39).
 *
 * Stage 144 replaced the copies: the same 6000/500/10/1024 lived in five sender
 * workers (DHCP events, external DNS cache — two workers — DNS queries, served
 * time requests) and the 5000 ms send timeout was declared under four different
 * names. The values must **not** change in a rename, which is what this test
 * pins: if someone later "tidies" one of these numbers, the failure is here
 * rather than in the field.
 *
 * The header is deliberately free of ESP-IDF, so it compiles on the host — and
 * this test proves that too.
 *
 * Build (MinGW, from the repository root):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -I. test/test_restsenderlimits.cpp \
 *       -o test_restsenderlimits.exe
 */

#include <cstdio>

#include "../src/core/RestSenderLimits.h"

namespace limits = dhcp::core;

// The compile-time half of the promise (the runtime checks below print, these
// simply refuse to build).
static_assert(limits::kDrainDeadlineMs == 6000, "drain deadline changed");
static_assert(limits::kQueueWaitMs == 500, "queue wait changed");
static_assert(limits::kStopMarkerPollMs == 10, "stop marker poll changed");
static_assert(limits::kSendTimeoutMs == 5000, "send timeout changed");
static_assert(limits::kHttpClientBufferBytes == 1024, "http buffer changed");

#define TEST_ASSERT_TRUE(cond)                                              \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define TEST_ASSERT_EQ(a, b)                                                \
    do {                                                                    \
        if ((a) != (b)) {                                                   \
            printf("FAIL: %s:%d: %s == %s (%d != %d)\n", __FILE__, __LINE__, \
                   #a, #b, static_cast<int>(a), static_cast<int>(b));        \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static int test_values_are_the_ones_that_were_replaced()
{
    // Every number here was a literal in each of the five senders before
    // stage 144; the stage was a rename, so they are all still the same.
    TEST_ASSERT_EQ(limits::kDrainDeadlineMs, 6000);
    TEST_ASSERT_EQ(limits::kQueueWaitMs, 500);
    TEST_ASSERT_EQ(limits::kStopMarkerPollMs, 10);
    TEST_ASSERT_EQ(limits::kSendTimeoutMs, 5000);
    TEST_ASSERT_EQ(limits::kHttpClientBufferBytes, 1024);
    printf("values: drain %d ms, queue wait %d ms, stop poll %d ms, "
           "send timeout %d ms, http buffers %d B\n",
           limits::kDrainDeadlineMs, limits::kQueueWaitMs,
           limits::kStopMarkerPollMs, limits::kSendTimeoutMs,
           limits::kHttpClientBufferBytes);
    return 0;
}

static int test_stop_path_fits_inside_the_deadline()
{
    // The shutdown path polls for the stop marker and then waits for the queue;
    // both waits have to end well before the deadline that bounds the whole
    // drain, otherwise the deadline is a decoration.
    TEST_ASSERT_TRUE(limits::kStopMarkerPollMs < limits::kQueueWaitMs);
    TEST_ASSERT_TRUE(limits::kQueueWaitMs < limits::kDrainDeadlineMs);
    printf("ordering: %d < %d < %d\n", limits::kStopMarkerPollMs,
           limits::kQueueWaitMs, limits::kDrainDeadlineMs);
    return 0;
}

static int test_http_client_buffers_hold_a_log_record()
{
    // The buffers are a power of two and large enough for one record; a buffer
    // smaller than a kilobyte would make the HTTP client truncate replies.
    TEST_ASSERT_TRUE(limits::kHttpClientBufferBytes >= 1024);
    TEST_ASSERT_EQ(limits::kHttpClientBufferBytes & (limits::kHttpClientBufferBytes - 1), 0);
    return 0;
}

int main()
{
    int failed = 0;
    failed += test_values_are_the_ones_that_were_replaced();
    failed += test_stop_path_fits_inside_the_deadline();
    failed += test_http_client_buffers_hold_a_log_record();

    if (failed == 0) {
        printf("All RestSenderLimits tests PASSED!\n");
        return 0;
    }
    printf("%d RestSenderLimits test(s) FAILED\n", failed);
    return 1;
}
