/**
 * @file test_timedefaults.cpp
 * @brief Unit tests for the factory values of the Time Server page (stage 194).
 *
 * The operator asked for a "Reset to default" button at the bottom of the Time
 * Server page, shown only while that page differs from the factory values. The
 * two things this file pins are the ones a button like that gets wrong silently:
 * *which* settings it owns — it owns the switches of the page and the fields of
 * the "External NTP server" block, and must leave the Logging sub-page's settings
 * alone — and *what* the factory values are, which is why they are stated here
 * instead of only being read: changing a default then becomes a deliberate act
 * with a failing test, not a line nobody notices.
 *
 * Build (MinGW):
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST -I. \
 *       test/test_timedefaults.cpp -o test_timedefaults
 */
#include "src/core/TimeDefaults.h"

#include <cstdio>
#include <string>

using namespace std;

using dhcp::core::TimeConfig;
using dhcp::core::resetTimePageToDefaults;
using dhcp::core::timePageIsDefault;

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

/** A config that differs from the factory one in every field the page owns. */
static TimeConfig changedPage()
{
    TimeConfig cfg;
    cfg.enabled = true;
    cfg.syncEnabled = false;
    cfg.externalNtp = "time.google.com";
    cfg.timezone = "UTC";
    cfg.utcOffsetHours = 0;
    cfg.syncIntervalSec = 3600;
    cfg.allowOwnSubnet = false;
    cfg.onlyAllowedComputers = true;
    cfg.rateLimitPerSec = 10;
    return cfg;
}

/** A config whose Logging sub-page settings were all changed. */
static TimeConfig changedLogging()
{
    TimeConfig cfg;
    cfg.logTerminal = true;
    cfg.logRest = true;
    cfg.logFile = true;
    cfg.logUrl = "http://192.168.1.5/ntp";
    cfg.logAuthEnabled = true;
    cfg.logAuthUser = "logger";
    cfg.logAuthPassword = "secret";
    return cfg;
}

static void test_factory_values()
{
    const TimeConfig factory;
    check(factory.externalNtp == "pool.ntp.org, time.google.com, time.cloudflare.com",
          "factory servers: three of them, from three different operators");
    check(factory.timezone == "Europe/Moscow", "factory timezone: Europe/Moscow");
    check(factory.utcOffsetHours == 3, "factory offset: +3");
    check(factory.syncIntervalSec == 86400, "factory interval: 24 h");
    check(factory.rateLimitPerSec == 5, "factory reply limit: 5/s");
    check(factory.enabled == false, "factory: the NTP server is off");
    check(factory.syncEnabled == true, "factory: the SNTP client is on");
    check(factory.allowOwnSubnet == true, "factory: only the own subnet is answered");
    check(factory.onlyAllowedComputers == false,
          "factory: any computer of that subnet is answered");
}

static void test_default_detection()
{
    check(timePageIsDefault(TimeConfig()), "a config straight from the initialisers is default");
    check(!timePageIsDefault(changedPage()), "a page changed in every field is not");
}

static void test_each_field_counts()
{
    // One field at a time: each of the nine has to be able to bring the button
    // back on its own, and these nine are the set the operator named — the
    // switches of the page plus the fields of the "External NTP server" block.
    {
        TimeConfig cfg;
        cfg.enabled = !cfg.enabled;
        check(!timePageIsDefault(cfg), "Enable NTP server counts");
    }
    {
        TimeConfig cfg;
        cfg.syncEnabled = !cfg.syncEnabled;
        check(!timePageIsDefault(cfg), "Sync clock (NTP client) counts");
    }
    {
        TimeConfig cfg;
        cfg.allowOwnSubnet = !cfg.allowOwnSubnet;
        check(!timePageIsDefault(cfg), "Allow only own subnet counts");
    }
    {
        TimeConfig cfg;
        cfg.onlyAllowedComputers = !cfg.onlyAllowedComputers;
        check(!timePageIsDefault(cfg), "Only allowed computers counts");
    }
    {
        TimeConfig cfg;
        cfg.externalNtp = "ntp1.vniiftri.ru";
        check(!timePageIsDefault(cfg), "the external NTP server counts");
    }
    {
        TimeConfig cfg;
        cfg.timezone = "UTC";
        check(!timePageIsDefault(cfg), "the timezone counts");
    }
    {
        TimeConfig cfg;
        cfg.utcOffsetHours = 0;
        check(!timePageIsDefault(cfg), "the custom offset counts");
    }
    {
        TimeConfig cfg;
        cfg.syncIntervalSec = 3600;
        check(!timePageIsDefault(cfg), "the sync interval counts");
    }
    {
        TimeConfig cfg;
        cfg.rateLimitPerSec = 10;
        check(!timePageIsDefault(cfg), "the reply limit counts");
    }
}

static void test_logging_is_not_this_page()
{
    // The Logging sub-page owns its own settings and has its own page: a change
    // there must not offer to reset this one, or the button would sit on screen
    // for ever and its press would look like it had done nothing.
    check(timePageIsDefault(changedLogging()),
          "the logging settings are not part of this page");
}

static void test_reset_puts_the_page_back()
{
    TimeConfig cfg = changedPage();
    cfg.logFile = true;
    cfg.logUrl = "http://192.168.1.5/ntp";
    cfg.logAuthUser = "logger";

    resetTimePageToDefaults(cfg);

    check(timePageIsDefault(cfg), "after the reset the page is back at the factory values");
    check(cfg.logFile == true && cfg.logUrl == "http://192.168.1.5/ntp" &&
              cfg.logAuthUser == "logger",
          "and not one logging setting was touched");
}

static void test_reset_of_a_default_page()
{
    TimeConfig cfg;
    resetTimePageToDefaults(cfg);
    check(timePageIsDefault(cfg), "resetting an already-factory page leaves it factory");
}

static void test_reset_keeps_a_lone_logging_change()
{
    // The other direction: a page that is factory except for the Logging
    // sub-page must stay factory after a reset, logging included.
    TimeConfig cfg = changedLogging();
    const string url = cfg.logUrl;
    const string user = cfg.logAuthUser;

    resetTimePageToDefaults(cfg);

    check(timePageIsDefault(cfg), "a reset leaves a page that was already factory alone");
    check(cfg.logUrl == url && cfg.logAuthUser == user && cfg.logFile == true,
          "logging survives that reset unchanged");
}

int main()
{
    printf("Running TimeDefaults tests...\n");
    test_factory_values();
    test_default_detection();
    test_each_field_counts();
    test_logging_is_not_this_page();
    test_reset_puts_the_page_back();
    test_reset_of_a_default_page();
    test_reset_keeps_a_lone_logging_change();

    if (g_failed == 0) {
        printf("All %d TimeDefaults checks PASSED!\n", g_checks);
        return 0;
    }
    printf("%d of %d TimeDefaults checks FAILED!\n", g_failed, g_checks);
    return 1;
}
