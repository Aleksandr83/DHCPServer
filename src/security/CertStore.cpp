#include "CertStore.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

#include "CertErrorText.h"

#include "esp_log.h"

#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"

using namespace std;

namespace dhcp {
namespace security {

namespace {

const char* TAG = "CertStore";

// Rule 39: the sizes this file works with.

/** @brief Read window of a PEM file; a pair is a couple of kilobytes at most. */
constexpr size_t kReadChunkBytes = 1024;
/** @brief Low five bits of a context-specific ASN.1 tag carry the SAN type (RFC 5280). */
constexpr int kSanTypeMask = 0x1F;
/** @brief Room for the subject text of a parsed certificate. */
constexpr size_t kSubjectTextBytes = 256;

void setDetail(string* detail, const string& text)
{
    if (detail != nullptr) *detail = text;
}

/** @brief errno as text, for the `detail` out-parameter. */
string errnoText()
{
    return string(strerror(errno));
}

bool fileExists(const string& path)
{
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

bool isFolder(const string& path)
{
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) != 0;
}

/**
 * @brief Unix seconds of a parsed X.509 time.
 *
 * @return false when the date is outside the range the project's calendar
 *         covers (1970..2100) — a certificate from the year 3000 is reported as
 *         "cannot read the dates" rather than silently turned into 1970.
 */
bool epochOf(const mbedtls_x509_time& time, int64_t& out)
{
    if (!time::TimeMath::isValidDate(time.year, time.mon, time.day)) return false;
    if (!time::TimeMath::isValidTime(time.hour, time.min, time.sec)) return false;
    time::DateTime dt;
    dt.year = time.year;
    dt.month = time.mon;
    dt.day = time.day;
    dt.hour = time.hour;
    dt.minute = time.min;
    dt.second = time.sec;
    out = static_cast<int64_t>(time::TimeMath::toUnixSec(dt));
    return true;
}

/** @brief Subject of a parsed certificate as text (`CN=dhcpserver.local`). */
string subjectText(const mbedtls_x509_crt& crt)
{
    char buffer[kSubjectTextBytes];
    buffer[0] = '\0';
    mbedtls_x509_dn_gets(buffer, sizeof buffer, &crt.subject);
    return string(buffer);
}

/**
 * @brief Subject alternative names of a parsed certificate as text.
 *
 * mbedTLS keeps them as a list of raw extension values, the same shape its own
 * checks walk (`x509_crt_check_san_*`), so the value is read here directly:
 * an IP address is four bytes and is rendered as an address, a DNS name is text.
 * Other SAN types are skipped — this store writes only these two.
 */
string sanText(const mbedtls_x509_crt& crt)
{
    string out;
    for (const mbedtls_x509_sequence* cur = &crt.subject_alt_names;
         cur != nullptr; cur = cur->next) {
        const int type = static_cast<int>(cur->buf.tag) & kSanTypeMask;
        string value;
        if (type == MBEDTLS_X509_SAN_IP_ADDRESS) {
            if (cur->buf.len != kIpv4Bytes) continue;
            value = ipv4Text(cur->buf.p);
        } else if (type == MBEDTLS_X509_SAN_DNS_NAME) {
            value.assign(reinterpret_cast<const char*>(cur->buf.p), cur->buf.len);
        } else {
            continue;
        }
        if (!out.empty()) out += ", ";
        out += value;
    }
    return out;
}

} // namespace

CertStore::CertStore(storage::IFileSystem& internal, storage::IFileSystem& sd)
    : internal_(&internal), sd_(&sd)
{
}

storage::IFileSystem& CertStore::volumeOf(CertStorage storage) const
{
    return storage == CertStorage::SdCard ? *sd_ : *internal_;
}

bool CertStore::available(string* detail) const
{
    const storage::IFileSystem& volume = volumeOf(storage_);
    if (volume.isMounted()) return true;

    const string reason = volume.lastError();
    setDetail(detail, reason.empty()
                          ? string("the volume ") + mountPoint() + " is not available"
                          : reason);
    return false;
}

bool CertStore::pairPresentIn(CertStorage storage) const
{
    if (!volumeOf(storage).isMounted()) return false;
    return fileExists(certFilePathIn(storage)) && fileExists(keyFilePathIn(storage));
}

CertInfo CertStore::info(string* detail) const
{
    CertInfo out;
    out.path = certPath();

    string reason;
    if (!available(&reason)) {
        out.error = reason;
        setDetail(detail, reason);
        return out;
    }
    out.available = true;

    string certPem;
    if (!readFile(out.path, certPem, &reason)) {
        out.error = reason;
        setDetail(detail, reason);
        return out;
    }
    out.certBytes = static_cast<uint32_t>(certPem.size());

    string keyPem;
    if (!readFile(keyPath(), keyPem, &reason)) {
        out.error = reason;
        setDetail(detail, reason);
        return out;
    }
    out.keyBytes = static_cast<uint32_t>(keyPem.size());
    out.present = true;

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    // The length includes the terminator: that is how mbedTLS is told the input
    // is a PEM block rather than DER.
    const int ret = mbedtls_x509_crt_parse(
        &crt, reinterpret_cast<const unsigned char*>(certPem.c_str()), certPem.size() + 1);
    if (ret != 0) {
        out.error = "the certificate cannot be read: " + mbedErrorText(ret);
        mbedtls_x509_crt_free(&crt);
        setDetail(detail, out.error);
        return out;
    }

    if (!epochOf(crt.valid_from, out.notBeforeEpoch) ||
        !epochOf(crt.valid_to, out.notAfterEpoch)) {
        out.error = "the validity dates of the certificate are outside 1970..2100";
        mbedtls_x509_crt_free(&crt);
        setDetail(detail, out.error);
        return out;
    }

    out.valid = true;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    out.expired = certExpired(out.notAfterEpoch, now);
    out.notYetValid = certNotYetValid(out.notBeforeEpoch, now);
    out.expiringSoon = certExpiresSoon(out.notAfterEpoch, now);
    out.subject = subjectText(crt);
    out.sans = sanText(crt);
    mbedtls_x509_crt_free(&crt);
    return out;
}

bool CertStore::ensureFolder(string* detail)
{
    return ensureFolderIn(storage_, detail);
}

bool CertStore::erase(string* detail)
{
    if (!available(detail)) return false;
    return eraseFiles(storage_, detail);
}

bool CertStore::generate(const string& hostName, const string& ip,
                         string* detail, int years)
{
    if (!available(detail)) return false;
    if (!isValidHostName(hostName)) {
        setDetail(detail, "\"" + hostName + "\" is not a usable certificate name");
        return false;
    }

    uint8_t ipBytes[kIpv4Bytes];
    if (!parseIpv4(ip, ipBytes)) {
        setDetail(detail, "\"" + ip + "\" is not an IPv4 address");
        return false;
    }

    // The periods are a table, not a range: the operator picks one of the
    // offered ones, and a value from anywhere else (a stale page, a hand-written
    // request) is refused with the list spelled out.
    if (!certValidityYearsAllowed(years)) {
        setDetail(detail, "the validity period must be " + certValidityYearChoicesText() +
                              " years");
        return false;
    }

    // A certificate is a statement about time. Without a clock the dates would be
    // 1970, every browser would reject the result and the operator would think
    // the feature is broken — so this is refused with the reason spelled out.
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (now < kClockSetSinceEpoch) {
        setDetail(detail, "the device clock is not set yet: the certificate would be dated 1970");
        return false;
    }

    int64_t notAfter = 0;
    if (!certNotAfterEpoch(now, years, notAfter)) {
        setDetail(detail, "the device clock is too far ahead for a valid certificate");
        return false;
    }

    if (!ensureFolder(detail)) return false;

    CertRequest request;
    request.hostName = hostName;
    memcpy(request.ip, ipBytes, kIpv4Bytes);
    request.notBeforeEpoch = now - kNotBeforeBackdateSec;
    request.notAfterEpoch = notAfter;

    CertificateBuilder builder;
    string certPem;
    string keyPem;
    if (!builder.build(request, certPem, keyPem, detail)) return false;
    return writePairIn(storage_, certPem, keyPem, detail);
}

bool CertStore::load(string& certPem, string& keyPem, string* detail) const
{
    certPem.clear();
    keyPem.clear();
    if (!available(detail)) return false;
    if (!readFile(certPath(), certPem, detail)) return false;
    if (!readFile(keyPath(), keyPem, detail)) {
        certPem.clear();               // never hand out half of the pair
        return false;
    }
    return true;
}

bool CertStore::readCertificate(string& certPem, string* detail) const
{
    certPem.clear();
    if (!available(detail)) return false;
    return readFile(certPath(), certPem, detail);
}

bool CertStore::copyTo(CertStorage target, string* detail)
{
    if (target == storage_) return true;
    if (!available(detail)) return false;

    const storage::IFileSystem& volume = volumeOf(target);
    if (!volume.isMounted()) {
        const string reason = volume.lastError();
        setDetail(detail, reason.empty()
                              ? string("the volume ") + certMountPoint(target) +
                                    " is not available"
                              : reason);
        return false;
    }
    if (!ensureFolderIn(target, detail)) return false;

    string certPem;
    string keyPem;
    if (!load(certPem, keyPem, detail)) return false;
    if (!writePairIn(target, certPem, keyPem, detail)) return false;

    ESP_LOGI(TAG, "copied the certificate pair to %s", certMountPoint(target));
    return true;
}

bool CertStore::ensureFolderIn(CertStorage storage, string* detail)
{
    const storage::IFileSystem& volume = volumeOf(storage);
    if (!volume.isMounted()) {
        const string reason = volume.lastError();
        setDetail(detail, reason.empty()
                              ? string("the volume ") + certMountPoint(storage) +
                                    " is not available"
                              : reason);
        return false;
    }

    const string folder = certFolderPathIn(storage);
    if (isFolder(folder)) return true;
    if (fileExists(folder)) {
        setDetail(detail, folder + " exists and is not a folder");
        return false;
    }
    if (::mkdir(folder.c_str(), static_cast<mode_t>(kFolderMode)) != 0 && errno != EEXIST) {
        setDetail(detail, "cannot create " + folder + ": " + errnoText());
        return false;
    }
    return true;
}

bool CertStore::eraseFiles(CertStorage storage, string* detail) const
{
    // Removing a file that is not there is not a failure: the caller asked for
    // "no pair on this volume", and that is what it gets.
    bool ok = true;
    const string paths[] = {certFilePathIn(storage), keyFilePathIn(storage)};
    for (const string& path : paths) {
        if (remove(path.c_str()) != 0 && errno != ENOENT) {
            setDetail(detail, "cannot delete " + path + ": " + errnoText());
            ok = false;
        }
    }
    return ok;
}

bool CertStore::writePairIn(CertStorage storage, const string& certPem,
                            const string& keyPem, string* detail)
{
    // Replace, never half-replace: the old pair goes first and a failure while
    // writing removes what was already written, so the volume never ends up
    // holding a certificate without its key (or the other way round).
    eraseFiles(storage, nullptr);
    if (!writeFile(certFilePathIn(storage), certPem, detail)) {
        eraseFiles(storage, nullptr);
        return false;
    }
    if (!writeFile(keyFilePathIn(storage), keyPem, detail)) {
        eraseFiles(storage, nullptr);
        return false;
    }
    ESP_LOGI(TAG, "wrote the certificate pair to %s",
             certFilePathIn(storage).c_str());
    return true;
}

bool CertStore::readFile(const string& path, string& out,
                         string* detail) const
{
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        setDetail(detail, "cannot read " + path + ": " + errnoText());
        return false;
    }

    out.clear();
    char buffer[kReadChunkBytes];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof buffer, file)) > 0) {
        out.append(buffer, read);
    }
    const bool ok = ferror(file) == 0;
    fclose(file);

    if (!ok) {
        setDetail(detail, "error while reading " + path);
        out.clear();
        return false;
    }
    return true;
}

bool CertStore::writeFile(const string& path, const string& text,
                          string* detail) const
{
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        setDetail(detail, "cannot write " + path + ": " + errnoText());
        return false;
    }

    const size_t written = fwrite(text.data(), 1, text.size(), file);
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;

    if (written != text.size() || !flushed || !closed) {
        setDetail(detail, "error while writing " + path);
        return false;
    }
    return true;
}

} // namespace security
} // namespace dhcp
