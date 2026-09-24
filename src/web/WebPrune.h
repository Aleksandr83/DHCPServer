#ifndef DHCP_WEB_WEBPRUNE_H
#define DHCP_WEB_WEBPRUNE_H

#include <cstddef>
#include <string>
#include <vector>

namespace dhcp {
namespace web {

/**
 * @brief The rule that keeps the device's web tree equal to an uploaded folder.
 *
 * The interface is updated from a folder, one file per request
 * (`POST /api/web/file`), and that path only ever **writes**: a page dropped
 * from `data/` stayed on the device for good, holding its bytes and answering
 * nothing, because its route had gone away with the firmware (stage 169). The
 * operator put the consequence plainly — the device has to be serviceable from a
 * browser alone, without a cable and without a flash — so the update ends with a
 * **prune**: the page tells the device which files the folder holds, and the
 * device removes whatever else lives on the volume.
 *
 * The comparison lives here, free of ESP-IDF, for one reason: it decides what
 * gets **deleted**, and a decision like that has to be provable on the host.
 * Three rules:
 *
 *   * names are compared after @ref normalise (no leading `/`, no mount prefix,
 *     no trailing `/`) and **case-sensitively** — the volume is case-sensitive,
 *     so `Index.html` is a different file from `index.html`, and the folder's
 *     spelling is the one that wins;
 *   * a name the folder sends has to pass @ref isAcceptableName, which is the
 *     very rule the upload enforces (one definition, used by both): a list that
 *     could describe names the upload cannot create is refused rather than acted
 *     on;
 *   * the answer names the volume's own spelling of every file, so the caller
 *     deletes exactly what it was told about.
 *
 * What it deliberately does **not** do: guess. An empty folder list means "delete
 * everything" to a naive comparison, so the emptiness is the caller's business —
 * the route refuses such a request and this class simply computes it.
 */
class WebPrune {
public:
    /**
     * @brief Longest relative path the upload accepts, in bytes.
     *
     * The upload handler uses this same constant for its own validation, so the
     * list that decides the deletions cannot hold a name the upload would have
     * refused. (SPIFFS's real limit is smaller — 31 characters including the
     * leading slash — and the route-table guard test keeps an eye on that.)
     */
    static constexpr size_t kMaxNameLen = 64;

    /**
     * @brief A name in the shape the comparison uses.
     *
     * Strips a leading `/`, a `spiffs/` mount prefix and trailing `/`, so
     * `/spiffs/pages/x.html`, `spiffs/pages/x.html` and `pages/x.html` are one
     * name. Nothing else is touched: no case folding, no path collapsing.
     */
    static std::string normalise(const std::string& name);

    /**
     * @brief True when @p name passes the upload's own rules.
     *
     * Letters, digits, `.`, `_` and `-` in every segment; no empty segment; no
     * `.` or `..`; no leading `/`; at most @ref kMaxNameLen bytes.
     */
    static bool isAcceptableName(const std::string& name);

    /**
     * @brief Which of @p onDevice the uploaded folder does not hold.
     *
     * @param onDevice Names as the volume reports them (`pages/x.html`).
     * @param uploaded Names the chosen folder holds, in the same shape.
     * @return The device's names, **in the volume's own spelling and order**, so
     *         the caller can delete exactly those. Duplicates in @p uploaded are
     *         harmless; an empty @p uploaded means "everything", which is the
     *         honest answer and the caller's decision to act on.
     */
    static std::vector<std::string> extra(const std::vector<std::string>& onDevice,
                                          const std::vector<std::string>& uploaded);
};

} // namespace web
} // namespace dhcp

#endif // DHCP_WEB_WEBPRUNE_H
