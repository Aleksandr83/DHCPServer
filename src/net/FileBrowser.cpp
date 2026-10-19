#include "FileBrowser.h"

#include <algorithm>

using namespace std;

namespace dhcp {
namespace net {

namespace {

/// @brief The columns the screen spends on decoration, so the panes get the rest.
constexpr int kDividerMinWidth = 1;
constexpr int kMarkerWidth = 2;

/// @brief How many keys a prompt accepts; a name is not a document.
constexpr size_t kPromptMaxChars = 120;

/// @brief What a screen can be drawn in: a terminal can be tiny or enormous.
constexpr int kMinCols = 40;
constexpr int kMaxCols = 200;
constexpr int kMinRows = 8;
constexpr int kMaxRows = 60;

int clampCols(int cols)
{
    return cols < kMinCols ? kMinCols : (cols > kMaxCols ? kMaxCols : cols);
}

int clampRows(int rows)
{
    return rows < kMinRows ? kMinRows : (rows > kMaxRows ? kMaxRows : rows);
}

bool lessByName(const FileEntry& a, const FileEntry& b)
{
    return a.name < b.name;
}

} // namespace

constexpr uint64_t FileBrowser::kViewLimitBytes;

FileBrowser::FileBrowser(IFileOperations& fs, int cols, int rows)
    : fs_(fs)
    , cols_(clampCols(cols))
    , rows_(clampRows(rows))
{
    for (int i = 0; i < 2; i++) {
        panels_[i].volume = 0;
        panels_[i].dir = FileShellPath::kVolumes[0];
        reload(i);
    }
}

void FileBrowser::resize(int cols, int rows)
{
    cols_ = clampCols(cols);
    rows_ = clampRows(rows);
    for (int i = 0; i < 2; i++) {
        // The cursor stays on its name; only the window around it changed, and the
        // next frame scrolls the list to bring it back into view.
        if (panels_[i].top > panels_[i].cursor) panels_[i].top = panels_[i].cursor;
    }
}

FileBrowser::Panel& FileBrowser::panel(int index)
{
    return panels_[index == 0 ? 0 : 1];
}

const FileBrowser::Panel& FileBrowser::panel(int index) const
{
    return panels_[index == 0 ? 0 : 1];
}

FileBrowser::Panel& FileBrowser::activePanel()
{
    return panels_[active_];
}

const FileBrowser::Panel& FileBrowser::activePanel() const
{
    return panels_[active_];
}

int FileBrowser::paneWidth() const
{
    // Two panes of the **same** width with a divider between them. A screen of odd
    // width leaves one column over, and it goes to the divider — panels that differ
    // by a column read as a broken screen.
    return (cols_ - kDividerMinWidth) / 2;
}

int FileBrowser::dividerWidth() const
{
    return cols_ - 2 * paneWidth();
}

string FileBrowser::fit(const string& text, int width)
{
    if (width <= 0) return string();
    string out;
    out.reserve(static_cast<size_t>(width));
    for (char c : text) {
        if (static_cast<int>(out.size()) >= width) break;
        const unsigned char byte = static_cast<unsigned char>(c);
        out.push_back(byte < 0x20 || byte == 0x7f ? '?' : c);
    }
    while (static_cast<int>(out.size()) < width) out.push_back(' ');
    return out;
}

void FileBrowser::reload(int index)
{
    Panel& p = panel(index);
    // The name the cursor was on, so a reload (after an operation or a return from a
    // directory) does not throw the operator back to the top of the list.
    string keep;
    const FileEntry* current = nullptr;
    if (!p.entries.empty() && p.cursor < p.entries.size()) {
        current = &p.entries[p.cursor];
        keep = current->name;
    } else if (p.cursor == 0 && !p.parent.empty()) {
        keep = "..";
    }

    p.entries.clear();
    p.parent.clear();
    const string volume = FileShellPath::kVolumes[p.volume];
    if (p.dir != volume) {
        const size_t slash = p.dir.rfind('/');
        if (slash != string::npos && slash >= string(volume).size()) {
            p.parent = p.dir.substr(0, slash);
        } else {
            p.parent = volume;
        }
    }

    vector<FileEntry> listed;
    if (fs_.list(p.dir, listed) == FileOpStatus::Ok) {
        sort(listed.begin(), listed.end(), lessByName);
        p.entries = move(listed);
    }

    // The cursor follows the name it was on; failing that it stays inside the list.
    size_t wanted = 0;
    bool found = false;
    if (!keep.empty()) {
        if (keep == ".." && !p.parent.empty()) {
            wanted = 0;
            found = true;
        } else {
            for (size_t i = 0; i < p.entries.size(); i++) {
                if (p.entries[i].name == keep) { wanted = i; found = true; break; }
            }
        }
    }
    const size_t total = p.entries.size() + (p.parent.empty() ? 0 : 1);
    if (!found) wanted = 0;
    if (total == 0) {
        p.cursor = 0;
        p.top = 0;
    } else {
        p.cursor = min(wanted, total - 1);
    }
}

const FileEntry* FileBrowser::selected() const
{
    const Panel& p = panels_[active_];
    if (!p.parent.empty()) {
        if (p.cursor == 0) return nullptr;
        const size_t index = p.cursor - 1;
        return index < p.entries.size() ? &p.entries[index] : nullptr;
    }
    return p.cursor < p.entries.size() ? &p.entries[p.cursor] : nullptr;
}

void FileBrowser::moveCursor(int delta)
{
    Panel& p = activePanel();
    const size_t total = p.entries.size() + (p.parent.empty() ? 0 : 1);
    if (total == 0) return;
    const long wanted = static_cast<long>(p.cursor) + delta;
    p.cursor = static_cast<size_t>(max(0L, min(wanted, static_cast<long>(total) - 1)));
}

void FileBrowser::page(int direction)
{
    const int listRows = rows_ - 3;
    moveCursor(direction * listRows);
}

void FileBrowser::openSelected()
{
    Panel& p = activePanel();
    if (!p.parent.empty() && p.cursor == 0) {
        goUp();
        return;
    }
    const FileEntry* entry = selected();
    if (entry == nullptr) return;

    string why;
    const string path = resolveIn(p, entry->name, &why);
    if (path.empty()) {
        message_ = "error: " + why;
        return;
    }
    if (entry->isDir) {
        p.dir = path;
        p.cursor = 0;
        p.top = 0;
        reload(active_);
        message_.clear();
        return;
    }
    startView();
}

void FileBrowser::goUp()
{
    Panel& p = activePanel();
    const string volume = FileShellPath::kVolumes[p.volume];
    if (p.dir == volume) {
        // The volume root is the ceiling: saying so is better than standing still
        // without a word.
        message_ = "error: a path cannot leave its volume";
        return;
    }
    const string leaving = p.dir;
    p.dir = volume;
    reload(active_);
    // The directory just left is where the cursor goes, as in every file manager.
    for (size_t i = 0; i < p.entries.size(); i++) {
        string why;
        const string path = resolveIn(p, p.entries[i].name, &why);
        if (!path.empty() && path == leaving) {
            p.cursor = i + (p.parent.empty() ? 0 : 1);
            break;
        }
    }
    message_.clear();
}

void FileBrowser::switchVolume(int index)
{
    Panel& p = panel(index);
    p.volume = (p.volume + 1) % 2;
    p.dir = FileShellPath::kVolumes[p.volume];
    p.cursor = 0;
    p.top = 0;
    reload(index);
    message_.clear();
}

string FileBrowser::resolveIn(const Panel& p, const string& typed, string* why) const
{
    FileShellPath policy;
    if (!policy.changeTo(p.dir)) return string();
    return policy.resolve(typed, why);
}

void FileBrowser::openPrompt(PromptAction action, const string& title,
                             const string& text)
{
    mode_ = Mode::Prompt;
    promptAction_ = action;
    promptTitle_ = title;
    promptText_ = text;
    message_.clear();
}

void FileBrowser::applyPrompt()
{
    const PromptAction action = promptAction_;
    const string typed = promptText_;
    const string title = promptTitle_;
    mode_ = Mode::Browse;
    promptAction_ = PromptAction::None;
    promptTitle_.clear();
    promptText_.clear();

    if (typed.empty()) {
        message_ = "error: a name is empty";
        return;
    }

    Panel& p = activePanel();
    string why;
    const string target = resolveIn(p, typed, &why);
    if (target.empty()) {
        message_ = "error: " + why;
        return;
    }

    FileOpStatus status = FileOpStatus::Ok;
    switch (action) {
        case PromptAction::NewFile: {
            bool created = false;
            status = fs_.touch(target, created);
            break;
        }
        case PromptAction::NewDirectory:
            status = fs_.makeDir(target);
            break;
        case PromptAction::Rename: {
            const FileEntry* entry = selected();
            if (entry == nullptr) {
                message_ = "error: nothing is selected";
                return;
            }
            string fromWhy;
            const string from = resolveIn(p, entry->name, &fromWhy);
            status = from.empty() ? FileOpStatus::NotFound : fs_.rename(from, target);
            break;
        }
        case PromptAction::Copy: {
            const FileEntry* entry = selected();
            if (entry == nullptr) {
                message_ = "error: nothing is selected";
                return;
            }
            string fromWhy;
            const string from = resolveIn(p, entry->name, &fromWhy);
            status = from.empty() ? FileOpStatus::NotFound : fs_.copy(from, target);
            break;
        }
        case PromptAction::None:
            break;
    }

    if (status != FileOpStatus::Ok) {
        message_ = "error: " + title + " failed";
        return;
    }
    reload(active_);
    message_ = title + " done";
}

void FileBrowser::doDelete()
{
    const FileEntry* entry = selected();
    if (entry == nullptr) {
        message_ = "error: nothing is selected";
        return;
    }
    Panel& p = activePanel();
    string why;
    const string path = resolveIn(p, entry->name, &why);
    if (path.empty()) {
        message_ = "error: " + why;
        return;
    }
    const FileOpStatus status = entry->isDir ? fs_.removeTree(path) : fs_.removeFile(path);
    if (status != FileOpStatus::Ok) {
        message_ = "error: delete failed";
        return;
    }
    reload(active_);
    message_ = "deleted " + entry->name;
}

void FileBrowser::startView()
{
    const FileEntry* entry = selected();
    if (entry == nullptr) return;

    Panel& p = activePanel();
    string why;
    const string path = resolveIn(p, entry->name, &why);
    if (path.empty()) {
        message_ = "error: " + why;
        return;
    }

    string text;
    bool binary = false;
    const FileOpStatus status = fs_.readText(path, kViewLimitBytes, text, binary);
    if (status == FileOpStatus::NoSpace) {
        message_ = "error: larger than " + to_string(kViewLimitBytes) + " bytes: " + entry->name;
        return;
    }
    if (status != FileOpStatus::Ok) {
        message_ = "error: cannot read " + entry->name;
        return;
    }
    if (binary) {
        message_ = "error: not a text file: " + entry->name;
        return;
    }

    viewLines_.clear();
    string line;
    for (char c : text) {
        if (c == '\n') {
            viewLines_.push_back(line);
            line.clear();
        } else if (c != '\r') {
            line += c;
        }
    }
    if (!line.empty()) viewLines_.push_back(line);

    viewName_ = entry->name;
    viewTop_ = 0;
    mode_ = Mode::View;
    message_.clear();
}

void FileBrowser::handleBrowse(const TuiKey& key)
{
    switch (key.kind) {
        case TuiKeyKind::Up:       moveCursor(-1); break;
        case TuiKeyKind::Down:     moveCursor(1); break;
        case TuiKeyKind::PageUp:   page(-1); break;
        case TuiKeyKind::PageDown: page(1); break;
        case TuiKeyKind::Home:     moveCursor(-static_cast<int>(rows_) * 1000); break;
        case TuiKeyKind::End:      moveCursor(static_cast<int>(rows_) * 1000); break;
        case TuiKeyKind::Enter:    openSelected(); break;
        case TuiKeyKind::Backspace: goUp(); break;
        case TuiKeyKind::Tab:      active_ = 1 - active_; message_.clear(); break;
        case TuiKeyKind::AltF1:    switchVolume(0); break;
        case TuiKeyKind::AltF2:    switchVolume(1); break;
        case TuiKeyKind::F3:       startView(); break;
        case TuiKeyKind::F4:       openPrompt(PromptAction::NewFile, "New file: ", ""); break;
        case TuiKeyKind::F5: {
            const FileEntry* entry = selected();
            if (entry == nullptr) {
                message_ = "error: nothing is selected";
                break;
            }
            // Pre-filled with the other panel's directory: that is the copy this
            // screen exists for.
            const Panel& other = panel(1 - active_);
            openPrompt(PromptAction::Copy, "Copy to: ",
                       other.dir + "/" + entry->name);
            break;
        }
        case TuiKeyKind::F6: {
            const FileEntry* entry = selected();
            if (entry == nullptr) {
                message_ = "error: nothing is selected";
                break;
            }
            openPrompt(PromptAction::Rename, "Rename to: ", entry->name);
            break;
        }
        case TuiKeyKind::F7: openPrompt(PromptAction::NewDirectory, "New directory: ", ""); break;
        case TuiKeyKind::F8: doDelete(); break;
        // F10 leaves, as in mc. A letter does not: in a file manager every letter
        // belongs to the list, and `q` was the one the operator had to remember.
        case TuiKeyKind::F10:      finished_ = true; break;
        case TuiKeyKind::CtrlC:    finished_ = true; break;
        default: break;
    }
}

void FileBrowser::handlePrompt(const TuiKey& key)
{
    switch (key.kind) {
        case TuiKeyKind::Enter:
            applyPrompt();
            return;
        case TuiKeyKind::Escape:
            mode_ = Mode::Browse;
            promptAction_ = PromptAction::None;
            promptTitle_.clear();
            promptText_.clear();
            message_ = "cancelled";
            return;
        case TuiKeyKind::Backspace:
            if (!promptText_.empty()) promptText_.pop_back();
            return;
        case TuiKeyKind::F10:
        case TuiKeyKind::CtrlC:
            finished_ = true;
            return;
        case TuiKeyKind::Char:
            if (promptText_.size() < kPromptMaxChars) promptText_ += key.ch;
            return;
        default:
            return;
    }
}

void FileBrowser::handleView(const TuiKey& key)
{
    // The viewer draws its content in the list rows, so "the end" means the last
    // line is on screen — not that it scrolled past it.
    const size_t listRows = rows_ > 3 ? static_cast<size_t>(rows_ - 3) : 1;
    const size_t last = viewLines_.empty() ? 0 : viewLines_.size() - 1;
    const size_t maxTop = last >= listRows - 1 ? last - (listRows - 1) : 0;
    switch (key.kind) {
        case TuiKeyKind::Up:
            if (viewTop_ > 0) --viewTop_;
            return;
        case TuiKeyKind::Down:
            if (viewTop_ < maxTop) ++viewTop_;
            return;
        case TuiKeyKind::PageUp:
            viewTop_ = viewTop_ > listRows ? viewTop_ - listRows : 0;
            return;
        case TuiKeyKind::PageDown:
            viewTop_ = min(viewTop_ + listRows, maxTop);
            return;
        case TuiKeyKind::Home:
            viewTop_ = 0;
            return;
        case TuiKeyKind::End:
            viewTop_ = maxTop;
            return;
        case TuiKeyKind::F3:
        case TuiKeyKind::Escape:
            mode_ = Mode::Browse;
            viewLines_.clear();
            return;
        case TuiKeyKind::F10:
        case TuiKeyKind::CtrlC:
            finished_ = true;
            return;
        case TuiKeyKind::Char:
            if (key.ch == 'q') {
                mode_ = Mode::Browse;
                viewLines_.clear();
            }
            return;
        default:
            return;
    }
}

void FileBrowser::feed(const TuiKey& key)
{
    if (finished_) return;
    switch (mode_) {
        case Mode::Browse: handleBrowse(key); break;
        case Mode::Prompt: handlePrompt(key); break;
        case Mode::View:   handleView(key); break;
    }
}

string FileBrowser::renderHeaderLine(int index) const
{
    const Panel& p = panel(index);
    string text = (index == active_ ? "* " : "  ");
    text += FileShellPath::kVolumes[p.volume];
    const string volume = FileShellPath::kVolumes[p.volume];
    if (p.dir != volume) text += p.dir.substr(volume.size());
    return text;
}

string FileBrowser::entryName(const Panel& p, size_t index) const
{
    if (!p.parent.empty() && index == 0) return "../";
    const size_t at = index - (p.parent.empty() ? 0 : 1);
    if (at >= p.entries.size()) return string();
    const FileEntry& entry = p.entries[at];
    return entry.isDir ? entry.name + "/" : entry.name;
}

string FileBrowser::renderStatus() const
{
    if (mode_ == Mode::Prompt) return promptTitle_ + promptText_;
    if (mode_ == Mode::View) return viewName_;
    if (!message_.empty()) return message_;
    const size_t total = activePanel().entries.size();
    if (total == 0) return "empty";
    return to_string(total) + " entries";
}

string FileBrowser::renderHints() const
{
    if (mode_ == Mode::View) return "Up/Down PgUp/PgDn Home/End  F3 or q  back  F10 Quit";
    if (mode_ == Mode::Prompt) return "Enter apply  Esc cancel  F10 Quit";
    // Short enough to fit an 80-column terminal: a hint that is cut off names
    // nothing, and the key it would have named is the one that leaves the screen.
    return "F3 View  F4 New  F5 Copy  F6 Ren  F7 Mkdir  F8 Del  Tab  Alt+F1/2  F10 Quit";
}

vector<string> FileBrowser::render()
{
    vector<string> lines;
    for (const vector<Piece>& row : buildFrame()) {
        string text;
        for (const Piece& piece : row) text += piece.text;
        lines.push_back(move(text));
    }
    return lines;
}

vector<string> FileBrowser::renderAnsi()
{
    vector<string> lines;
    for (const vector<Piece>& row : buildFrame()) {
        string text;
        CellStyle current = CellStyle::Plain;
        for (const Piece& piece : row) {
            if (piece.style != current) {
                text += ansiFor(piece.style);
                current = piece.style;
            }
            text += piece.text;
        }
        // Never leave a colour switched on for the shell prompt that follows.
        if (current != CellStyle::Plain) text += ansiFor(CellStyle::Plain);
        lines.push_back(move(text));
    }
    return lines;
}

const char* FileBrowser::ansiFor(CellStyle style)
{
    switch (style) {
        case CellStyle::Pane:       return "\x1b[44;37m";    // blue panel, white text
        case CellStyle::PaneActive: return "\x1b[44;1;37m";  // ... and bold when it has the keys
        case CellStyle::Directory:  return "\x1b[44;1;36m";  // a directory is cyan
        case CellStyle::Selected:   return "\x1b[46;30m";    // the cursor row: black on cyan
        case CellStyle::Plain:      return "\x1b[0m";
    }
    return "\x1b[0m";
}

void FileBrowser::appendDivider(vector<FileBrowser::Piece>& row) const
{
    string text = "|";
    text.append(static_cast<size_t>(dividerWidth() - 1), ' ');
    row.push_back({ text, CellStyle::Pane });
}

vector<FileBrowser::Piece> FileBrowser::barRow(const string& text)
{
    return { { fit(text, cols_), CellStyle::Plain } };
}

vector<FileBrowser::Piece> FileBrowser::headerRow()
{
    const int width = paneWidth();
    vector<Piece> row;
    for (int side = 0; side < 2; side++) {
        const CellStyle style = side == active_ ? CellStyle::PaneActive : CellStyle::Pane;
        row.push_back({ fit(renderHeaderLine(side), width), style });
        if (side == 0) appendDivider(row);
    }
    return row;
}

vector<FileBrowser::Piece> FileBrowser::listRow(int row)
{
    const int width = paneWidth();
    const int listRows = rows_ - 3;
    vector<Piece> line;

    for (int side = 0; side < 2; side++) {
        Panel& p = panel(side);
        const size_t total = p.entries.size() + (p.parent.empty() ? 0 : 1);
        if (side == active_ && total > 0) {
            if (p.cursor < p.top) p.top = p.cursor;
            const size_t lastVisible = p.top + static_cast<size_t>(listRows);
            if (p.cursor >= lastVisible && listRows > 0) {
                p.top = p.cursor - static_cast<size_t>(listRows) + 1;
            }
        }

        const size_t index = p.top + static_cast<size_t>(row);
        string cell;
        CellStyle style = CellStyle::Pane;
        if (index < total) {
            const bool isSelected = (side == active_ && index == p.cursor);
            const bool isParent = !p.parent.empty() && index == 0;
            const FileEntry* entry = isParent
                ? nullptr
                : &p.entries[index - (p.parent.empty() ? 0 : 1)];
            cell = (isSelected ? "> " : "  ") + entryName(p, index);
            if (isSelected) {
                style = CellStyle::Selected;
            } else if (isParent || (entry != nullptr && entry->isDir)) {
                style = CellStyle::Directory;
            }
        }
        line.push_back({ fit(cell, width), style });
        if (side == 0) appendDivider(line);
    }

    return line;
}

vector<vector<FileBrowser::Piece>> FileBrowser::buildFrame()
{
    vector<vector<Piece>> frame;
    frame.reserve(static_cast<size_t>(rows_));

    const int listRows = rows_ - 3;

    if (mode_ == Mode::View) {
        frame.push_back({ { fit("* " + viewName_, cols_), CellStyle::PaneActive } });
        const size_t first = viewTop_;
        for (int row = 0; row < listRows; row++) {
            const size_t at = first + static_cast<size_t>(row);
            frame.push_back({ { fit(at < viewLines_.size() ? viewLines_[at] : string(), cols_),
                                CellStyle::Pane } });
        }
        frame.push_back(barRow(renderStatus()));
        frame.push_back(barRow(renderHints()));
        return frame;
    }

    frame.push_back(headerRow());
    for (int row = 0; row < listRows; row++) frame.push_back(listRow(row));
    frame.push_back(barRow(renderStatus()));
    frame.push_back(barRow(renderHints()));
    return frame;
}

} // namespace net
} // namespace dhcp
