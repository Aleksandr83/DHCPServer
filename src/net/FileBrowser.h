#ifndef DHCP_NET_FILEBROWSER_H
#define DHCP_NET_FILEBROWSER_H

#include "FileShellPath.h"
#include "IFileOperations.h"
#include "TuiKey.h"

#include <string>
#include <vector>

namespace dhcp {
namespace net {

/**
 * @file FileBrowser.h
 * @brief The two-pane file screen behind `mc` in the SSH session (stage 225).
 *
 * This class is the **whole screen except the terminal**: it takes keys, keeps two
 * panels, asks a volume through `IFileOperations`, and renders plain text lines of
 * exactly `cols` by `rows`. The SSH session only moves bytes — render, write, read,
 * decode — which is why every rule of the screen can be checked in a host test.
 *
 * The operator's decisions for this stage:
 *
 *   * **two panels**, as in `mc`: the left one and the right one, each with its own
 *     directory and its own volume;
 *   * **operations inside the browser** — F4 new file, F5 copy, F6 rename, F7 new
 *     directory, F8 delete (no confirmation, as agreed);
 *   * **keys**: arrows, Home/End, PgUp/PgDn, Enter, Backspace, `Tab` switches the
 *     **panel**, `Alt+F1`/`Alt+F2` switch the **volume** of the left/right panel,
 *     `F3` views a file, `q` leaves.
 *
 * The paths of both panels obey `FileShellPath` — the same sandbox the commands of
 * the shell use — so the browser cannot reach anything but `/fat` and `/sdcard`.
 */
class FileBrowser {
public:
    /// @brief The most a viewer shows; a larger file is refused, not cut.
    static constexpr uint64_t kViewLimitBytes = 64u * 1024u;

    /// @brief How one piece of the screen is painted.
    ///
    /// The layout knows *what* a cell is, not *how* it looks: the same frame turns
    /// into plain text for a test and into coloured text for a terminal. That is
    /// what keeps the colour from disturbing the width of anything.
    enum class CellStyle {
        Plain,       ///< the bar lines at the bottom
        Pane,        ///< the panel itself: blue, white text
        PaneActive,  ///< the panel the keys go to: blue, bold
        Directory,   ///< a directory inside a panel: cyan
        Selected     ///< the line under the cursor: a bar of its own colour
    };

    /// @brief One run of characters painted in one style.
    struct Piece {
        std::string text;
        CellStyle style = CellStyle::Plain;
    };

    FileBrowser(IFileOperations& fs, int cols, int rows);

    /// @brief One key from the terminal.
    void feed(const TuiKey& key);

    /// @brief The screen as text: exactly `rows()` lines, each `cols()` characters.
    std::vector<std::string> render();

    /// @brief The same screen with the escape sequences a terminal needs.
    ///
    /// The visible characters are unchanged: taking the escapes out gives back
    /// exactly what @ref render returns, which is what the host test checks.
    std::vector<std::string> renderAnsi();

    /// @brief Draw at a new terminal size — the window was resized mid-session.
    void resize(int cols, int rows);

    /// @brief True once the operator asked to leave (`q`, Ctrl+C).
    bool finished() const { return finished_; }

    int cols() const { return cols_; }
    int rows() const { return rows_; }

private:
    /// @brief What the screen is doing right now.
    enum class Mode {
        Browse,   ///< the panels
        Prompt,   ///< a one-line question (F4..F7)
        View      ///< a file shown full-screen (F3)
    };

    /// @brief What a prompt is going to do when it is answered.
    enum class PromptAction {
        None,
        NewFile,
        NewDirectory,
        Rename,
        Copy
    };

    /// @brief One panel: where it stands, what it shows, where the cursor is.
    struct Panel {
        int volume = 0;            ///< index into `FileShellPath::kVolumes`
        std::string dir;           ///< absolute directory
        std::vector<FileEntry> entries;
        std::string parent;        ///< shown as `..`, empty in a volume root
        size_t cursor = 0;
        size_t top = 0;
    };

    Panel& activePanel();
    const Panel& activePanel() const;
    Panel& panel(int index);
    const Panel& panel(int index) const;

    /// @brief Re-read a panel's directory, keeping the cursor on the same name.
    void reload(int index);

    /// @brief The entry under the cursor, or nullptr (`..` and an empty list).
    const FileEntry* selected() const;

    void handleBrowse(const TuiKey& key);
    void handlePrompt(const TuiKey& key);
    void handleView(const TuiKey& key);

    void openPrompt(PromptAction action, const std::string& title,
                    const std::string& text);
    void applyPrompt();

    void moveCursor(int delta);
    void page(int direction);
    void openSelected();
    void goUp();
    void switchVolume(int index);
    void startView();
    void doDelete();

    /// @brief Resolve a typed name inside a panel's directory (sandbox included).
    std::string resolveIn(const Panel& panel, const std::string& typed,
                          std::string* why) const;

    /// @brief The pane width: two of them and the divider fill `cols`.
    int paneWidth() const;

    /// @brief The columns between the panes (one or two, so both panes stay equal).
    int dividerWidth() const;

    /// @brief The whole screen as painted runs, the single source of the layout.
    std::vector<std::vector<Piece>> buildFrame();

    /// @brief The row of the two panel headers.
    std::vector<Piece> headerRow();

    /// @brief List row @p row of both panels.
    std::vector<Piece> listRow(int row);

    /// @brief A full-width line of the bottom bars, painted plainly.
    std::vector<Piece> barRow(const std::string& text);

    /// @brief The divider between the panes, appended to a row in place.
    void appendDivider(std::vector<Piece>& row) const;

    /// @brief The escape sequence that starts a run of @p style.
    static const char* ansiFor(CellStyle style);

    std::string renderHeaderLine(int index) const;
    std::string entryName(const Panel& panel, size_t index) const;
    std::string renderStatus() const;
    std::string renderHints() const;

    /// @brief Pad or cut one cell to exactly `width` characters.
    ///
    /// Control bytes become `?`: a directory on the FAT volume was once left with
    /// names that are not text, and an escape in a name must not reach the terminal
    /// as an escape.
    static std::string fit(const std::string& text, int width);

    IFileOperations& fs_;
    int cols_;
    int rows_;
    Panel panels_[2];
    int active_ = 0;

    Mode mode_ = Mode::Browse;
    PromptAction promptAction_ = PromptAction::None;
    std::string promptTitle_;
    std::string promptText_;
    std::string message_;
    std::vector<std::string> viewLines_;
    size_t viewTop_ = 0;
    std::string viewName_;
    bool finished_ = false;
};

} // namespace net
} // namespace dhcp

#endif // DHCP_NET_FILEBROWSER_H
