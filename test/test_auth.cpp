/**
 * @file test_auth.cpp
 * @brief Unit tests for AuthManager.
 */

#include <cstdio>
#include <string>

#include "../src/web/AuthManager.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)

extern "C" {

static int test_auth_valid_credentials()
{
    dhcp::web::AuthManager auth;

    // Default credentials: admin:admin
    // Base64 of "admin:admin" is "YWRtaW46YWRtaW4="
    std::string authHeader = "Basic YWRtaW46YWRtaW4=";

    bool result = auth.authenticate(authHeader, "192.168.1.10");
    TEST_ASSERT_TRUE(result);

    printf("Auth valid credentials test PASSED\n");
    return 0;
}

static int test_auth_invalid_credentials()
{
    dhcp::web::AuthManager auth;

    // Wrong password
    std::string authHeader = "Basic YWRtaW46d3Jvbmc="; // admin:wrong

    bool result = auth.authenticate(authHeader, "192.168.1.10");
    TEST_ASSERT_FALSE(result);

    printf("Auth invalid credentials test PASSED\n");
    return 0;
}

static int test_auth_empty_header()
{
    dhcp::web::AuthManager auth;

    bool result = auth.authenticate("", "192.168.1.10");
    TEST_ASSERT_FALSE(result);

    printf("Auth empty header test PASSED\n");
    return 0;
}

static int test_auth_lockout()
{
    dhcp::web::AuthManager auth;

    std::string authHeader = "Basic YWRtaW46d3Jvbmc="; // admin:wrong
    std::string clientIp = "10.0.0.1";

    // Fail 5 times
    for (int i = 0; i < 5; i++) {
        auth.authenticate(authHeader, clientIp);
    }

    // Should be locked out
    bool locked = auth.isLockedOut(clientIp);
    TEST_ASSERT_TRUE(locked);

    // Correct password should also be rejected
    std::string goodAuth = "Basic YWRtaW46YWRtaW4=";
    bool result = auth.authenticate(goodAuth, clientIp);
    TEST_ASSERT_FALSE(result);

    printf("Auth lockout test PASSED\n");
    return 0;
}

void app_main()
{
    printf("Running AuthManager tests...\n");
    int failures = 0;

    failures += test_auth_valid_credentials();
    failures += test_auth_invalid_credentials();
    failures += test_auth_empty_header();
    failures += test_auth_lockout();

    if (failures == 0) {
        printf("All AuthManager tests PASSED!\n");
    } else {
        printf("Some AuthManager tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"
