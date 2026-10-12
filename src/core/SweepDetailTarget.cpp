#include "SweepDetailTarget.h"

using namespace std;

namespace dhcp {
namespace core {

bool SweepDetailTarget::begin(const string& mountPoint, const string& fileName,
                              size_t keep)
{
    // Opened outside the lock: creating folders and opening a file on a card can
    // take milliseconds, and a line from the sweep must not wait for that.
    auto sink = make_unique<SweepFileLogTarget>();
    if (!sink->open(mountPoint, fileName, keep)) return false;

    lock_guard<mutex> lock(mutex_);
    sink_ = move(sink);   // an earlier sink is closed by its destructor
    return true;
}

void SweepDetailTarget::end()
{
    unique_ptr<SweepFileLogTarget> sink;
    {
        lock_guard<mutex> lock(mutex_);
        sink = move(sink_);
    }
    // Flushed outside the lock: the last lines of a sweep are a card write, and
    // holding the lock through it would stall the writer for no reason.
    if (sink) sink->close();
}

bool SweepDetailTarget::append(LogLevel level, const string& line)
{
    lock_guard<mutex> lock(mutex_);
    if (!sink_) return false;   // no sweep is logging: nothing to write into
    return sink_->append(level, line);
}

bool SweepDetailTarget::active() const
{
    lock_guard<mutex> lock(mutex_);
    return sink_ != nullptr;
}

string SweepDetailTarget::currentFile() const
{
    lock_guard<mutex> lock(mutex_);
    return sink_ ? sink_->description() : string();
}

uint64_t SweepDetailTarget::lines() const
{
    lock_guard<mutex> lock(mutex_);
    return sink_ ? sink_->lines() : 0;
}

} // namespace core
} // namespace dhcp
