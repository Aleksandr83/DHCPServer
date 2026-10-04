/**
 * @file test_authattemptpolicy.cpp
 * @brief Host test for the login attempt counter (rule 23).
 *
 * The decision "is this header a guess at the password" is what stands between a
 * busy browser and a five-minute lockout of the whole interface, and it is pure
 * string logic — so it is checked here rather than on the board. The header has
 * no ESP-IDF dependency on purpose; `test_auth` cannot be built on the host
 * because `AuthManager` itself needs the mbedTLS headers.
 */

#include "core/AuthAttemptPolicy.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace std;

using namespace dhcp::core;

int main()
{
    // The defect of 04.10.2026: the browser's first, credential-free request and
    // the page's own `sendBeacon` carry no header at all. Neither is an attempt.
    assert(!countsAsFailedAttempt(""));

    // The scheme on its own is not a credential either, and neither is a scheme
    // this server does not use: the parser would refuse it without ever looking
    // at a password, so it must not spend the attempt budget.
    assert(!countsAsFailedAttempt("Basic"));
    assert(!countsAsFailedAttempt("Basic "));
    assert(!countsAsFailedAttempt("Bearer YWRtaW46YWRtaW4="));
    assert(!countsAsFailedAttempt("Digest username=\"admin\""));
    // Leading space: not the prefix, so not a credential this parser would read.
    assert(!countsAsFailedAttempt("  Basic YWRtaW46YWRtaW4="));

    // A real credential is counted — the protection itself, unchanged.
    assert(countsAsFailedAttempt("Basic YWRtaW46YWRtaW4="));
    assert(countsAsFailedAttempt("Basic YWRtaW46d3Jvbmc="));
    // An empty, whitespace or undecodable credential is a guess too: it reaches
    // the parser and fails there, which is the case worth counting.
    assert(countsAsFailedAttempt("Basic  "));
    assert(countsAsFailedAttempt("Basic !!!!"));

    printf("All AuthAttemptPolicy tests PASSED!\n");
    return 0;
}
