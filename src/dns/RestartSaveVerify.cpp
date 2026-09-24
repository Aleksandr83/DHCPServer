#include "RestartSaveVerify.h"

// Rule 40: a translation unit of its own, so the directive cannot leak.
using namespace std;

namespace dhcp {
namespace dns {

namespace {
/** @brief The outcome a single attempt's result stands for. */
RestartSaveVerify::Outcome outcomeOf(RestartSaveVerify::AttemptResult result)
{
    switch (result) {
        case RestartSaveVerify::AttemptResult::Ok:       return RestartSaveVerify::Outcome::Ok;
        case RestartSaveVerify::AttemptResult::Mismatch: return RestartSaveVerify::Outcome::Mismatch;
        case RestartSaveVerify::AttemptResult::Failed:   return RestartSaveVerify::Outcome::Failed;
    }
    return RestartSaveVerify::Outcome::Failed;
}
} // namespace

const char* RestartSaveVerify::attemptName(AttemptResult result)
{
    switch (result) {
        case AttemptResult::Ok:       return "ok";
        case AttemptResult::Mismatch: return "mismatch";
        case AttemptResult::Failed:   return "failed";
    }
    return "unknown";
}

const char* RestartSaveVerify::outcomeName(Outcome outcome)
{
    switch (outcome) {
        case Outcome::Ok:       return "ok";
        case Outcome::Mismatch: return "mismatch";
        case Outcome::Failed:   return "failed";
    }
    return "unknown";
}

RestartSaveVerify::Outcome RestartSaveVerify::run(const Attempt& attempt, const Report& report)
{
    Outcome last = Outcome::Failed;

    for (int n = 1; n <= kAttempts; n++) {
        string why;
        // An attempt that is not there at all is a failed attempt, not an
        // exception: the caller that forgot to pass one still gets a verdict.
        const AttemptResult result = attempt ? attempt(n, why) : AttemptResult::Failed;

        if (report) report(n, result, why);

        last = outcomeOf(result);
        // Nothing to retry once the volume holds what was written. The retry
        // exists for a file that did not make it, and it is deliberately the
        // only one.
        if (result == AttemptResult::Ok) break;
    }

    return last;
}

} // namespace dns
} // namespace dhcp
