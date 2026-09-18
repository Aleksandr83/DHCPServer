/**
 * @file test_errorlogcore.cpp
 * @brief Unit tests for the part of the error log that is plain C++: the line
 *        format, the truncation rule, and moving queued messages into a target.
 *
 * The device writes the log through a queue in a task of its own, which is not
 * testable on a host — but everything that makes the log *trustworthy* is:
 *
 *   * a line always says **when** it happened, and a device whose clock is not
 *     set gets an uptime stamp instead of a 1970 date that looks like a fact;
 *   * a message too long for one queue item is cut **with a marker**, so nobody
 *     mistakes a cut message for a short one;
 *   * messages lost to a full queue (or refused by the target) are counted and
 *     reported inside the log, because a log with silent holes is worse than no
 *     log.
 *
 * Build (MinGW g++ 13, PATH must contain C:\Qt\Tools\mingw1310_64\bin):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_errorlogcore.cpp src/core/ErrorLogCore.cpp -o t_errorlogcore.exe
 *
 * The harness always exits with 0 — the proof is the printed text.
 */
#ifdef DHCP_TEST_HOST

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "src/core/ErrorLogCore.h"

using dhcp::core::ErrorLogCore;
using dhcp::core::ErrorLogEntry;
using dhcp::core::IErrorLogTarget;
using dhcp::core::IErrorQueue;
using dhcp::core::LogLevel;

static int g_fail = 0;

static void check(bool ok, const std::string& what)
{
    if (ok) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_fail;
    }
}

/** A queue that behaves like the device's, minus the FreeRTOS part. */
class StubQueue : public IErrorQueue {
public:
    size_t capacity = 64;
    std::vector<ErrorLogEntry> items;

    bool push(const ErrorLogEntry& entry) override
    {
        if (items.size() >= capacity) return false;
        items.push_back(entry);
        return true;
    }

    bool pop(ErrorLogEntry& out, uint32_t /*timeoutMs*/) override
    {
        if (items.empty()) return false;
        out = items.front();
        items.erase(items.begin());
        return true;
    }
};

/** A target that keeps the lines in memory (and can be told to refuse them). */
class StubTarget : public IErrorLogTarget {
public:
    std::vector<std::string> lines;
    std::vector<LogLevel> levels;
    size_t failFrom = static_cast<size_t>(-1);   // refuse once this many lines are in
    std::string name = "stub";

    bool append(LogLevel level, const std::string& line) override
    {
        if (lines.size() >= failFrom) return false;
        levels.push_back(level);
        lines.push_back(line);
        return true;
    }

    const std::string& description() const override { return name; }
};

static bool looksLikeDateStamp(const std::string& stamp)
{
    if (stamp.size() != 19) return false;
    if (stamp[4] != '-' || stamp[7] != '-' || stamp[10] != ' ' ||
        stamp[13] != ':' || stamp[16] != ':') {
        return false;
    }
    for (size_t i = 0; i < stamp.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if (stamp[i] < '0' || stamp[i] > '9') return false;
    }
    return true;
}

static size_t countChar(const std::string& text, char c)
{
    return static_cast<size_t>(std::count(text.begin(), text.end(), c));
}

static void test_a_line_says_when_it_happened()
{
    std::printf("test_a_line_says_when_it_happened\n");

    const std::string withClock =
        ErrorLogCore::formatLine(LogLevel::Error, "dns", "boom", 1758000000ULL, 12);
    check(withClock.find(" [E] dns: boom") != std::string::npos,
          "level, tag and message are there");
    check(looksLikeDateStamp(withClock.substr(0, 19)),
          "and the stamp is a real date/time: " + withClock.substr(0, 19));
    check(withClock.find("t+") == std::string::npos,
          "the uptime form is not used when the clock is set");

    const std::string noClock =
        ErrorLogCore::formatLine(LogLevel::Error, "dns", "boom", 100ULL, 152);
    check(noClock == "t+152s [E] dns: boom",
          "a device whose clock is not set gets an uptime stamp, not 1970: " + noClock);
    check(!ErrorLogCore::clockIsSet(1577836799ULL), "one second before 2020 is 'no clock'");
    check(ErrorLogCore::clockIsSet(1577836800ULL), "2020-01-01 is a settable clock");

    check(std::string(ErrorLogCore::levelTag(LogLevel::Error)) == "[E]", "[E] for errors");
    check(std::string(ErrorLogCore::levelTag(LogLevel::Warn)) == "[W]", "[W] for warnings");
}

static void test_a_long_message_is_marked_as_truncated()
{
    std::printf("test_a_long_message_is_marked_as_truncated\n");

    const std::string shortMessage = "short";
    check(ErrorLogCore::clampMessage(shortMessage) == shortMessage,
          "a message that fits is left alone");

    const std::string longMessage(500, 'x');
    const std::string clamped = ErrorLogCore::clampMessage(longMessage);
    check(countChar(clamped, 'x') == ErrorLogCore::kMaxMessage,
          "a long message is cut exactly at the limit");
    check(clamped.find("...(truncated)") != std::string::npos,
          "and carries the marker, so a cut is never mistaken for an end");

    const std::string line =
        ErrorLogCore::formatLine(LogLevel::Warn, "log", longMessage, 100ULL, 1);
    check(line.find("...(truncated)") != std::string::npos,
          "the marker survives the formatting too");
}

static void test_submit_then_drain_writes_every_line()
{
    std::printf("test_submit_then_drain_writes_every_line\n");

    StubQueue queue;
    StubTarget target;
    ErrorLogCore log(queue, target, []() { return 7u; });

    check(log.submit(LogLevel::Error, "dns", "first"), "the first message is queued");
    check(log.submit(LogLevel::Warn, "fs", "second"), "the second one too");
    check(queue.items.size() == 2, "both are waiting in the queue");
    check(log.dropped() == 0, "nothing was lost");

    const uint32_t written = log.drain(0);
    check(written == 2, "the drain wrote both lines");
    check(target.lines.size() == 2, "and the target has them");
    check(target.lines[0].find("first") != std::string::npos, "in order (1)");
    check(target.lines[1].find("second") != std::string::npos, "in order (2)");
    check(target.levels[1] == LogLevel::Warn, "with their levels");
    check(log.drain(0) == 0, "draining an empty queue writes nothing");
}

static void test_a_full_queue_is_reported_inside_the_log()
{
    std::printf("test_a_full_queue_is_reported_inside_the_log\n");

    StubQueue queue;
    queue.capacity = 2;
    StubTarget target;
    ErrorLogCore log(queue, target);

    check(log.submit(LogLevel::Error, "dns", "one"), "message 1 fits");
    check(log.submit(LogLevel::Error, "dns", "two"), "message 2 fits");
    check(!log.submit(LogLevel::Error, "dns", "three"), "message 3 does not — and does not wait");
    check(!log.submit(LogLevel::Error, "dns", "four"), "nor message 4");
    check(log.dropped() == 2, "both losses are counted");

    const uint32_t written = log.drain(0);
    check(written == 3, "the drain wrote the notice plus the two surviving messages");
    check(target.lines[0].find("2 message(s) lost") != std::string::npos,
          "the loss is the first thing the log says: " + target.lines[0]);
    check(target.levels[0] == LogLevel::Warn, "and it is a warning, not an error");
    check(log.dropped() == 0, "once written, the count starts over");
}

static void test_a_target_that_refuses_lines_is_counted_too()
{
    std::printf("test_a_target_that_refuses_lines_is_counted_too\n");

    StubQueue queue;
    StubTarget target;
    target.failFrom = 0;   // the first append fails; the test opens the door after it
    ErrorLogCore log(queue, target);

    log.submit(LogLevel::Error, "dns", "lost to the target");
    log.drain(0);
    check(log.dropped() == 1, "a line the target refused counts as a loss");
    check(target.lines.empty(), "and nothing was written");

    target.failFrom = static_cast<size_t>(-1);   // the target works again
    log.submit(LogLevel::Error, "dns", "kept");
    log.drain(0);
    check(target.lines.size() == 2, "the next drain writes the notice and the message");
    check(target.lines[0].find("1 message(s) lost") != std::string::npos,
          "the device's own failure is admitted too: " + target.lines[0]);
    check(target.lines[1].find("kept") != std::string::npos, "then the message itself");
}

int main()
{
    test_a_line_says_when_it_happened();
    test_a_long_message_is_marked_as_truncated();
    test_submit_then_drain_writes_every_line();
    test_a_full_queue_is_reported_inside_the_log();
    test_a_target_that_refuses_lines_is_counted_too();

    if (g_fail == 0) {
        std::printf("\nPASSED!\n");
        return 0;
    }
    std::printf("\nFAILED (%d)\n", g_fail);
    return 1;
}

#endif // DHCP_TEST_HOST
