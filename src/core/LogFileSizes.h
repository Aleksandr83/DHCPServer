#ifndef DHCP_CORE_LOGFILESIZES_H
#define DHCP_CORE_LOGFILESIZES_H

#include <cstddef>
#include <cstdint>

#include "LogFileFormat.h"

namespace dhcp {
namespace core {

/**
 * @brief The sizes a file log can be given, and what a stored value means
 *        (stage 204).
 *
 * The operator asked for a "log file size" setting with three values - 64 KB (the
 * default), 128 KB and 256 KB - and a value like that reaches the writer from
 * three directions that can each be wrong in its own way: the settings page, an
 * NVS entry written before the setting existed, and a settings file imported from
 * somewhere else. It does not stay harmless when it is wrong: the cap decides
 * both whether a diagnosis survives a restart (a cap of nothing rotates on every
 * line, and the file ends up holding ten lines) and whether the log can fill the
 * volume it lives on (a cap of a gigabyte is a file nobody can send anywhere).
 *
 * So the set is **closed** - three sizes, no free number - and this header is the
 * one place that turns a stored value into one of them: a value that is offered
 * is kept as it is, and everything else (absent, zero, negative, a plausible size
 * nobody offers, a number past the largest) becomes the default. The default of
 * the combobox is the writer's own default cap, so a device that has never been
 * given a size rotates exactly where the page says it does.
 *
 * Free of ESP-IDF on purpose - host-tested in `test/test_logfilesizes.cpp` - and
 * used by the config, the settings pages' REST endpoints and the settings import
 * alike, which is what keeps those three from disagreeing.
 */

/** @brief The default, spelled as the default cap of a writer. */
constexpr size_t kLogFileSize64Kb = kLogFileDefaultMaxBytes;

/** @brief The middle size the operator offered. */
constexpr size_t kLogFileSize128Kb = 128u * 1024u;

/** @brief The largest size the operator offered. */
constexpr size_t kLogFileSize256Kb = 256u * 1024u;

/** @brief How many sizes the combobox holds. */
constexpr size_t kLogFileSizeCount = 3;

/** @brief The sizes, ascending, so the combobox reads as a scale. */
constexpr size_t kLogFileSizes[kLogFileSizeCount] = {
    kLogFileSize64Kb,
    kLogFileSize128Kb,
    kLogFileSize256Kb,
};

/** @brief Is @p bytes one of the sizes the operator can choose? */
inline bool logFileSizeValid(size_t bytes)
{
    for (const size_t size : kLogFileSizes) {
        if (bytes == size) return true;
    }
    return false;
}

/**
 * @brief What a stored value means: one of the offered sizes, or the default.
 *
 * The comparison is made as a signed number, so an entry that was never written
 * (`0`), a negative one left by a damaged read and a value that overflowed when
 * it was written all land on the default instead of being read as a huge cap.
 */
inline size_t logFileSizeOrDefault(int64_t stored)
{
    for (const size_t size : kLogFileSizes) {
        if (stored == static_cast<int64_t>(size)) return size;
    }
    return kLogFileSize64Kb;
}

/** @brief The combobox entry that shows @p bytes (the default entry when unknown). */
inline size_t logFileSizeIndex(size_t bytes)
{
    for (size_t i = 0; i < kLogFileSizeCount; ++i) {
        if (kLogFileSizes[i] == bytes) return i;
    }
    return 0;
}

} // namespace core
} // namespace dhcp

#endif // DHCP_CORE_LOGFILESIZES_H
