/**
 * @file test_certstore.cpp
 * @brief Host test for the certificate rules (stage 157, rule 23).
 *
 * The header carries no mbedTLS and no ESP-IDF dependency on purpose, so the
 * names of the files, the volume of the pair and the calendar arithmetic of the
 * validity are checked on the development machine instead of on the board. The
 * numbers here are the ones the page will offer and the API will accept — and
 * the two places where guessing them wrong would be invisible on a running
 * device are the mount point of the pair ("внутренний накопитель" is the
 * internal FAT `/fat`, not the SPIFFS volume of the web files) and the day the
 * certificate stops being valid.
 */

#include "../src/security/CertStore.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace std;

using namespace dhcp::security;

// The rules the operator fixed on 20.09.2026 — 5 years, P-256, 30 days of
// warning, one day of back-dating — must not drift silently.
static_assert(kValidityYears == 5, "the validity of a new certificate changed");
// The periods on offer (stage 160) come from the same decision: the ends, the
// count and the default are checked here so that a change to the table stops the
// build instead of silently changing what the page offers.
static_assert(kValidityYearChoiceCount == 5, "the number of offered periods changed");
static_assert(kValidityYearChoices[0] == 1 && kValidityYearChoices[4] == 10,
              "the ends of the list of periods changed");
static_assert(certValidityYearsAllowed(kValidityYears),
              "the default period is not one of the offered ones");
static_assert(!certValidityYearsAllowed(0) && !certValidityYearsAllowed(-1) &&
                  !certValidityYearsAllowed(4) && !certValidityYearsAllowed(6) &&
                  !certValidityYearsAllowed(11) && !certValidityYearsAllowed(100),
              "a period outside the offered list is accepted");
static_assert(kKeyBits == 256, "the key size is not P-256 any more");
static_assert(kSerialBytes == 8, "the serial lost its 64 bits of randomness");
static_assert(kDaysWarningWindow == 30, "the warning window changed");
static_assert(kExpiryWarnSec == 30 * 86400, "the warning window is not a month");
static_assert(kNotBeforeBackdateSec == 86400, "the back-dating of not-before changed");
static_assert(kIpv4Bytes == 4, "an IPv4 address is not four bytes");
static_assert(kCertStorageCount == 2, "the number of volumes changed");
static_assert(kInternalPreferredMaxBytes == 1024 * 1024, "the size rule changed");
static_assert(kTimeTextLen == 15, "an ASN.1 time is not YYYYMMDDhhmmss");

/** @brief Local helper: the epoch values below were computed independently. */
namespace {

constexpr int64_t kIssued = 1789898820;        // 2026-09-20 10:07:00 UTC
constexpr int64_t kIssuedPlus1Year = 1821434820;   // 2027-09-20 10:07:00 UTC
constexpr int64_t kIssuedPlus5Years = 1947665220;  // 2031-09-20 10:07:00 UTC
constexpr int64_t kIssuedPlus10Years = 2105518020; // 2036-09-20 10:07:00 UTC
constexpr int64_t kDay = dhcp::time::TimeMath::kSecondsPerDay;

// The counting rule of the page (stage 160) is checked where it is defined, so
// the displayed number can never disagree with the warning that goes with it.
static_assert(certDaysLeft(kIssued + kDay, kIssued) == 1, "a whole day left is one day");
static_assert(certDaysLeft(kIssued + 1, kIssued) == 1, "the last partial day is a day");
static_assert(certDaysLeft(kIssued, kIssued) == 0, "an expired pair has no days left");
static_assert(certDaysLeft(kIssued + kExpiryWarnSec, kIssued) == kDaysWarningWindow,
              "the day count and the warning window disagree");

} // namespace

int main()
{
    // ─── Which volume holds the pair ───
    // The internal volume is `/fat`: that is the partition `main.cpp` mounts
    // through FatFileSystem (the one that also holds cache.dat). The web files
    // live on the SPIFFS volume `/spiffs`, and a certificate there would be both
    // unreadable by the file explorer and lost on a data re-flash.
    assert(string(certMountPoint(CertStorage::Internal)) == "/fat");
    assert(string(certMountPoint(CertStorage::SdCard)) == "/sdcard");
    assert(certStorageIndex(CertStorage::Internal) == 0);
    assert(certStorageIndex(CertStorage::SdCard) == 1);
    assert(certStorageFromIndex(0) == CertStorage::Internal);
    assert(certStorageFromIndex(1) == CertStorage::SdCard);
    assert(certStorageFromIndex(2) == CertStorage::Internal);    // unknown NVS value
    assert(certStorageFromIndex(255) == CertStorage::Internal);  // and a stale one

    // ─── Where the two files are ───
    assert(certFolderPathIn(CertStorage::Internal) == "/fat/certs");
    assert(certFolderPathIn(CertStorage::SdCard) == "/sdcard/certs");
    assert(certFilePathIn(CertStorage::Internal) == "/fat/certs/server.crt");
    assert(keyFilePathIn(CertStorage::Internal) == "/fat/certs/server.key");
    assert(certFilePathIn(CertStorage::SdCard) == "/sdcard/certs/server.crt");
    assert(keyFilePathIn(CertStorage::SdCard) == "/sdcard/certs/server.key");
    assert(string(kCertFileName) == "server.crt");
    assert(string(kKeyFileName) == "server.key");
    assert(string(kCertFolderName) == "certs");      // lower case, like the rest
    // The folder name is the one the operator named for the layout, and the
    // paths above must survive the validation the file API applies to them.
    {
        string normalized;
        assert(dhcp::storage::PathUtil::normalize("/certs", normalized));
        assert(normalized == "/certs");
        assert(dhcp::storage::PathUtil::normalize("/certs/server.crt", normalized));
        assert(normalized == "/certs/server.crt");
    }

    // ─── The ASN.1 time text of the validity ───
    {
        char text[kTimeTextLen];
        assert(asn1Time(kIssued, text, sizeof text));
        assert(string(text) == "20260920100700");
        assert(asn1Time(0, text, sizeof text));
        assert(string(text) == "19700101000000");
        assert(asn1Time(1577836800, text, sizeof text));      // 2020-01-01
        assert(string(text) == "20200101000000");
        // Out of range or a short buffer is refused, not clamped: a certificate
        // with a silently changed date is worse than a refused generation.
        assert(!asn1Time(-1, text, sizeof text));
        assert(!asn1Time(kMaxUnixSec + 1, text, sizeof text));
        assert(!asn1Time(kIssued, text, kTimeTextLen - 1));
        assert(!asn1Time(kIssued, text, 0));
        assert(!asn1Time(kIssued, nullptr, sizeof text));
    }

    // ─── Five years later, day for day ───
    {
        int64_t end = 0;
        assert(certNotAfterEpoch(kIssued, kValidityYears, end));
        assert(end == kIssuedPlus5Years);

        // 29 February is the one date that does not exist five years later.
        assert(certNotAfterEpoch(1835438400, 5, end));        // 2028-02-29 12:00 UTC
        assert(end == 1993204800);                            // 2033-02-28 12:00 UTC
        assert(certNotAfterEpoch(1709208000, 5, end));        // 2024-02-29 12:00 UTC
        assert(end == 1866974400);                            // 2029-02-28 12:00 UTC

        // A clock set past the calendar's end is refused instead of wrapped.
        assert(!certNotAfterEpoch(-1, 5, end));
        assert(!certNotAfterEpoch(4102444800 + 1, 5, end));   // beyond 2100
    }

    // ─── The periods on offer (stage 160) ───
    // The API hands the list to the page and the store refuses anything outside
    // it, so a period here that is not offered (or the other way round) would be
    // an option the device rejects or a value the page can never send. Every
    // number of this table is checked: the ends, the gaps and the text the
    // refusal quotes.
    {
        int64_t end = 0;
        for (const int years : kValidityYearChoices) {
            assert(certValidityYearsAllowed(years));
            // A period the store accepts has to be writable into the calendar
            // from the day the device was built: one that is not would be refused
            // at generation time, on a page that offers it.
            assert(certNotAfterEpoch(kIssued, years, end));
        }
        // The count, the ends, the default and the refused values are checked
        // where they are defined (the static_asserts at the top of this file).

        // The text is built from the table and is what a refusal says, so it must
        // be the list read aloud — not a second copy of it (rule 39).
        assert(certValidityYearChoicesText() == "1, 2, 3, 5 or 10");
    }

    // ─── The ends of the list, day for day ───
    // One year and ten years are the shortest and the longest period the
    // operator can pick, and each of them ends on a different day of the same
    // calendar the five-year default was checked against.
    {
        int64_t end = 0;
        assert(certNotAfterEpoch(kIssued, 1, end));
        assert(end == kIssuedPlus1Year);
        assert(certNotAfterEpoch(kIssued, 10, end));
        assert(end == kIssuedPlus10Years);

        // 29 February: one year later gives 28 February, ten years later too.
        assert(certNotAfterEpoch(1835438400, 1, end));        // 2028-02-29 12:00 UTC
        assert(end == 1866974400);                            // 2029-02-28 12:00 UTC
        assert(certNotAfterEpoch(1835438400, 10, end));
        assert(end == 2150971200);                            // 2038-02-28 12:00 UTC

        // Ten years reaches the end of the calendar sooner than five: the pair a
        // browser refuses is one whose "not after" could not be written down.
        assert(certNotAfterEpoch(3786912000, 10, end));       // 2090-01-01 -> 2100-01-01
        assert(end == 4102444800);
        assert(!certNotAfterEpoch(3818448000, 10, end));      // 2091-01-01 -> 2101
        assert(!certNotAfterEpoch(4102444800, 10, end));      // 2100-01-01 -> 2110
    }

    // ─── Expired / not yet valid / about to expire ───
    assert(!certExpired(kIssuedPlus5Years, kIssued));
    assert(certExpired(kIssuedPlus5Years, kIssuedPlus5Years));   // the last second counts
    assert(certExpired(kIssuedPlus5Years, kIssuedPlus5Years + 1));
    assert(certNotYetValid(kIssued + 10, kIssued));
    assert(!certNotYetValid(kIssued, kIssued));                  // the first second counts

    // The warning appears inside the last month and nowhere earlier.
    assert(!certExpiresSoon(kIssuedPlus5Years, kIssued));
    assert(!certExpiresSoon(kIssuedPlus5Years, kIssuedPlus5Years - kExpiryWarnSec - 1));
    assert(certExpiresSoon(kIssuedPlus5Years, kIssuedPlus5Years - kExpiryWarnSec));
    assert(certExpiresSoon(kIssuedPlus5Years, kIssuedPlus5Years - 1));
    assert(!certExpiresSoon(kIssuedPlus5Years, kIssuedPlus5Years));   // expired is not "soon"

    // ─── Which volume a pair of a given size belongs on ───
    assert(preferredStorageForSize(2048) == CertStorage::Internal);       // a real pair
    assert(preferredStorageForSize(kInternalPreferredMaxBytes - 1) == CertStorage::Internal);
    assert(preferredStorageForSize(kInternalPreferredMaxBytes) == CertStorage::SdCard);

    // ─── The name that goes into CN and the SAN ───
    assert(isValidHostName("dhcpserver.local"));
    assert(isValidHostName("a"));
    assert(isValidHostName("host-1.example.com"));
    assert(isValidHostName("192.168.4.1"));                  // legal as a DNS name too
    assert(isValidHostName(string(kMaxHostNameLen, 'a')));
    assert(!isValidHostName(""));
    assert(!isValidHostName(string(kMaxHostNameLen + 1, 'a')));
    assert(!isValidHostName(".local"));                      // leading dot
    assert(!isValidHostName("local."));                      // trailing dot
    assert(!isValidHostName("-host"));                       // leading dash
    assert(!isValidHostName("host-"));                       // trailing dash
    assert(!isValidHostName("a..b"));                        // empty label
    assert(!isValidHostName("a b"));                         // space
    assert(!isValidHostName("a_b"));                         // underscore
    assert(!isValidHostName("a/b"));                         // a name is not a path
    assert(commonName("dhcpserver.local") == "CN=dhcpserver.local");
    assert(string(kDefaultCommonName) == "dhcpserver.local");
    // The default name is what a device that was never given one issues, so it
    // has to pass the same rule as a typed name: otherwise the first generation,
    // the one no field has been filled for yet, would be refused.
    assert(isValidHostName(kDefaultCommonName));
    // A name typed on the certificates page is that same kind of value: both
    // cases of letter, digits, dots and dashes. Anything else a text field can
    // carry is refused when it arrives, with the value quoted in the message.
    assert(isValidHostName("My-DHCPServer-2.local"));
    assert(isValidHostName("printer-1"));
    assert(!isValidHostName(" dhcpserver.local"));       // the page trims, the store refuses
    assert(!isValidHostName("dhcpserver.local "));
    assert(!isValidHostName("dhcpserver.local:443"));    // a name is not an address
    assert(!isValidHostName("*.local"));                 // no wildcards

    // ─── The address that goes into the SAN as raw bytes ───
    {
        uint8_t ip[kIpv4Bytes] = {0, 0, 0, 0};
        assert(parseIpv4("192.168.4.1", ip));
        assert(ip[0] == 192 && ip[1] == 168 && ip[2] == 4 && ip[3] == 1);
        assert(ipv4Text(ip) == "192.168.4.1");
        assert(parseIpv4("0.0.0.0", ip));
        assert(ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0);
        assert(parseIpv4("255.255.255.255", ip));
        assert(ip[0] == 255 && ip[3] == 255);

        // Everything a browser would interpret differently is refused, so no
        // certificate is written that only some clients accept.
        assert(!parseIpv4("", ip));
        assert(!parseIpv4("1.2.3", ip));
        assert(!parseIpv4("1.2.3.4.5", ip));
        assert(!parseIpv4("1.2.3.", ip));
        assert(!parseIpv4("192.168.4.256", ip));
        assert(!parseIpv4("192.168.04.1", ip));               // leading zero
        assert(!parseIpv4("192.168.4.1 ", ip));
        assert(!parseIpv4("a.b.c.d", ip));
        assert(!parseIpv4("192.168.4.-1", ip));
        assert(!parseIpv4("192.168.4.1000", ip));
    }

    // ─── May the HTTPS server run with what is stored? ───
    {
        CertInfo info;                                   // nothing known yet
        assert(certStatus(info) == CertStatus::StorageUnavailable);
        assert(!certUsable(certStatus(info)));

        info.available = true;                           // volume is there, no files
        assert(certStatus(info) == CertStatus::NoPair);

        info.present = true;                             // files are there, unreadable
        assert(certStatus(info) == CertStatus::Unreadable);

        info.valid = true;                               // parsed, dates unknown to us
        assert(certStatus(info) == CertStatus::Ready);
        assert(certUsable(certStatus(info)));

        info.expired = true;                             // ended
        assert(certStatus(info) == CertStatus::Expired);

        info.expired = false;
        info.notYetValid = true;                         // clock behind the certificate
        assert(certStatus(info) == CertStatus::NotYetValid);

        // The order of the answers matters: a file that does not parse is not
        // reported as an expired certificate, and a missing volume is not
        // reported as a missing certificate — on the page those two send the
        // operator to completely different places.
        info.present = false;
        info.valid = false;
        info.notYetValid = false;
        info.expired = true;
        assert(certStatus(info) == CertStatus::NoPair);
        info.available = false;
        assert(certStatus(info) == CertStatus::StorageUnavailable);

        // Every status has a name: an empty string or a repeated name would make
        // the page translate the same situation twice or show nothing.
        const CertStatus all[] = {CertStatus::Ready, CertStatus::NoStore,
                                  CertStatus::StorageUnavailable, CertStatus::NoPair,
                                  CertStatus::Unreadable, CertStatus::Expired,
                                  CertStatus::NotYetValid};
        for (const CertStatus status : all) {
            assert(certStatusName(status) != nullptr && certStatusName(status)[0] != '\0');
            assert(string(certStatusName(status)) != "unknown");
        }
        assert(string(certStatusName(CertStatus::Ready)) == "ready");
        assert(string(certStatusName(CertStatus::NoPair)) == "no_certificate");
        assert(!certUsable(CertStatus::NoStore));
        assert(!certUsable(CertStatus::Expired));
    }

    // ─── The names the page and the API use for the two volumes (stage 160) ───
    // The JSON value of the setting is also the dictionary key of the page, so
    // the pair has to survive a round-trip: a name that parses back into the
    // other volume would move the certificate without saying so.
    {
        assert(string(certStorageName(CertStorage::Internal)) == "internal");
        assert(string(certStorageName(CertStorage::SdCard)) == "sdcard");

        CertStorage parsed = CertStorage::SdCard;
        assert(certStorageFromName("internal", parsed) && parsed == CertStorage::Internal);
        assert(certStorageFromName("sdcard", parsed) && parsed == CertStorage::SdCard);

        const CertStorage both[] = {CertStorage::Internal, CertStorage::SdCard};
        for (const CertStorage storage : both) {
            CertStorage back = CertStorage::SdCard;
            assert(certStorageFromName(certStorageName(storage), back));
            assert(back == storage);
        }

        // A name that came from the page is a request, and a request that was
        // not understood must be refused — unlike a stale NVS value, which falls
        // back to the internal volume (certStorageFromIndex above). "sd" or
        // "fat" are the volume ids of the file explorer, not these names, and
        // accepting them would turn a typo into a silent move of the pair.
        assert(!certStorageFromName("", parsed));
        assert(!certStorageFromName("sd", parsed));
        assert(!certStorageFromName("fat", parsed));
        assert(!certStorageFromName("Internal", parsed));
        assert(!certStorageFromName("SDCARD", parsed));
        assert(!certStorageFromName("/sdcard", parsed));
    }

    // ─── How many days are left, as the page shows them (stage 160) ───
    {
        assert(certDaysLeft(kIssued + 5 * kDay, kIssued) == 5);
        assert(certDaysLeft(kIssued + kDay, kIssued) == 1);
        // The last partial day counts as a whole one: "0 days left" next to a
        // certificate that is valid until this evening would read as expired.
        assert(certDaysLeft(kIssued + 1, kIssued) == 1);
        assert(certDaysLeft(kIssued + kDay + 1, kIssued) == 2);
        assert(certDaysLeft(kIssued + kExpiryWarnSec, kIssued) == kDaysWarningWindow);
        // The number and the warning are two answers to the same question, and
        // the page shows them side by side: at a month they must agree.
        assert(certExpiresSoon(kIssued + kExpiryWarnSec, kIssued));
        assert(!certExpiresSoon(kIssued + kExpiryWarnSec + 1, kIssued));
        assert(certDaysLeft(kIssued + kExpiryWarnSec + 1, kIssued) == kDaysWarningWindow + 1);
        // Expired (or expiring at this very second) is 0 days, never a negative
        // count: the page would have to invent a sign for it.
        assert(certDaysLeft(kIssued, kIssued) == 0);
        assert(certDaysLeft(kIssued - 1, kIssued) == 0);
        assert(certDaysLeft(kIssued - 10 * kDay, kIssued) == 0);
        assert(certDaysLeft(0, kIssued) == 0);
        // The five-year validity of the operator's decision, in days.
        assert(certDaysLeft(kIssuedPlus5Years, kIssued) == 1826);   // two leap years
    }

    printf("All CertStore tests PASSED!\n");
    return 0;
}
