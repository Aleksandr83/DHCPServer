/**
 * @file test_dhcplogwriter.cpp
 * @brief Unit tests for the DHCP log as a file: where its archives go and what it
 *        calls itself (stage 212).
 *
 * The operator asked for a file log on the DHCP Logging page — its own switch, a
 * size of its own, archived to the card — and the fifth file log of the project was
 * built the way the other four are: one writer class per log, all of them on
 * `core::LogFileWriter`, with the two hooks each of them has to answer. The
 * rotation policy itself is checked by `test_logfilewriter`; what is checked here is
 * that **this** log's hooks are right — a writer whose archive folder was copied
 * from the DNS log would put DHCP archives among the forwarded answers, and one that
 * named the wrong log in a failure would send the operator to the wrong file.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_dhcplogwriter.cpp src/core/Subnet.cpp -o test_dhcplogwriter
 */
#include "src/dhcp/DhcpLogWriter.h"
#include "src/core/ArchiveFileLogTarget.h"

#include <cstdio>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#endif

using namespace std;

using dhcp::core::ArchiveFileLogTarget;
using dhcp::core::LogLevel;
using dhcp::core::LogStamp;
using dhcp::dhcp::DhcpLogWriter;

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const string& what)
{
    ++g_checks;
    if (ok) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        ++g_failed;
    }
}

namespace {

const string kRoot = ".dhcplogtest";
const string kLive = kRoot + "/fat/logs/Dhcp.log";
const string kArchive = kRoot + "/sdcard/log/Archive/Dhcp/2026.10.16-2026.10.16/Dhcp.log";
const LogStamp kStamp{2026, 10, 16, 21, 30, 0, true};

/** One record, 50 bytes: the writer adds the newline, as it does for every log. */
string longLine(int i)
{
    char num[8];
    snprintf(num, sizeof(num), "%04d", i);
    return "DHCP OFFER 192.168.1." + string(num) + " " + string(28, 'x');
}

string longRecord(int i) { return longLine(i) + "\n"; }

int makeDir(const string& path)
{
#if defined(_WIN32)
    return ::_mkdir(path.c_str());
#else
    return ::mkdir(path.c_str(), 0777);
#endif
}

void removeTree(const string& path)
{
    if (DIR* dir = opendir(path.c_str())) {
        vector<string> names;
        while (struct dirent* ent = readdir(dir)) {
            const string name = ent->d_name;
            if (name != "." && name != "..") names.push_back(name);
        }
        closedir(dir);
        for (const auto& name : names) removeTree(path + "/" + name);
    }
    remove(path.c_str());
    rmdir(path.c_str());
}

string readFile(const string& path)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) return string();
    string out;
    char buf[512];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), file)) > 0) out.append(buf, got);
    fclose(file);
    return out;
}

} // namespace

static void test_a_full_file_is_archived_under_its_own_folder()
{
    removeTree(kRoot);
    makeDir(kRoot);
    makeDir(kRoot + "/fat");
    makeDir(kRoot + "/sdcard");

    // Cap 200: three 51-byte lines fit, the fourth does not.
    auto writer = make_unique<DhcpLogWriter>(kLive, kRoot + "/sdcard",
                                             [] { return true; }, nullptr, 200);
    ArchiveFileLogTarget target(move(writer), [] { return kStamp; });
    for (int i = 1; i <= 4; ++i) target.append(LogLevel::Info, longLine(i));

    check(readFile(kArchive) == longRecord(1) + longRecord(2) + longRecord(3),
          "the DHCP log's archives live in log/Archive/Dhcp, its own folder on the card");
    check(readFile(kLive) == longRecord(4),
          "and its live file is `/logs/Dhcp.log` on the volume it was built with");
    check(target.description() == kLive,
          "the target describes itself as the DHCP log's live file, not another log's");
    removeTree(kRoot);
}

static void test_the_lines_keep_their_order()
{
    removeTree(kRoot);
    makeDir(kRoot);
    makeDir(kRoot + "/fat");
    makeDir(kRoot + "/sdcard");

    // A DHCP packet is one line, and the handshake of one client is four of them in
    // the order they happened: a file that reordered them would read as a client
    // acknowledging an address before it was offered one.
    auto writer = make_unique<DhcpLogWriter>(kLive, kRoot + "/sdcard",
                                             [] { return true; }, nullptr, 4096);
    ArchiveFileLogTarget target(move(writer), [] { return kStamp; });
    target.append(LogLevel::Info, "DHCP DISCOVER from 24:0a:c4:01:23:45");
    target.append(LogLevel::Info, "DHCP OFFER 192.168.1.100 to 24:0a:c4:01:23:45");
    target.append(LogLevel::Info, "DHCP REQUEST 192.168.1.100");
    target.append(LogLevel::Info, "DHCP ACK 192.168.1.100 to 24:0a:c4:01:23:45");

    check(readFile(kLive) ==
              "DHCP DISCOVER from 24:0a:c4:01:23:45\n"
              "DHCP OFFER 192.168.1.100 to 24:0a:c4:01:23:45\n"
              "DHCP REQUEST 192.168.1.100\n"
              "DHCP ACK 192.168.1.100 to 24:0a:c4:01:23:45\n",
          "the handshake of one client is written in the order it happened");
    removeTree(kRoot);
}

int main()
{
    printf("Running DhcpLogWriter tests...\n");
    test_a_full_file_is_archived_under_its_own_folder();
    test_the_lines_keep_their_order();

    if (g_failed == 0) {
        printf("All %d DhcpLogWriter checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d DhcpLogWriter checks FAILED!\n", g_failed, g_checks);
    return 1;
}
