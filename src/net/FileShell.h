#ifndef DHCP_NET_FILESHELL_H
#define DHCP_NET_FILESHELL_H

#include "IFileOperations.h"
#include "FileShellPath.h"

#include "../menu/IConsoleOutput.h"

#include <string>
#include <vector>

namespace dhcp {
namespace net {

/**
 * @file FileShell.h
 * @brief The file commands of the SSH shell (Command over a port).
 *
 * Ten verbs — `ls`, `cd`, `pwd`, `mkdir`, `touch`, `rm`, `mv`, `cp`, `cat`, `df` —
 * plus `clear`. Each one is a handler in a dispatch table (**Command**), and every
 * touch of a volume goes through `IFileOperations` (**Ports & Adapters**), so the
 * whole shell runs in a host test against a fake and the device only supplies the
 * POSIX adapter.
 *
 * The rules the operator chose for this stage:
 *
 *   * **SSH only** — the serial console keeps the commands it had;
 *   * **two volumes, as a sandbox** — `/fat` and `/sdcard`, with `cd` inside a
 *     volume and a refusal to leave it (`FileShellPath`);
 *   * **strict** — `rm` removes a directory only with `-r`, `cp`/`mv` refuse an
 *     existing target unless `-f` is given, and `cat` refuses a file over 64 KB or
 *     one that is not text.
 *
 * Every answer is a line (or several) ending in CR LF: the SSH channel is a raw
 * terminal, and a lone LF staircases on most clients.
 */
class FileShell {
public:
    /// @brief The most a `cat` will print.
    static constexpr uint64_t kCatLimitBytes = 64u * 1024u;

    FileShell(::dhcp::menu::IConsoleOutput& out, IFileOperations& fs);

    /// @brief Is @p word one of this shell's verbs? (`help` and `exit` are not.)
    static bool handles(const std::string& word);

    /// @brief Run one line (no trailing newline). Unknown verbs are not answered.
    void execute(const std::string& line);

    /// @brief The section `help` adds to the console's own.
    void printHelp();

private:
    struct Args {
        bool recursive = false;   ///< `-r` of `rm`
        bool force = false;       ///< `-f` of `cp`/`mv`
        std::vector<std::string> words;
    };

    static Args parseArgs(const std::string& rest);

    void cmdLs(const Args& args);
    void cmdCd(const Args& args);
    void cmdPwd();
    void cmdMkdir(const Args& args);
    void cmdTouch(const Args& args);
    void cmdRm(const Args& args);
    void cmdMv(const Args& args);
    void cmdCp(const Args& args);
    void cmdCat(const Args& args);
    void cmdDf();
    void cmdClear();

    /// @brief `name: message` + CR LF.
    void say(const std::string& text);
    /// @brief The sentence a failed port answer gets, named after the verb.
    void sayFailure(const char* verb, const std::string& path, FileOpStatus status);
    /// @brief Existence through the port's own `attributes` (no separate call).
    bool pathExists(const std::string& path, bool* isDir = nullptr,
                    uint64_t* size = nullptr);

    ::dhcp::menu::IConsoleOutput& out_;
    IFileOperations& fs_;
    FileShellPath path_;
};

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_FILESHELL_H
