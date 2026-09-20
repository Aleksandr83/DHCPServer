#ifndef DHCP_SECURITY_CERTIFICATEBUILDER_H
#define DHCP_SECURITY_CERTIFICATEBUILDER_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "time/TimeMath.h"

namespace dhcp {
namespace security {

// ─── What goes into the certificate ─────────────────────────────────────────
// Every number of the certificate rules has a name here (rule 39), because the
// host test checks them and the web page offers the same bounds.

/** @brief Common Name of a generated certificate (operator decision, 20.09.2026). */
constexpr const char* kDefaultCommonName = "dhcpserver.local";
/** @brief Default validity of a generated certificate, in years (operator decision). */
constexpr int kValidityYears = 5;
/**
 * @brief The validity periods the operator can pick, in years (operator decision).
 *
 * This table is the only place these bounds exist (rule 39): the REST answer
 * carries the list to the page, @ref certValidityYearsAllowed refuses everything
 * outside it, and the host test checks both ends.
 */
constexpr int kValidityYearChoices[] = {1, 2, 3, 5, 10};
/** @brief Entries in @ref kValidityYearChoices. */
constexpr size_t kValidityYearChoiceCount =
    sizeof(kValidityYearChoices) / sizeof(kValidityYearChoices[0]);

/** @brief True when @p years is a period the operator can pick. */
constexpr bool certValidityYearsAllowed(int years)
{
    for (size_t i = 0; i < kValidityYearChoiceCount; i++) {
        if (kValidityYearChoices[i] == years) return true;
    }
    return false;
}

/**
 * @brief The periods of @ref kValidityYearChoices as one text: "1, 2, 3, 5 or 10".
 *
 * Written from the table, so a refusal can never name periods the store would
 * have accepted.
 */
inline std::string certValidityYearChoicesText()
{
    std::string text;
    for (size_t i = 0; i < kValidityYearChoiceCount; i++) {
        if (i > 0) text += (i + 1 == kValidityYearChoiceCount) ? " or " : ", ";
        text += std::to_string(kValidityYearChoices[i]);
    }
    return text;
}
/** @brief Key size in bits: ECDSA P-256 (operator decision: "ключ — ECDSA P-256"). */
constexpr int kKeyBits = 256;
/**
 * @brief Bytes of the random serial number of the certificate.
 *
 * The CA/Browser Forum has required at least 64 bits of entropy in a serial
 * since 2016, so eight random bytes — not a counter that starts again at every
 * generation.
 */
constexpr size_t kSerialBytes = 8;
/** @brief The interface warns while less than this many days are left (operator decision). */
constexpr int kDaysWarningWindow = 30;
/** @brief Bytes of an IPv4 address; the SAN extension carries it as raw bytes. */
constexpr size_t kIpv4Bytes = 4;
/** @brief Digits of one IPv4 part at most ("255" is the longest). */
constexpr size_t kMaxIpv4PartDigits = 3;
/** @brief Largest value of one IPv4 part. */
constexpr int kMaxIpv4Octet = 255;
/** @brief Room for "255.255.255.255" plus the terminator. */
constexpr size_t kIpv4TextLen = 16;
/** @brief Longest DNS name accepted for CN/SAN (RFC 1035 allows 253 characters). */
constexpr size_t kMaxHostNameLen = 253;
/** @brief `YYYYMMDDhhmmss` plus the terminator — the validity format X.509 uses. */
constexpr size_t kTimeTextLen = 15;
/** @brief Largest Unix time the project's calendar covers (uint32 seconds, 2106). */
constexpr int64_t kMaxUnixSec = 4294967295LL;
/** @brief 2020-01-01 00:00:00 UTC: below this the device clock counts as not set. */
constexpr int64_t kClockSetSinceEpoch = 1577836800;

/**
 * @brief Not-before of a generated certificate: one day in the past.
 *
 * The device takes its time from the network, and a client whose clock is a few
 * minutes ahead would otherwise refuse a certificate that is still "not yet
 * valid". X.509 back-dating is the usual answer to that.
 */
constexpr int64_t kNotBeforeBackdateSec = time::TimeMath::kSecondsPerDay;
/** @brief The "expires soon" window in seconds (rule 39: one name per number). */
constexpr int64_t kExpiryWarnSec = kDaysWarningWindow * time::TimeMath::kSecondsPerDay;

/**
 * @brief `YYYYMMDDhhmmss` text of a Unix time — the validity format of X.509.
 *
 * The calendar comes from `time::TimeMath`, so there is exactly one
 * implementation of the date arithmetic in the project (rule 2) and it is the
 * one the host tests already cover.
 *
 * @param[in]  epoch  Unix seconds (UTC).
 * @param[out] out    Buffer of at least @ref kTimeTextLen bytes.
 * @param[in]  outLen Capacity of @p out.
 * @return false when @p epoch is outside 1970..2106 or the buffer is too small —
 *         the caller refuses to write a certificate with a nonsense date
 *         instead of clamping it silently.
 */
inline bool asn1Time(int64_t epoch, char* out, size_t outLen)
{
    if (out == nullptr || outLen < kTimeTextLen) return false;
    if (epoch < 0 || epoch > kMaxUnixSec) return false;
    const time::DateTime dt = time::TimeMath::fromUnixSec(static_cast<uint32_t>(epoch));
    if (!time::TimeMath::isValidDate(dt.year, dt.month, dt.day)) return false;
    const int written = std::snprintf(out, outLen, "%04d%02d%02d%02d%02d%02d",
                                      dt.year, dt.month, dt.day,
                                      dt.hour, dt.minute, dt.second);
    return written == static_cast<int>(kTimeTextLen - 1);
}

/**
 * @brief End of validity: the same UTC date @p years later.
 *
 * A certificate issued on 29 February of a leap year gets 28 February when the
 * later year has no 29th — the only place where "the same date next time" does
 * not exist. Anything the project's calendar cannot represent (a clock set past
 * 2100) is refused instead of wrapped around.
 *
 * @param[in]  issuedAt Unix seconds the certificate is issued at.
 * @param[in]  years    Validity in years (@ref kValidityYears in production).
 * @param[out] out      Unix seconds of the end of validity.
 * @return false when @p issuedAt or the result is outside the calendar range.
 */
inline bool certNotAfterEpoch(int64_t issuedAt, int years, int64_t& out)
{
    if (issuedAt < 0 || issuedAt > kMaxUnixSec) return false;
    time::DateTime dt = time::TimeMath::fromUnixSec(static_cast<uint32_t>(issuedAt));
    dt.year += years;
    if (!time::TimeMath::isValidDate(dt.year, dt.month, dt.day)) {
        const int days = time::TimeMath::daysInMonth(dt.year, dt.month);
        if (days <= 0 || !time::TimeMath::isValidDate(dt.year, dt.month, days)) return false;
        dt.day = days;
    }
    out = static_cast<int64_t>(time::TimeMath::toUnixSec(dt));
    return true;
}

/** @brief True once @p now has reached the end of validity. */
constexpr bool certExpired(int64_t notAfter, int64_t now)
{
    return now >= notAfter;
}

/** @brief True while the certificate is not valid yet (the device clock is behind). */
constexpr bool certNotYetValid(int64_t notBefore, int64_t now)
{
    return now < notBefore;
}

/** @brief True when less than @ref kExpiryWarnSec is left — the page shows a warning. */
constexpr bool certExpiresSoon(int64_t notAfter, int64_t now)
{
    return !certExpired(notAfter, now) && (notAfter - now) <= kExpiryWarnSec;
}

/**
 * @brief Whole days of validity left, as the page shows them (stage 160).
 *
 * Rounded up, so the last partial day counts as a day: a certificate that is
 * valid until this evening must not be displayed as "0 days left" next to the
 * warning that says it is still usable. An expired one is 0, never a negative
 * number — the page has no sign to draw for that.
 */
constexpr int64_t certDaysLeft(int64_t notAfter, int64_t now)
{
    if (certExpired(notAfter, now)) return 0;
    return (notAfter - now + time::TimeMath::kSecondsPerDay - 1) /
           time::TimeMath::kSecondsPerDay;
}

/**
 * @brief True when @p name is usable both as a CN and as the DNS name of a SAN.
 *
 * Letters, digits, `-` and `.`, no empty label, no leading/trailing dot or
 * dash. Deliberately stricter than the DNS grammar allows: the name ends up in
 * a certificate, and a refused name with a clear message beats a certificate
 * nobody can validate.
 */
inline bool isValidHostName(const std::string& name)
{
    if (name.empty() || name.size() > kMaxHostNameLen) return false;
    if (name.front() == '.' || name.back() == '.' ||
        name.front() == '-' || name.back() == '-') {
        return false;
    }
    bool previousWasDot = false;
    for (const char c : name) {
        if (c == '.') {
            if (previousWasDot) return false;      // an empty label: "a..b"
            previousWasDot = true;
            continue;
        }
        previousWasDot = false;
        const bool isDigit = c >= '0' && c <= '9';
        const bool isLetter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!isDigit && !isLetter && c != '-') return false;
    }
    return true;
}

/**
 * @brief Dotted-decimal IPv4 → the four bytes the SAN extension needs.
 *
 * mbedTLS writes a subject alternative name as raw bytes, so an IP address must
 * be converted here rather than stored as text. Leading zeros are rejected
 * ("01.2.3.4"): browsers and the operating systems disagree about their meaning,
 * so accepting them would create certificates that only some clients accept.
 *
 * @param[in]  text Dotted-decimal address.
 * @param[out] out  Exactly @ref kIpv4Bytes bytes on success; untouched otherwise.
 * @return true when @p text is a valid IPv4 address.
 */
inline bool parseIpv4(const std::string& text, uint8_t out[kIpv4Bytes])
{
    if (text.empty()) return false;
    uint8_t bytes[kIpv4Bytes] = {0, 0, 0, 0};
    int filled = 0;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool atEnd = (i == text.size());
        if (!atEnd && text[i] != '.') continue;

        const size_t len = i - start;
        if (len == 0 || len > kMaxIpv4PartDigits) return false;
        if (len > 1 && text[start] == '0') return false;
        int value = 0;
        for (size_t k = 0; k < len; ++k) {
            const char c = text[start + k];
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        if (value > kMaxIpv4Octet) return false;
        if (filled >= static_cast<int>(kIpv4Bytes)) return false;
        bytes[filled++] = static_cast<uint8_t>(value);
        start = i + 1;
    }
    if (filled != static_cast<int>(kIpv4Bytes)) return false;
    for (size_t k = 0; k < kIpv4Bytes; ++k) out[k] = bytes[k];
    return true;
}

/** @brief Dotted-decimal text of an address, for the page and for log lines. */
inline std::string ipv4Text(const uint8_t ip[kIpv4Bytes])
{
    char buf[kIpv4TextLen];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u",
                  static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
                  static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]));
    return std::string(buf);
}

/** @brief `CN=<name>` as mbedTLS wants the subject and issuer name. */
inline std::string commonName(const std::string& hostName)
{
    return "CN=" + hostName;
}

/** @brief Everything one generated certificate is built from. */
struct CertRequest {
    /** @brief CN of the subject and issuer, and the DNS name of the SAN. */
    std::string hostName;
    /** @brief Address put into the SAN as raw bytes (already parsed). */
    uint8_t ip[kIpv4Bytes] = {0, 0, 0, 0};
    /** @brief Start of validity, Unix seconds UTC. */
    int64_t notBeforeEpoch = 0;
    /** @brief End of validity, Unix seconds UTC. */
    int64_t notAfterEpoch = 0;
};

/**
 * @brief Builds one self-signed ECDSA P-256 certificate together with its key.
 *
 * The class owns the mbedTLS and PSA objects of one generation attempt, so a
 * failed attempt releases them through the destructor — the calling code has no
 * cleanup path to forget. Generating twice on the same object is allowed: the
 * previous attempt is released first.
 *
 * This header stays free of mbedTLS types (the contexts live behind @ref Impl),
 * so the certificate rules above can be compiled and tested on the host, like
 * `storage::PathUtil` and `time::TimeMath` (rule 23).
 */
class CertificateBuilder {
public:
    CertificateBuilder();
    ~CertificateBuilder();

    CertificateBuilder(const CertificateBuilder&) = delete;
    CertificateBuilder& operator=(const CertificateBuilder&) = delete;

    /**
     * @brief Generate a new key and a self-signed certificate for @p request.
     *
     * @param[in]  request  Names, address and validity of the certificate.
     * @param[out] certPem  The certificate in PEM, ready to be written to disk.
     * @param[out] keyPem   The private key in PEM, ready to be written to disk.
     * @param[out] detail   Human-readable reason of a failure ("" on success).
     * @return true when both PEM blocks were produced.
     */
    bool build(const CertRequest& request, std::string& certPem, std::string& keyPem,
               std::string* detail = nullptr);

private:
    /** @brief Release whatever a previous attempt left behind (idempotent). */
    void reset();
    /** @brief Generate the key pair and render its private key as PEM. */
    bool generateKey(std::string& keyPem, std::string* detail);
    /** @brief Fill and sign the certificate. */
    bool writeCertificate(const CertRequest& request, std::string& certPem,
                          std::string* detail);

    /**
     * @brief The mbedTLS and PSA objects of one attempt.
     *
     * Declared here and defined in the `.cpp` so this header does not pull in
     * mbedTLS — the rules of the file are used by the host test.
     */
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace security
} // namespace dhcp

#endif // DHCP_SECURITY_CERTIFICATEBUILDER_H
