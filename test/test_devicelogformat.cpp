/**
 * @file test_devicelogformat.cpp
 * @brief Unit tests for the sentences of `Device.log`.
 *
 * The device journal answers one question an operator asks after every surprise:
 * "what did this box do, and why did it restart?" The sentences are therefore a
 * promise — the version a run started with, the reason the previous run ended,
 * who asked for a restart — and they are built without a timestamp
 * (`ErrorLogCore` adds that), which is what lets a host test check them.
 *
 * There is nothing to compare against in the past: before this change the header
 * did not exist, so this test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_devicelogformat.cpp host_main.cpp -o test_devicelogformat
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <string>

#include "../src/core/DeviceLogFormat.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)

using namespace dhcp::core;

namespace {

/** A plain power-on says only that the run started, and with which version. */
int test_the_plain_boot()
{
    TEST_ASSERT_STREQ(deviceBootText("01.03.058.00.26.10.RU"),
                      "boot: DHCPServer 01.03.058.00.26.10.RU started");
    return 0;
}

/** A restart that was not a power-on names the reason in the same line. */
int test_the_boot_after_a_restart()
{
    TEST_ASSERT_STREQ(deviceBootAfterText("01.03.058.00.26.10.RU",
                                          "a panic (the firmware aborted)"),
                      "boot: DHCPServer 01.03.058.00.26.10.RU started; the previous "
                      "run ended with a panic (the firmware aborted)");
    TEST_ASSERT_STREQ(deviceBootAfterText("01.02.056.00.26.09.RU", "a software restart"),
                      "boot: DHCPServer 01.02.056.00.26.09.RU started; the previous "
                      "run ended with a software restart");
    return 0;
}

/** A restart somebody asked for names who asked. */
int test_the_requested_restart()
{
    TEST_ASSERT_STREQ(deviceRestartText("the web interface"),
                      "restart requested from the web interface");
    TEST_ASSERT_STREQ(deviceRestartText("the console"),
                      "restart requested from the console");
    TEST_ASSERT_STREQ(deviceRestartText("an SSH session"),
                      "restart requested from an SSH session");
    return 0;
}

/** A factory reset says what it erased, because nothing else will be left. */
int test_the_factory_reset()
{
    TEST_ASSERT_STREQ(deviceFactoryResetText("the web interface"),
                      "factory reset requested from the web interface: every setting "
                      "is erased and the device restarts");
    TEST_ASSERT_STREQ(deviceFactoryResetText("the console"),
                      "factory reset requested from the console: every setting is "
                      "erased and the device restarts");
    return 0;
}

/** An imported backup is a device event: the settings on the box changed. */
int test_the_settings_import()
{
    TEST_ASSERT_STREQ(deviceSettingsImportText(),
                      "settings imported from a backup file");
    return 0;
}

/** A crash says where it happened, not only that it happened. */
int test_the_crash_location()
{
    TEST_ASSERT_STREQ(deviceSshCrashText("the key exchange"),
                      "the previous run ended inside the SSH server: the key exchange");
    TEST_ASSERT_STREQ(deviceSshCrashText("waiting inside accept"),
                      "the previous run ended inside the SSH server: waiting inside accept");
    return 0;
}

/** libssh's own lines keep their function name and lose the trailing newline. */
int test_the_libssh_lines()
{
    TEST_ASSERT_STREQ(deviceLibsshText("ssh_kex2", "SSH_MSG_KEXINIT sent\n"),
                      "libssh ssh_kex2: SSH_MSG_KEXINIT sent");
    TEST_ASSERT_STREQ(deviceLibsshText("", "boom"),
                      "libssh ?: boom");
    TEST_ASSERT_STREQ(deviceLibsshText("ssh_packet", "a\r\n"), "libssh ssh_packet: a");
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running DeviceLogFormat tests...\n");
    int failures = 0;

    failures += test_the_plain_boot();
    failures += test_the_boot_after_a_restart();
    failures += test_the_requested_restart();
    failures += test_the_factory_reset();
    failures += test_the_settings_import();
    failures += test_the_crash_location();
    failures += test_the_libssh_lines();

    if (failures == 0) {
        printf("All DeviceLogFormat tests PASSED!\n");
    } else {
        printf("Some DeviceLogFormat tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
