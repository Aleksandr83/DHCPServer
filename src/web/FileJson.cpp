#include "FileJson.h"

namespace dhcp {
namespace web {

std::string FileJson::volumeArray(const std::vector<::dhcp::storage::VolumeInfo>& volumes)
{
    std::string list = "[";
    for (size_t i = 0; i < volumes.size(); ++i) {
        if (i != 0) list += ',';
        const auto& vol = volumes[i];

        JsonWriter w;
        w.str("id", vol.id);
        w.str("mount_point", vol.mountPoint);
        w.boolean("mounted", vol.mounted);
        w.boolean("present", vol.present);
        w.num("total_bytes", static_cast<int64_t>(vol.totalBytes));
        w.num("free_bytes", static_cast<int64_t>(vol.freeBytes));
        w.str("error", vol.error);
        list += w.toString();
    }
    list += ']';
    return list;
}

std::string FileJson::volumes(bool enabled,
                              const std::vector<::dhcp::storage::VolumeInfo>& volumes)
{
    JsonWriter out;
    out.boolean("enabled", enabled);
    out.literal("volumes", volumeArray(volumes));
    return out.toString();
}

std::string FileJson::entryObject(const ::dhcp::files::FileEntry& entry)
{
    JsonWriter w;
    w.str("name", entry.name);
    w.boolean("is_dir", entry.isDir);
    w.num("size", static_cast<int64_t>(entry.size));
    w.num("mtime", static_cast<int64_t>(entry.mtime));
    return w.toString();
}

std::string FileJson::entryArray(const std::vector<::dhcp::files::FileEntry>& entries)
{
    std::string out = "[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) out += ',';
        out += entryObject(entries[i]);
    }
    out += ']';
    return out;
}

std::string FileJson::list(const ListPayload& payload)
{
    JsonWriter w;
    w.str("volume", payload.volume);
    w.str("path", payload.path);
    w.boolean("mounted", payload.mounted);
    w.num("total_bytes", static_cast<int64_t>(payload.totalBytes));
    w.num("free_bytes", static_cast<int64_t>(payload.freeBytes));
    w.boolean("truncated", payload.truncated);
    w.literal("entries", entryArray(payload.entries));
    return w.toString();
}

std::string FileJson::text(const TextPayload& payload)
{
    JsonWriter w;
    w.str("volume", payload.volume);
    w.str("path", payload.path);
    w.num("size", static_cast<int64_t>(payload.size));
    w.num("mtime", static_cast<int64_t>(payload.mtime));
    w.boolean("truncated", payload.truncated);
    w.str("text", payload.text);
    return w.toString();
}

std::string FileJson::settings(const SettingsPayload& payload)
{
    JsonWriter w;
    w.boolean("enabled", payload.enabled);
    w.boolean("allow_own_subnet", payload.allowOwnSubnet);
    w.str("subnet_address", payload.subnetAddress);
    w.str("subnet_mask", payload.subnetMask);
    w.boolean("filter_active", payload.filterActive);
    w.num("blocked_count", static_cast<int64_t>(payload.blockedCount));
    return w.toString();
}

std::string FileJson::checkErrorObject(const ::dhcp::files::CheckError& error)
{
    JsonWriter w;
    w.str("path", error.path);
    w.str("detail", error.detail);
    return w.toString();
}

std::string FileJson::checkErrorArray(const std::vector<::dhcp::files::CheckError>& errors)
{
    std::string out = "[";
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i != 0) out += ',';
        out += checkErrorObject(errors[i]);
    }
    out += ']';
    return out;
}

std::string FileJson::transfer(const ::dhcp::files::TransferReport& report)
{
    JsonWriter out;
    out.str("phase", ::dhcp::files::transferPhaseName(report.phase));
    out.boolean("busy", report.busy);
    out.boolean("finished", report.finished);
    out.boolean("cancelled", report.cancelled);
    out.boolean("instant", report.instant);
    out.str("op", report.op == ::dhcp::files::TransferOp::Move ? "move" : "copy");
    out.str("src_volume", report.srcVolume);
    out.str("dst_volume", report.dstVolume);
    out.str("dst_path", report.dstPath);
    out.str("current", report.current);
    out.num("done_bytes", static_cast<int64_t>(report.doneBytes));
    out.num("total_bytes", static_cast<int64_t>(report.totalBytes));
    out.num("needed_bytes", static_cast<int64_t>(report.neededBytes));
    out.num("free_bytes", static_cast<int64_t>(report.freeBytes));
    out.num("files_done", report.filesDone);
    out.num("files_total", report.filesTotal);
    out.num("dirs_done", report.dirsDone);
    out.num("dirs_total", report.dirsTotal);
    out.num("skipped", report.skipped);
    out.num("failed", report.failed);
    out.num("deleted", report.deleted);
    out.str("error", report.error);
    out.str("error_path", report.errorPath);
    return out.toString();
}

std::string FileJson::nameArray(const std::vector<std::string>& names)
{
    // Objects rather than bare strings: a name may hold any character a FAT
    // volume allows, and JsonWriter is the one place that knows how to escape
    // it — the page reads `conflicts[].name`.
    std::string list = "[";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) list += ',';
        JsonWriter w;
        w.str("name", names[i]);
        list += w.toString();
    }
    list += ']';
    return list;
}

std::string FileJson::transferConflicts(const std::vector<std::string>& names)
{
    JsonWriter out;
    out.str("status", "conflict");
    out.literal("conflicts", nameArray(names));
    return out.toString();
}

std::string FileJson::check(const ::dhcp::files::CheckReport& report)
{
    JsonWriter w;
    w.boolean("busy", report.busy);
    w.boolean("finished", report.finished);
    w.boolean("truncated", report.truncated);
    w.boolean("cancelled", report.cancelled);
    w.str("volume", report.volume);
    w.str("current", report.current);
    w.num("dirs", static_cast<int64_t>(report.dirs));
    w.num("files", static_cast<int64_t>(report.files));
    w.num("bad_entries", static_cast<int64_t>(report.badEntries));
    w.num("bytes_read", static_cast<int64_t>(report.bytes));
    w.num("budget_bytes", static_cast<int64_t>(report.budgetBytes));
    w.literal("errors", checkErrorArray(report.errors));
    return w.toString();
}

std::string FileJson::jobObject(const ::dhcp::core::JobInfo& job)
{
    JsonWriter w;
    w.str("id", job.id);
    w.str("title_key", job.titleKey);
    w.str("arg", job.arg);
    w.str("state", ::dhcp::core::jobStateText(job.state));
    w.num("done", static_cast<int64_t>(job.done));
    w.num("total", static_cast<int64_t>(job.total));
    w.num("percent", job.percent());
    w.str("detail", job.detail);
    w.num("elapsed_ms", static_cast<int64_t>(job.durationMs));
    w.boolean("cancel_requested", job.cancelRequested);
    w.num("repeat_sec", static_cast<int64_t>(job.repeatSec));
    return w.toString();
}

std::string FileJson::jobArray(const std::vector<::dhcp::core::JobInfo>& jobs)
{
    std::string out = "[";
    for (size_t i = 0; i < jobs.size(); ++i) {
        if (i != 0) out += ',';
        out += jobObject(jobs[i]);
    }
    out += ']';
    return out;
}

std::string FileJson::jobs(const std::vector<::dhcp::core::JobInfo>& jobs)
{
    JsonWriter w;
    w.literal("jobs", jobArray(jobs));
    return w.toString();
}

} // namespace web
} // namespace dhcp
