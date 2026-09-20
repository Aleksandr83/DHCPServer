#ifndef DHCP_SECURITY_CERTSTORE_H
#define DHCP_SECURITY_CERTSTORE_H

#include <cstdint>
#include <string>

#include "CertificateBuilder.h"
#include "storage/IFileSystem.h"
#include "storage/PathUtil.h"

namespace dhcp {
namespace security {

/** @brief Which volume holds the pair of certificate files. */
enum class CertStorage : uint8_t {
    /** @brief `/fat` — the internal FAT of the device (same volume as `cache.dat`). */
    Internal = 0,
    /** @brief `/sdcard` — the external microSD card. */
    SdCard = 1,
};

// ─── Where the pair lives ───────────────────────────────────────────────────
// The folder and the file names are the same on both volumes; only the mount
// point differs, so switching storage is a move of two files and never a change
// of the paths inside the certificate itself. Every value is named (rule 39)
// because the page, the API and the docs all quote them.

/** @brief How many volumes a pair can live on (the index stored in NVS). */
constexpr uint8_t kCertStorageCount = 2;
/** @brief Name of the folder inside the volume. */
constexpr const char* kCertFolderName = "certs";
/** @brief The folder, volume-relative. */
constexpr const char* kCertFolderPath = "/certs";
/** @brief Name of the certificate file. */
constexpr const char* kCertFileName = "server.crt";
/** @brief Name of the private key file. */
constexpr const char* kKeyFileName = "server.key";
/** @brief The certificate, volume-relative. */
constexpr const char* kCertFilePath = "/certs/server.crt";
/** @brief The private key, volume-relative. */
constexpr const char* kKeyFilePath = "/certs/server.key";
/** @brief Mount point of the internal volume (`FatFileSystem` in `main.cpp`). */
constexpr const char* kInternalMountPoint = "/fat";
/** @brief Mount point of the card (`SdFileSystem` in `main.cpp`). */
constexpr const char* kSdMountPoint = "/sdcard";
/** @brief Directory mode, the same the file explorer uses on the data volume. */
constexpr int kFolderMode = 0777;
/**
 * @brief A pair larger than this belongs on the card (operator decision, 20.09.2026).
 *
 * A self-signed P-256 pair is a couple of kilobytes, so in practice the pair
 * always fits the internal volume — the rule exists so that a later, larger
 * certificate is not squeezed into the flash partition that holds the data.
 */
constexpr uint64_t kInternalPreferredMaxBytes = 1024 * 1024;

/** @brief The index stored in NVS for @p storage (0 or 1). */
constexpr uint8_t certStorageIndex(CertStorage storage)
{
    return static_cast<uint8_t>(storage);
}

/**
 * @brief The volume behind an index read from NVS.
 *
 * Anything that is not the card means the internal volume: a device that once
 * stored another value must still start with a usable pair rather than refuse
 * to serve its certificate.
 */
constexpr CertStorage certStorageFromIndex(uint8_t index)
{
    return index == certStorageIndex(CertStorage::SdCard) ? CertStorage::SdCard
                                                          : CertStorage::Internal;
}

/** @brief Machine-readable name of @p storage: the JSON value and the dictionary key. */
constexpr const char* certStorageName(CertStorage storage)
{
    return storage == CertStorage::SdCard ? "sdcard" : "internal";
}

/**
 * @brief The volume behind the name a client sent (stage 160).
 *
 * Only the two names of @ref certStorageName are accepted. A name from the page
 * is a request, and one that was not understood has to be refused: a typo read
 * as "internal" the way a stale NVS index is (see @ref certStorageFromIndex)
 * would move the pair to another volume while telling the caller it worked.
 *
 * @param[in]  name The name received in the request body.
 * @param[out] out  The volume, written only when the name is known.
 * @return false when the name is not one of the two.
 */
inline bool certStorageFromName(const std::string& name, CertStorage& out)
{
    if (name == certStorageName(CertStorage::Internal)) {
        out = CertStorage::Internal;
        return true;
    }
    if (name == certStorageName(CertStorage::SdCard)) {
        out = CertStorage::SdCard;
        return true;
    }
    return false;
}

/** @brief Mount point of the volume behind @p storage. */
constexpr const char* certMountPoint(CertStorage storage)
{
    return storage == CertStorage::SdCard ? kSdMountPoint : kInternalMountPoint;
}

/** @brief Preferred volume for a pair of @p bytes (see @ref kInternalPreferredMaxBytes). */
constexpr CertStorage preferredStorageForSize(uint64_t bytes)
{
    return bytes >= kInternalPreferredMaxBytes ? CertStorage::SdCard
                                               : CertStorage::Internal;
}

/** @brief Absolute path of the folder that holds the pair on @p storage. */
inline std::string certFolderPathIn(CertStorage storage)
{
    return storage::PathUtil::join(certMountPoint(storage), kCertFolderPath);
}

/** @brief Absolute path of the certificate on @p storage. */
inline std::string certFilePathIn(CertStorage storage)
{
    return storage::PathUtil::join(certMountPoint(storage), kCertFilePath);
}

/** @brief Absolute path of the private key on @p storage. */
inline std::string keyFilePathIn(CertStorage storage)
{
    return storage::PathUtil::join(certMountPoint(storage), kKeyFilePath);
}

/** @brief What the selected volume currently holds. */
struct CertInfo {
    /** @brief The volume is mounted, so the pair can be read or written. */
    bool available = false;
    /** @brief Both files of the pair are there. */
    bool present = false;
    /** @brief The certificate file parsed. */
    bool valid = false;
    /** @brief Validity has ended (the certificate must be generated again). */
    bool expired = false;
    /** @brief Validity has not started yet (the device clock is behind). */
    bool notYetValid = false;
    /** @brief Less than @ref kExpiryWarnSec is left — the page warns about it. */
    bool expiringSoon = false;
    /** @brief Start of validity, Unix seconds UTC (0 when unknown). */
    int64_t notBeforeEpoch = 0;
    /** @brief End of validity, Unix seconds UTC (0 when unknown). */
    int64_t notAfterEpoch = 0;
    /** @brief Size of the certificate file in bytes. */
    uint32_t certBytes = 0;
    /** @brief Size of the key file in bytes. */
    uint32_t keyBytes = 0;
    /** @brief Subject of the parsed certificate, e.g. `CN=dhcpserver.local`. */
    std::string subject;
    /** @brief Subject alternative names of the parsed certificate, comma separated. */
    std::string sans;
    /** @brief Absolute path of the certificate file. */
    std::string path;
    /** @brief Why the pair is not usable ("" when everything asked for is fine). */
    std::string error;
};

/**
 * @brief Why the HTTPS server may or may not run with the stored pair.
 *
 * One enum instead of a sentence built where the answer is needed: the page
 * translates the name, the log prints it, and the decision itself is a pure
 * function that the host test checks.
 */
enum class CertStatus : uint8_t {
    /** @brief The pair is there, readable and inside its validity. */
    Ready = 0,
    /** @brief This build has no volume for certificates at all. */
    NoStore = 1,
    /** @brief The chosen volume is not mounted (no card, partition missing). */
    StorageUnavailable = 2,
    /** @brief The certificate or its key is not there. */
    NoPair = 3,
    /** @brief Both files are there but the certificate does not parse. */
    Unreadable = 4,
    /** @brief The validity has ended. */
    Expired = 5,
    /** @brief The validity has not started yet — the device clock is behind. */
    NotYetValid = 6,
};

/**
 * @brief Decide whether a pair in this state can serve HTTPS.
 *
 * The order is the order of the answers an operator needs: an unmounted volume
 * is not "no certificate", a missing file is not "broken file", and an expired
 * certificate is not a broken one. Without that order the page would send the
 * operator to generate a certificate when the real problem is an unplugged card.
 */
constexpr CertStatus certStatus(const CertInfo& info)
{
    if (!info.available) return CertStatus::StorageUnavailable;
    if (!info.present) return CertStatus::NoPair;
    if (!info.valid) return CertStatus::Unreadable;
    if (info.expired) return CertStatus::Expired;
    if (info.notYetValid) return CertStatus::NotYetValid;
    return CertStatus::Ready;
}

/** @brief True when the stored pair can serve HTTPS. */
constexpr bool certUsable(CertStatus status)
{
    return status == CertStatus::Ready;
}

/** @brief Machine-readable name of @p status: the JSON value and the dictionary key. */
constexpr const char* certStatusName(CertStatus status)
{
    switch (status) {
    case CertStatus::Ready:              return "ready";
    case CertStatus::NoStore:            return "no_store";
    case CertStatus::StorageUnavailable: return "storage_unavailable";
    case CertStatus::NoPair:             return "no_certificate";
    case CertStatus::Unreadable:         return "unreadable";
    case CertStatus::Expired:            return "expired";
    case CertStatus::NotYetValid:        return "not_yet_valid";
    }
    return "unknown";
}

/**
 * @brief Stores the HTTPS certificate pair and reports its state.
 *
 * The pair is two files — `server.crt` and `server.key` — inside `certs` on one
 * of the two volumes of the device (internal flash or the microSD card). The
 * volume is a setting, not a compile-time choice, so this class never mentions a
 * mount point of its own: it asks the `storage::IFileSystem` objects `main.cpp`
 * registered (rule 7), which is also why the file names and the paths are pure
 * functions that the host test can check without a board.
 *
 * What the class deliberately does **not** do is let the private key out: the
 * key is read only into the caller's string for the TLS server, and the API on
 * top of this class has no way to hand it to a client (operator decision: the
 * key is never downloadable).
 */
class CertStore {
public:
    /**
     * @param[in] internal The internal volume (`/fat`).
     * @param[in] sd       The card volume (`/sdcard`); the caller owns both and
     *                     keeps them alive for the whole life of the store.
     */
    CertStore(storage::IFileSystem& internal, storage::IFileSystem& sd);

    CertStore(const CertStore&) = delete;
    CertStore& operator=(const CertStore&) = delete;

    // ─── Selection ───

    /** @brief Choose the volume the pair lives on (the NVS value decides on boot). */
    void setStorage(CertStorage storage) { storage_ = storage; }
    /** @brief The chosen volume. */
    CertStorage storage() const { return storage_; }
    /** @brief The chosen volume object. */
    storage::IFileSystem& volume() const { return volumeOf(storage_); }
    /** @brief Mount point of the chosen volume. */
    const char* mountPoint() const { return certMountPoint(storage_); }
    /** @brief Absolute path of the certificate of the chosen volume. */
    std::string certPath() const { return certFilePathIn(storage_); }
    /** @brief Absolute path of the private key of the chosen volume. */
    std::string keyPath() const { return keyFilePathIn(storage_); }
    /** @brief Absolute path of the folder of the chosen volume. */
    std::string folderPath() const { return certFolderPathIn(storage_); }

    // ─── State ───

    /**
     * @brief Is the chosen volume usable right now?
     *
     * A missing card or an unmounted partition is reported as "not available"
     * instead of being silently replaced by the other volume: the operator chose
     * this one, and a pair that quietly moves elsewhere is worse than an honest
     * "the card is not there".
     */
    bool available(std::string* detail = nullptr) const;

    /** @brief Are both files of the pair in @p storage (checked without switching)? */
    bool pairPresentIn(CertStorage storage) const;

    /** @brief Read the state of the chosen volume: presence, dates, expiry. */
    CertInfo info(std::string* detail = nullptr) const;

    // ─── Actions ───

    /** @brief Create the `certs` folder of the chosen volume (idempotent). */
    bool ensureFolder(std::string* detail = nullptr);

    /**
     * @brief Generate a new pair, replacing whatever was there.
     *
     * Refused while the device clock is not set: the validity of a certificate
     * is a statement about time, and one dated 1970 would be rejected by every
     * browser — better a clear message than a useless file.
     *
     * @param[in]  hostName CN and DNS name of the SAN (@ref isValidHostName).
     * @param[in]  ip       IPv4 address for the SAN, dotted decimal.
     * @param[out] detail   Human-readable reason of a failure.
     * @param[in]  years    Validity in years, one of @ref kValidityYearChoices
     *                      (@ref kValidityYears when the caller has no opinion).
     */
    bool generate(const std::string& hostName, const std::string& ip,
                  std::string* detail = nullptr, int years = kValidityYears);

    /**
     * @brief Read the pair for the TLS server.
     *
     * The private key leaves the store only through this call, and only inside
     * the firmware — the REST layer never passes it on (operator decision).
     */
    bool load(std::string& certPem, std::string& keyPem, std::string* detail = nullptr) const;

    /**
     * @brief Read the certificate file alone (the download of the page, stage 160).
     *
     * Separate from @ref load on purpose: this path never opens the key at all,
     * so "the key is never downloadable" (operator decision) is a property of
     * the code, not a promise about the handler that calls it.
     *
     * @param[out] certPem The PEM text of the certificate.
     * @param[out] detail  Human-readable reason of a failure.
     * @return false when the volume is not available or the file is missing.
     */
    bool readCertificate(std::string& certPem, std::string* detail = nullptr) const;

    /** @brief Delete both files of the pair from the chosen volume. */
    bool erase(std::string* detail = nullptr);

    /** @brief Copy the pair to @p target (used when the storage setting changes). */
    bool copyTo(CertStorage target, std::string* detail = nullptr);

private:
    /** @brief The volume object behind @p storage. */
    storage::IFileSystem& volumeOf(CertStorage storage) const;
    /** @brief Create the folder on @p storage. */
    bool ensureFolderIn(CertStorage storage, std::string* detail);
    /** @brief Delete the two files on @p storage. */
    bool eraseFiles(CertStorage storage, std::string* detail) const;
    /** @brief Read a whole file into memory. */
    bool readFile(const std::string& path, std::string& out, std::string* detail) const;
    /** @brief Write a whole file, replacing it. */
    bool writeFile(const std::string& path, const std::string& text,
                   std::string* detail) const;
    /** @brief Write the pair into @p storage, removing it again if anything fails. */
    bool writePairIn(CertStorage storage, const std::string& certPem,
                     const std::string& keyPem, std::string* detail);

    storage::IFileSystem* internal_;
    storage::IFileSystem* sd_;
    CertStorage storage_ = CertStorage::Internal;
};

} // namespace security
} // namespace dhcp

#endif // DHCP_SECURITY_CERTSTORE_H
