#ifndef DHCP_DNS_RESTARTSAVEVERIFY_H
#define DHCP_DNS_RESTARTSAVEVERIFY_H

#include <functional>
#include <string>

namespace dhcp {
namespace dns {

/**
 * @brief "Write the file, read it back, and try exactly once more" (stage 169).
 *
 * The operator's rule for the files a restart keeps — the statistics and the
 * cache — is one sentence long: *if the content does not match, save it once
 * more; if it still does not match, ask me*. This class is that sentence, and
 * nothing else, so both files (and both their switches) are governed by one
 * implementation of it:
 *
 *   * an **attempt** writes the file *and* reads it back: `Ok` means the volume
 *     holds what was written, `Mismatch` means it does not, `Failed` means the
 *     write itself did not happen;
 *   * a failed attempt is retried **once** — never twice, however the first one
 *     failed: a card that cannot hold the file will not start holding it because
 *     the device keeps trying, and the operator is waiting for his reboot;
 *   * the **outcome describes the volume as it is now**, which is what the last
 *     attempt found. A mismatch followed by a write that failed leaves no usable
 *     file, so that is `Failed`; the reverse ends in `Mismatch` — the honest
 *     answer in both cases, and the page asks the same question either way;
 *   * every attempt, its result and its reason go to the caller (`Report`): the
 *     operator has no serial console, and the error log is where he reads why.
 *
 * The attempt number (1 or 2) is handed to the attempt itself, because starting
 * the retry from a clean slate is the caller's business: `DnsStatStore` removes
 * the destination first (a leftover file may be the very reason the first write
 * failed), while `cache.dat` is opened with `"wb"` and truncates itself.
 *
 * Deliberately free of ESP-IDF — the rules above are the part this project has
 * broken before (a step that stayed silent, a verdict that lied), so they are
 * checked on the host.
 */
class RestartSaveVerify {
public:
    /** @brief What one attempt found on the volume. */
    enum class AttemptResult {
        Ok,        ///< Written, read back, and the file holds what was written
        Mismatch,  ///< Written, but the file read back is not what was written
        Failed,    ///< The write itself did not happen
    };

    /** @brief What the whole policy ended with (the last attempt's result). */
    enum class Outcome {
        Ok,
        Mismatch,
        Failed,
    };

    /**
     * @brief One attempt. Fills @p why with the device's own words on failure.
     * @param attempt 1 for the first try, 2 for the retry.
     */
    using Attempt = std::function<AttemptResult(int attempt, std::string& why)>;

    /**
     * @brief Told about every attempt, before the next one starts.
     *
     * @param attempt 1 or 2.
     * @param result  What that attempt found.
     * @param why     The reason, empty on success.
     */
    using Report = std::function<void(int attempt, AttemptResult result,
                                      const std::string& why)>;

    /** @brief The save and the one retry the operator allowed — no more. */
    static constexpr int kAttempts = 2;

    /**
     * @brief Run @p attempt, and run it once more unless it succeeded.
     *
     * @param attempt Required; an empty one counts as a failed attempt.
     * @param report  Optional; called after each attempt.
     */
    static Outcome run(const Attempt& attempt, const Report& report);

    /// @brief Short English name of an attempt result (logs, test output).
    static const char* attemptName(AttemptResult result);
    /// @brief Short English name of an outcome (logs, test output).
    static const char* outcomeName(Outcome outcome);
};

} // namespace dns
} // namespace dhcp

#endif // DHCP_DNS_RESTARTSAVEVERIFY_H
