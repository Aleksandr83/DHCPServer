/**
 * @file test_version.cpp
 * @brief Unit tests for Version class.
 *
 * Build with: pio test -e esp32dev
 */

#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>

// Include the code under test (adjust path as needed for test build)
#include "../src/core/Version.h"

// Simple test framework macros
#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_EQ(a, b)    do { if ((a) != (b)) { printf("FAIL: %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); return 1; } } while(0)
#define TEST_ASSERT_STR_EQ(a, b) do { if (std::string(a) != std::string(b)) { printf("FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a, b); return 1; } } while(0)

extern "C" {

static int test_version_format()
{
    const auto& ver = dhcp::core::Version::instance();
    const std::string& str = ver.toString();

    printf("Version string: %s\n", str.c_str());

    // Check format: XX.XX.XXX.XX.XX.XX.RR
    TEST_ASSERT_TRUE(str.length() >= 20);
    TEST_ASSERT_EQ(str[2], '.');
    TEST_ASSERT_EQ(str[5], '.');
    TEST_ASSERT_EQ(str[9], '.');
    TEST_ASSERT_EQ(str[12], '.');
    TEST_ASSERT_EQ(str[15], '.');
    TEST_ASSERT_EQ(str[18], '.');

    return 0;
}

static int test_version_components()
{
    const auto& v = dhcp::core::Version::instance();
    TEST_ASSERT_TRUE(v.global() <= 99);
    TEST_ASSERT_TRUE(v.device() <= 99);
    TEST_ASSERT_TRUE(v.release() <= 999);
    TEST_ASSERT_TRUE(v.subRelease() <= 99);
    TEST_ASSERT_TRUE(v.year() <= 99);
    TEST_ASSERT_TRUE(v.month() >= 1 && v.month() <= 12);
    TEST_ASSERT_TRUE(v.region().length() == 2);

    printf("Components: %02u.%02u.%03u.%02u.%02u.%02u.%s\n",
           v.global(), v.device(), v.release(), v.subRelease(),
           v.year(), v.month(), v.region().c_str());

    return 0;
}

static int test_version_caching()
{
    const auto& v = dhcp::core::Version::instance();
    const std::string& s1 = v.toString();
    const std::string& s2 = v.toString();
    TEST_ASSERT_TRUE(&s1 == &s2); // same cached object
    return 0;
}

void app_main()
{
    printf("Running Version tests...\n");
    int failures = 0;

    failures += test_version_format();
    failures += test_version_components();
    failures += test_version_caching();

    if (failures == 0) {
        printf("All Version tests PASSED!\n");
    } else {
        printf("Some Version tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
