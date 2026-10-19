#include "AtomicFile.h"

#include <cerrno>
#include <cstdio>

using namespace std;

namespace dhcp {
namespace core {
namespace atomicfile {

string partPath(const string& path)
{
    return path + ".part";
}

bool readable(const string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    fclose(f);
    return true;
}

bool publish(const string& path)
{
    const string part = partPath(path);
    if (!readable(part)) return false;

    // FatFS refuses a rename onto an existing name, so the old version goes first.
    // `::remove` (not `std::remove`): the algorithm with that name is a different
    // function, and this file means the C library's.
    if (::remove(path.c_str()) != 0 && errno != ENOENT) {
        // The destination could not be removed: publishing over it would fail
        // anyway, and the old file is intact — leave both and report.
        return false;
    }
    if (::rename(part.c_str(), path.c_str()) != 0) return false;
    return true;
}

void recover(const string& path)
{
    // Nothing to do when the destination is there: it is either the previous
    // complete version (the crash came before the rename) or the new one.
    if (readable(path)) return;
    const string part = partPath(path);
    if (!readable(part)) return;
    ::rename(part.c_str(), path.c_str());
}

} // namespace atomicfile
} // namespace core
} // namespace dhcp
