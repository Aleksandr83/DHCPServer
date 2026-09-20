#include "JobRegistry.h"

#include <algorithm>

#include "esp_log.h"

using namespace std;

namespace dhcp {
namespace core {

namespace {
const char* TAG = "JobRegistry";
} // namespace

const char* jobStateText(JobState state)
{
    switch (state) {
        case JobState::Running:   return "running";
        case JobState::Paused:    return "paused";
        case JobState::Done:      return "done";
        case JobState::Failed:    return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

// Rule 39: job progress is reported as a percentage.
constexpr int kPercentScale = 100;

int JobInfo::percent() const
{
    if (total == 0) return -1;
    if (done >= total) return kPercentScale;
    return static_cast<int>((static_cast<uint64_t>(done) * kPercentScale) / total);
}

JobRegistry& JobRegistry::instance()
{
    static JobRegistry registry;
    return registry;
}

chrono::milliseconds JobRegistry::now()
{
    return chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now().time_since_epoch());
}

size_t JobRegistry::slotFor(const string& id)
{
    // The same id is one operation, not two: an upload that resumes, or a second
    // run of the same kind, takes its own entry over.
    for (size_t i = 0; i < kMaxJobs; i++) {
        if (used_[i] && jobs_[i].id == id) return i;
    }
    for (size_t i = 0; i < kMaxJobs; i++) {
        if (!used_[i]) return i;
    }
    // Full: reuse the oldest record that is not running (a finished one is only
    // still there because it repeats, and another operation matters more).
    size_t oldest = kMaxJobs;
    chrono::milliseconds oldestTime = now();
    for (size_t i = 0; i < kMaxJobs; i++) {
        if (jobs_[i].state == JobState::Running) continue;
        if (started_[i] <= oldestTime) {
            oldestTime = started_[i];
            oldest = i;
        }
    }
    return oldest;
}

bool JobRegistry::begin(const string& id, const string& titleKey,
                        const string& arg, uint32_t total, uint32_t repeatSec)
{
    if (id.empty()) return false;

    lock_guard<mutex> lock(mutex_);

    const size_t slot = slotFor(id);
    if (slot >= kMaxJobs) {
        ESP_LOGW(TAG, "job '%s' refused: %u operations are running", id.c_str(),
                 static_cast<unsigned>(kMaxJobs));
        return false;
    }

    JobInfo& job = jobs_[slot];
    job = JobInfo{};
    job.id = id;
    job.titleKey = titleKey;
    job.arg = arg;
    job.state = JobState::Running;
    job.total = total;
    job.repeatSec = repeatSec;
    used_[slot] = true;
    started_[slot] = now();
    finished_[slot] = chrono::milliseconds{0};

    ESP_LOGI(TAG, "job started: %s %s", id.c_str(), arg.c_str());
    return true;
}

void JobRegistry::progress(const string& id, uint32_t done, uint32_t total,
                           const string& detail)
{
    lock_guard<mutex> lock(mutex_);

    for (size_t i = 0; i < kMaxJobs; i++) {
        if (!used_[i] || jobs_[i].id != id) continue;
        jobs_[i].state = JobState::Running;
        jobs_[i].done = done;
        if (total > 0) jobs_[i].total = total;
        if (!detail.empty()) jobs_[i].detail = detail;
        return;
    }

    // Progress without a start means the caller forgot begin(); creating the
    // record here would invent a name and a start time, so it is ignored.
}

void JobRegistry::pause(const string& id, const string& detail)
{
    lock_guard<mutex> lock(mutex_);

    for (size_t i = 0; i < kMaxJobs; i++) {
        if (!used_[i] || jobs_[i].id != id) continue;
        jobs_[i].state = JobState::Paused;
        if (!detail.empty()) jobs_[i].detail = detail;
        ESP_LOGI(TAG, "job paused: %s", id.c_str());
        return;
    }
}

void JobRegistry::finish(const string& id, JobState state, const string& detail)
{
    lock_guard<mutex> lock(mutex_);

    for (size_t i = 0; i < kMaxJobs; i++) {
        if (!used_[i] || jobs_[i].id != id) continue;

        jobs_[i].state = state;
        if (!detail.empty()) jobs_[i].detail = detail;
        if (jobs_[i].total > 0 && state == JobState::Done) {
            jobs_[i].done = jobs_[i].total;
        }
        finished_[i] = now();

        // A one-off operation is gone the moment it ends: the list answers "what
        // is running now". A scheduled repeat keeps its record, so the operator
        // can see that the next run is coming.
        if (jobs_[i].repeatSec == 0) {
            used_[i] = false;
            jobs_[i] = JobInfo{};
            started_[i] = chrono::milliseconds{0};
            finished_[i] = chrono::milliseconds{0};
        }

        ESP_LOGI(TAG, "job finished: %s (%s)", id.c_str(), jobStateText(state));
        return;
    }
}

bool JobRegistry::requestCancel(const string& id)
{
    lock_guard<mutex> lock(mutex_);

    for (size_t i = 0; i < kMaxJobs; i++) {
        if (!used_[i] || jobs_[i].id != id) continue;
        // Only an unfinished operation can be stopped; anything else in the list
        // has already ended (it is still there because it repeats).
        if (jobs_[i].state != JobState::Running &&
            jobs_[i].state != JobState::Paused) {
            return false;
        }
        jobs_[i].cancelRequested = true;
        ESP_LOGW(TAG, "job '%s' asked to stop", id.c_str());
        return true;
    }
    return false;
}

bool JobRegistry::cancelRequested(const string& id) const
{
    lock_guard<mutex> lock(mutex_);

    for (size_t i = 0; i < kMaxJobs; i++) {
        if (used_[i] && jobs_[i].id == id) return jobs_[i].cancelRequested;
    }
    return false;
}

bool JobRegistry::contains(const string& id) const
{
    lock_guard<mutex> lock(mutex_);

    for (size_t i = 0; i < kMaxJobs; i++) {
        if (used_[i] && jobs_[i].id == id) return true;
    }
    return false;
}

vector<JobInfo> JobRegistry::snapshot() const
{
    lock_guard<mutex> lock(mutex_);

    const chrono::milliseconds current = now();
    vector<JobInfo> out;
    out.reserve(kMaxJobs);

    for (size_t i = 0; i < kMaxJobs; i++) {
        if (!used_[i]) continue;
        JobInfo job = jobs_[i];
        const chrono::milliseconds end =
            (job.state == JobState::Running || job.state == JobState::Paused)
                ? current : finished_[i];
        const auto ms = end - started_[i];
        job.durationMs = ms.count() > 0 ? static_cast<uint32_t>(ms.count()) : 0;
        out.push_back(move(job));
    }

    // Newest first: the operation that just started is the one to watch.
    sort(out.begin(), out.end(), [](const JobInfo& a, const JobInfo& b) {
        return a.durationMs < b.durationMs;
    });
    return out;
}

void JobRegistry::clear()
{
    lock_guard<mutex> lock(mutex_);

    for (size_t i = 0; i < kMaxJobs; i++) {
        used_[i] = false;
        jobs_[i] = JobInfo{};
        started_[i] = chrono::milliseconds{0};
        finished_[i] = chrono::milliseconds{0};
    }
}

} // namespace core
} // namespace dhcp
