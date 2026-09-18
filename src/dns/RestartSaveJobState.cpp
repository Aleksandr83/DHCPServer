#include "RestartSaveJobState.h"

namespace dhcp {
namespace dns {

RestartSaveJobState::StartResult RestartSaveJobState::request(bool enabled)
{
    if (!enabled) {
        // The switch is off. Nothing runs, and — because the page may still be
        // polling — the verdict now says exactly that instead of leaving the
        // outcome of an earlier run standing as if it were today's.
        verdict_ = Verdict::Skipped;
        detail_.clear();
        return StartResult::Skipped;
    }

    if (busy_) {
        // Single-flight: the caller waits for the job already running. The
        // verdict is not touched — it belongs to that job until it finishes.
        return StartResult::Busy;
    }

    busy_ = true;
    // No inherited luck, and no inherited excuse either: whoever polls while this
    // job runs must not read what the previous one did or why it failed.
    verdict_ = Verdict::None;
    detail_.clear();
    return StartResult::Started;
}

void RestartSaveJobState::finish(Verdict verdict, const std::string& detail)
{
    busy_ = false;
    verdict_ = verdict;
    detail_ = detail;
}

void RestartSaveJobState::abortStart()
{
    busy_ = false;
    verdict_ = Verdict::Failed;
    detail_ = "the job task could not be created";
}

} // namespace dns
} // namespace dhcp
