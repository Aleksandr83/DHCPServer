#include "IFileManager.h"

namespace dhcp {
namespace files {

// The mapping from a domain status to its HTTP code and its English message.
//
// It used to live next to `FileManager` in `FileManager.cpp`, which is not
// host-buildable (FreeRTOS, esp_timer, the VFS). The transfer engine and the
// file-explorer JSON builders both need the *message* of a status, and both are
// host-tested, so the table moved into a file of its own: no ESP-IDF include, no
// I/O, just the two functions and their switch statements.

int httpStatusFor(FileStatus status)
{
    switch (status) {
        case FileStatus::Ok:            return 200;
        case FileStatus::InvalidPath:   return 400;
        case FileStatus::Unsupported:   return 400;
        case FileStatus::NotFound:      return 404;
        case FileStatus::NotMounted:    return 409;
        case FileStatus::AlreadyExists: return 409;
        case FileStatus::NotEmpty:      return 409;
        case FileStatus::Conflict:      return 409;
        case FileStatus::Busy:          return 409;
        case FileStatus::TooLarge:      return 413;
        case FileStatus::NotText:       return 415;
        case FileStatus::NoSpace:       return 507;
        case FileStatus::IoError:       return 500;
    }
    return 500;
}

const char* messageFor(FileStatus status)
{
    switch (status) {
        case FileStatus::Ok:            return "ok";
        case FileStatus::InvalidPath:   return "invalid path";
        case FileStatus::NotFound:      return "not found";
        case FileStatus::NotMounted:    return "volume is not mounted";
        case FileStatus::AlreadyExists: return "already exists";
        case FileStatus::NotEmpty:      return "directory is not empty";
        case FileStatus::Conflict:      return "file changed on the volume";
        case FileStatus::Busy:          return "the volume is busy with another operation";
        case FileStatus::TooLarge:      return "file is too large for the editor";
        case FileStatus::NotText:       return "file is not a text file";
        case FileStatus::NoSpace:       return "not enough free space on the volume";
        case FileStatus::IoError:       return "filesystem error";
        case FileStatus::Unsupported:   return "operation not supported for this volume";
    }
    return "error";
}

} // namespace files
} // namespace dhcp
