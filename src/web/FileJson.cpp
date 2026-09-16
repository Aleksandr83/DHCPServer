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

} // namespace web
} // namespace dhcp
