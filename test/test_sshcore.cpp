/**
 * @file test_sshcore.cpp
 * @brief Unit tests for the rules the SSH server is built on (stage 222).
 *
 * Three decisions are pure and therefore checkable on a host: which port may be
 * listened on, whether a login matches the stored credentials, and what a typed
 * console line means. The sockets, the key exchange and the shell live in
 * `net/SshServer.cpp` and need libssh, but a wrong port bound, an off-by-one in
 * the comparison or a command that silently runs something else are exactly the
 * defects this stage must not have.
 *
 * There is nothing to compare against in the past: before this stage these
 * headers did not exist, so this test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_sshcore.cpp host_main.cpp -o test_sshcore
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <string>

#include "../src/core/SshSettings.h"
#include "../src/core/Credentials.h"
#include "../src/menu/ConsoleCommand.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_FALSE(cond) do { if ((cond)) { printf("FAIL: %s:%d: !%s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)

using namespace dhcp::core;
using namespace dhcp::menu;

namespace {

/** Port 22 is the default; the whole 1..65535 range is a port, 0 and 65536 are not. */
int test_the_port_rule()
{
    TEST_ASSERT_TRUE(kSshDefaultPort == 22);
    TEST_ASSERT_TRUE(sshPortValid(kSshMinPort));
    TEST_ASSERT_TRUE(sshPortValid(kSshMaxPort));
    TEST_ASSERT_TRUE(sshPortValid(22));
    TEST_ASSERT_TRUE(sshPortValid(2222));
    TEST_ASSERT_FALSE(sshPortValid(0));
    TEST_ASSERT_FALSE(sshPortValid(-1));
    TEST_ASSERT_FALSE(sshPortValid(65536));
    TEST_ASSERT_TRUE(sshPortOrDefault(0) == kSshDefaultPort);
    TEST_ASSERT_TRUE(sshPortOrDefault(70000) == kSshDefaultPort);
    TEST_ASSERT_TRUE(sshPortOrDefault(2222) == 2222);
    return 0;
}

/** A login is the stored pair, both fields; a prefix or a case change is not. */
int test_the_credentials()
{
    TEST_ASSERT_TRUE(credentialsMatch("admin", "admin", "admin", "admin"));
    TEST_ASSERT_FALSE(credentialsMatch("admin", "admin ", "admin", "admin"));
    TEST_ASSERT_FALSE(credentialsMatch("Admin", "admin", "admin", "admin"));
    TEST_ASSERT_FALSE(credentialsMatch("", "", "admin", "admin"));
    TEST_ASSERT_FALSE(credentialsMatch("admin", "", "admin", "admin"));
    TEST_ASSERT_FALSE(credentialsMatch("", "admin", "admin", "admin"));
    TEST_ASSERT_FALSE(credentialsMatch("admin", "admin", "", ""));
    // A longer password whose start is the stored one must not pass.
    TEST_ASSERT_FALSE(credentialsMatch("admin", "admin123", "admin", "admin"));
    return 0;
}

/** The console commands the SSH shell shares with the serial menu. */
int test_the_command_parser()
{
    TEST_ASSERT_TRUE(parseConsoleCommand("").action == ConsoleAction::None);
    TEST_ASSERT_TRUE(parseConsoleCommand("   ").action == ConsoleAction::None);
    TEST_ASSERT_TRUE(parseConsoleCommand("help").action == ConsoleAction::Help);
    TEST_ASSERT_TRUE(parseConsoleCommand("  help  ").action == ConsoleAction::Help);
    TEST_ASSERT_TRUE(parseConsoleCommand("version").action == ConsoleAction::Version);
    TEST_ASSERT_TRUE(parseConsoleCommand("ifconfig").action == ConsoleAction::Ifconfig);
    TEST_ASSERT_TRUE(parseConsoleCommand("lan status").action == ConsoleAction::LanStatus);
    TEST_ASSERT_TRUE(parseConsoleCommand("lan").action == ConsoleAction::UnknownSub);
    TEST_ASSERT_TRUE(parseConsoleCommand("lan up").action == ConsoleAction::UnknownSub);
    TEST_ASSERT_TRUE(parseConsoleCommand("passwd reset").action == ConsoleAction::PasswdReset);
    TEST_ASSERT_TRUE(parseConsoleCommand("settings reset").action == ConsoleAction::SettingsReset);
    TEST_ASSERT_TRUE(parseConsoleCommand("reboot").action == ConsoleAction::Reboot);
    TEST_ASSERT_TRUE(parseConsoleCommand("exit").action == ConsoleAction::Exit);
    TEST_ASSERT_TRUE(parseConsoleCommand("quit").action == ConsoleAction::Exit);
    TEST_ASSERT_TRUE(parseConsoleCommand("logout").action == ConsoleAction::Exit);
    TEST_ASSERT_TRUE(parseConsoleCommand("wat").action == ConsoleAction::Unknown);
    // Case is not folded: the console is a command line, not a search box.
    TEST_ASSERT_TRUE(parseConsoleCommand("HELP").action == ConsoleAction::Unknown);
    TEST_ASSERT_TRUE(parseConsoleCommand("reboot now").action == ConsoleAction::Reboot);
    return 0;
}

/** Only `exit`/`quit`/`logout` end a session; a `reboot` restarts the device. */
int test_who_ends_the_session()
{
    TEST_ASSERT_TRUE(consoleActionEndsSession(ConsoleAction::Exit));
    TEST_ASSERT_FALSE(consoleActionEndsSession(ConsoleAction::Reboot));
    TEST_ASSERT_FALSE(consoleActionEndsSession(ConsoleAction::Help));
    TEST_ASSERT_FALSE(consoleActionEndsSession(ConsoleAction::Unknown));
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running SshCore tests...\n");
    int failures = 0;

    failures += test_the_port_rule();
    failures += test_the_credentials();
    failures += test_the_command_parser();
    failures += test_who_ends_the_session();

    if (failures == 0) {
        printf("All SshCore tests PASSED!\n");
    } else {
        printf("Some SshCore tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
