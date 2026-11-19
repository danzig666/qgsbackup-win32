#include "gui.h"
#include "backup_engine.h"
#include "filesystem_utils.h"
#include "localization.h"
#include "resource.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;
namespace qgsbackup {
namespace {
constexpr wchar_t WindowClass[] = L"QgsBackupMainWindow";
enum ControlId { ProjectEdit = 1001, ProjectBrowse, DestinationEdit, DestinationBrowse, TimestampCheck,
                 SkipCheck, VerboseCheck, SkipEditButton, StartButton, CancelButton, OpenButton, ProgressBar, LogEdit,
                 SkipLabel, StatusLabel, StatsLabel };
constexpr UINT MsgProgress = WM_APP + 1, MsgLog = WM_APP + 2, MsgDone = WM_APP + 3;

struct LogMessage { LogLevel level; std::wstring text; };
struct UiState {
    HWND hwnd{}, project{}, destination{}, timestamp{}, skip{}, verbose{}, editSkip{}, start{}, cancel{}, open{}, progress{}, log{}, skipLabel{}, status{}, stats{};
    HFONT font{}; UINT dpi{96}; std::thread worker; std::atomic_bool cancellation{false}; bool running{false}, closeWhenDone{false};
    fs::path completedDestination;
};

const wchar_t* appTitle() { return tr(L"QGIS Project Backup", L"QGIS projektmentés"); }

std::wstring copiedText(std::uint64_t copied, std::uint64_t total) {
    std::wstring text = humanReadableSize(copied);
    if (total) text += L" / " + humanReadableSize(total);
    text += tr(L" copied", L" átmásolva");
    if (total) text += L" (" + std::to_wstring(static_cast<unsigned>(100 * std::min(copied, total) / total)) + L"%)";
    return text;
}

UiState* stateOf(HWND hwnd) { return reinterpret_cast<UiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); }
std::wstring textOf(HWND control) { int n = GetWindowTextLengthW(control); std::wstring value(static_cast<std::size_t>(n) + 1, L'\0'); if (n) GetWindowTextW(control, value.data(), n + 1); value.resize(static_cast<std::size_t>(n)); return value; }
void setFont(HWND control, HFONT font) { SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); }
void place(HWND control, int x, int y, int w, int h) { SetWindowPos(control, nullptr, x, y, w, h, SWP_NOZORDER); }
int scaled(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

HFONT makeUiFont(UINT dpi) {
    return CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void updateFonts(UiState& s, UINT dpi) {
    HFONT replacement = makeUiFont(dpi);
    for (HWND child = GetWindow(s.hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) setFont(child, replacement);
    if (s.font) DeleteObject(s.font);
    s.font = replacement;
    s.dpi = dpi;
}

void appendLog(UiState& s, LogLevel level, const std::wstring& message) {
    const wchar_t* prefix = level == LogLevel::Warning ? tr(L"Warning: ", L"Figyelmeztetés: ") : level == LogLevel::Error ? tr(L"Error: ", L"Hiba: ") : level == LogLevel::Verbose ? tr(L"[verbose] ", L"[részletes] ") : L"";
    std::wstring line = prefix + message + L"\r\n";
    int length = GetWindowTextLengthW(s.log);
    if (length > 200000) { SendMessageW(s.log, EM_SETSEL, 0, 50000); SendMessageW(s.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"")); length = GetWindowTextLengthW(s.log); }
    SendMessageW(s.log, EM_SETSEL, length, length); SendMessageW(s.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void updateSkipLabel(UiState& s) {
    fs::path file = executableDirectory() / L"qgsbackup_skip.txt";
    const auto count = IsDlgButtonChecked(s.hwnd, SkipCheck) == BST_CHECKED ? loadSkipPatterns(file).size() : 0;
    std::wstring label = IsDlgButtonChecked(s.hwnd, SkipCheck) == BST_CHECKED
        ? (fs::exists(file) ? std::to_wstring(count) + tr(L" skip patterns loaded from ", L" kihagyási minta betöltve innen: ") + file.native()
                            : std::wstring(tr(L"Skip file not found; zero patterns will be used", L"A kihagyási lista nem található; nem lesz kihagyott minta")))
        : tr(L"Skip patterns disabled", L"Kihagyási minták kikapcsolva");
    SetWindowTextW(s.skipLabel, label.c_str());
}

struct SkipEditorData { fs::path file; };

bool saveUtf8Text(const fs::path& file, const std::wstring& text, std::wstring& error) {
    fs::path temporary = file; temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    const auto utf8 = wideToUtf8(text);
    if (!out || !out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()))) {
        error = tr(L"Unable to write ", L"Nem sikerült írni: ") + temporary.native(); return false;
    }
    out.close();
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = tr(L"Unable to save ", L"Nem sikerült menteni: ") + file.native() + tr(L" (Windows error ", L" (Windows-hiba: ") + std::to_wstring(GetLastError()) + L")";
        std::error_code ec; fs::remove(temporary, ec); return false;
    }
    return true;
}

INT_PTR CALLBACK skipEditorProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<SkipEditorData*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<SkipEditorData*>(lParam); SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        SetWindowTextW(dialog, tr(L"Edit qgsbackup_skip.txt", L"qgsbackup_skip.txt szerkesztése"));
        SetDlgItemTextW(dialog, IDC_SKIP_HINT, tr(L"One case-insensitive wildcard pattern per line (* and ? are supported).",
                                                  L"Soronként egy minta, kis- és nagybetű nem számít (* és ? használható)."));
        SetDlgItemTextW(dialog, IDOK, tr(L"Save", L"Mentés")); SetDlgItemTextW(dialog, IDCANCEL, tr(L"Cancel", L"Mégse"));
        std::ifstream in(data->file, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(in)), {});
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef && static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf) bytes.erase(0, 3);
        auto text = utf8ToWide(bytes); SetDlgItemTextW(dialog, IDC_SKIP_TEXT, text.c_str());
        SendDlgItemMessageW(dialog, IDC_SKIP_TEXT, EM_SETLIMITTEXT, 1024 * 1024, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            int length = GetWindowTextLengthW(GetDlgItem(dialog, IDC_SKIP_TEXT));
            std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
            GetDlgItemTextW(dialog, IDC_SKIP_TEXT, text.data(), length + 1); text.resize(static_cast<std::size_t>(length));
            std::wstring error;
            if (!saveUtf8Text(data->file, text, error)) { MessageBoxW(dialog, error.c_str(), tr(L"Unable to save skip list", L"Nem sikerült menteni a kihagyási listát"), MB_OK | MB_ICONERROR); return TRUE; }
            EndDialog(dialog, IDOK); return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

void editSkipList(UiState& s) {
    SkipEditorData data{executableDirectory() / L"qgsbackup_skip.txt"};
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SKIP_EDITOR), s.hwnd, skipEditorProc, reinterpret_cast<LPARAM>(&data));
    updateSkipLabel(s);
}

bool validProject(const std::wstring& value) {
    std::error_code ec; fs::path p(value); auto ext = p.extension().native();
    return fs::is_regular_file(p, ec) && !ec && (_wcsicmp(ext.c_str(), L".qgs") == 0 || _wcsicmp(ext.c_str(), L".qgz") == 0);
}
void updateStart(UiState& s) { EnableWindow(s.start, !s.running && validProject(textOf(s.project)) && !textOf(s.destination).empty()); }

void saveSettings(UiState& s) {
    HKEY key{}; if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\QgsBackup", 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    auto dest = textOf(s.destination); RegSetValueExW(key, L"LastDestination", 0, REG_SZ, reinterpret_cast<const BYTE*>(dest.c_str()), static_cast<DWORD>((dest.size() + 1) * sizeof(wchar_t)));
    DWORD timestamp = IsDlgButtonChecked(s.hwnd, TimestampCheck) == BST_CHECKED, skip = IsDlgButtonChecked(s.hwnd, SkipCheck) == BST_CHECKED;
    RegSetValueExW(key, L"Timestamp", 0, REG_DWORD, reinterpret_cast<BYTE*>(&timestamp), sizeof(timestamp));
    RegSetValueExW(key, L"UseSkip", 0, REG_DWORD, reinterpret_cast<BYTE*>(&skip), sizeof(skip)); RegCloseKey(key);
}

void loadSettings(UiState& s) {
    CheckDlgButton(s.hwnd, TimestampCheck, BST_CHECKED); CheckDlgButton(s.hwnd, SkipCheck, BST_CHECKED);
    HKEY key{}; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\QgsBackup", 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    wchar_t dest[32768]{}; DWORD type{}, bytes = sizeof(dest); if (RegQueryValueExW(key, L"LastDestination", nullptr, &type, reinterpret_cast<BYTE*>(dest), &bytes) == ERROR_SUCCESS && type == REG_SZ) SetWindowTextW(s.destination, dest);
    DWORD value{}, size = sizeof(value); if (RegQueryValueExW(key, L"Timestamp", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS) CheckDlgButton(s.hwnd, TimestampCheck, value ? BST_CHECKED : BST_UNCHECKED);
    size = sizeof(value); if (RegQueryValueExW(key, L"UseSkip", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS) CheckDlgButton(s.hwnd, SkipCheck, value ? BST_CHECKED : BST_UNCHECKED);
    RegCloseKey(key);
}

void chooseProject(UiState& s) {
    IFileOpenDialog* dialog{}; if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    COMDLG_FILTERSPEC filters[] = {{tr(L"QGIS projects (*.qgs;*.qgz)", L"QGIS projektek (*.qgs;*.qgz)"), L"*.qgs;*.qgz"}, {tr(L"QGIS project (*.qgs)", L"QGIS projekt (*.qgs)"), L"*.qgs"},
                                   {tr(L"Compressed QGIS project (*.qgz)", L"Tömörített QGIS projekt (*.qgz)"), L"*.qgz"}, {tr(L"All files", L"Minden fájl"), L"*.*"}};
    dialog->SetFileTypes(4, filters); dialog->SetTitle(tr(L"Select a QGIS project", L"Válasszon QGIS projektet"));
    if (SUCCEEDED(dialog->Show(s.hwnd))) { IShellItem* item{}; if (SUCCEEDED(dialog->GetResult(&item))) { PWSTR path{}; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { SetWindowTextW(s.project, path); CoTaskMemFree(path); } item->Release(); } }
    dialog->Release(); updateStart(s);
}

void chooseDestination(UiState& s) {
    IFileOpenDialog* dialog{}; if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options{}; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM); dialog->SetTitle(tr(L"Select the backup destination", L"Válassza ki a mentés célmappáját"));
    if (SUCCEEDED(dialog->Show(s.hwnd))) { IShellItem* item{}; if (SUCCEEDED(dialog->GetResult(&item))) { PWSTR path{}; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { SetWindowTextW(s.destination, path); CoTaskMemFree(path); } item->Release(); } }
    dialog->Release(); updateStart(s);
}

void setRunning(UiState& s, bool running) {
    s.running = running; EnableWindow(s.project, !running); EnableWindow(s.destination, !running); EnableWindow(GetDlgItem(s.hwnd, ProjectBrowse), !running); EnableWindow(GetDlgItem(s.hwnd, DestinationBrowse), !running);
    EnableWindow(s.timestamp, !running); EnableWindow(s.skip, !running); EnableWindow(s.verbose, !running); EnableWindow(s.editSkip, !running); EnableWindow(s.cancel, running); if (!running) updateStart(s); else EnableWindow(s.start, FALSE);
}

void startBackup(UiState& s) {
    auto project = textOf(s.project), destination = textOf(s.destination);
    if (!validProject(project)) { MessageBoxW(s.hwnd, (tr(L"The selected QGIS project does not exist or has an unsupported extension.\r\n\r\n", L"A kiválasztott QGIS projekt nem létezik, vagy nem támogatott a kiterjesztése.\r\n\r\n") + project).c_str(), appTitle(), MB_OK | MB_ICONWARNING); return; }
    if (destination.empty()) { MessageBoxW(s.hwnd, tr(L"Choose a backup destination.", L"Válassza ki a mentés célmappáját."), appTitle(), MB_OK | MB_ICONWARNING); return; }
    saveSettings(s); s.cancellation = false; s.completedDestination.clear(); SetWindowTextW(s.log, L""); SetWindowTextW(s.status, tr(L"Starting backup...", L"Mentés indítása...")); SetWindowTextW(s.stats, copiedText(0, 0).c_str()); SendMessageW(s.progress, PBM_SETPOS, 0, 0); EnableWindow(s.open, FALSE); setRunning(s, true);
    BackupOptions options; options.projectFile = project; options.destination = destination; options.timestamped = IsDlgButtonChecked(s.hwnd, TimestampCheck) == BST_CHECKED;
    options.useSkipFile = IsDlgButtonChecked(s.hwnd, SkipCheck) == BST_CHECKED; options.verbose = IsDlgButtonChecked(s.hwnd, VerboseCheck) == BST_CHECKED; options.skipFile = executableDirectory() / L"qgsbackup_skip.txt";
    HWND hwnd = s.hwnd; std::atomic_bool* cancellation = &s.cancellation;
    s.worker = std::thread([hwnd, cancellation, options]() {
        BackupCallbacks callbacks;
        callbacks.isCancelled = [cancellation] { return cancellation->load(); };
        callbacks.onProgress = [hwnd](const BackupProgress& p) { PostMessageW(hwnd, MsgProgress, 0, reinterpret_cast<LPARAM>(new BackupProgress(p))); };
        callbacks.onLog = [hwnd](LogLevel level, const std::wstring& text) { PostMessageW(hwnd, MsgLog, 0, reinterpret_cast<LPARAM>(new LogMessage{level, text})); };
        auto result = runBackup(options, callbacks); PostMessageW(hwnd, MsgDone, 0, reinterpret_cast<LPARAM>(new BackupResult(std::move(result))));
    });
}

void layout(UiState& s, int width, int height) {
    auto px = [&](int value) { return scaled(value, s.dpi); };
    const int margin = px(16), labelH = px(18), editH = px(26), browseW = px(92), gap = px(8);
    const int checkH = px(22), actionW = px(118), actionH = px(30), progressH = px(18), lineH = px(19);
    int y = px(14);
    place(GetDlgItem(s.hwnd, 2001), margin, y, width - 2 * margin, labelH); y += px(20);
    place(s.project, margin, y, width - 2 * margin - browseW - gap, editH); place(GetDlgItem(s.hwnd, ProjectBrowse), width - margin - browseW, y, browseW, editH); y += px(36);
    place(GetDlgItem(s.hwnd, 2002), margin, y, width - 2 * margin, labelH); y += px(20);
    place(s.destination, margin, y, width - 2 * margin - browseW - gap, editH); place(GetDlgItem(s.hwnd, DestinationBrowse), width - margin - browseW, y, browseW, editH); y += px(36);
    place(s.timestamp, margin, y, px(260), checkH); place(s.skip, margin + px(268), y, px(230), checkH); place(s.verbose, width - margin - px(130), y, px(130), checkH); y += px(24);
    const int skipEditW = px(150);
    place(s.skipLabel, margin, y + px(4), width - 2 * margin - skipEditW - gap, labelH); place(s.editSkip, width - margin - skipEditW, y, skipEditW, editH); y += px(34);
    place(s.start, width - margin - actionW, y, actionW, actionH); place(s.cancel, width - margin - 2 * actionW - gap, y, actionW, actionH); y += px(38);
    place(s.progress, margin, y, width - 2 * margin, progressH); y += px(24);
    place(s.status, margin, y, width - 2 * margin, lineH); y += px(20); place(s.stats, margin, y, width - 2 * margin, lineH); y += px(24);
    const int bottom = height - margin - actionH;
    place(s.log, margin, y, width - 2 * margin, std::max(px(72), (bottom - gap) - y));
    place(s.open, margin, bottom, px(168), actionH);
}

HWND make(HWND parent, DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

bool usesTransparentBackground(const UiState& s, HWND control) {
    return control == GetDlgItem(s.hwnd, 2001) || control == GetDlgItem(s.hwnd, 2002) ||
           control == s.timestamp || control == s.skip || control == s.verbose ||
           control == s.skipLabel || control == s.status || control == s.stats;
}

LRESULT transparentControlColor(HDC dc) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UiState* s = stateOf(hwnd);
    switch (msg) {
    case WM_CREATE: {
        auto state = std::make_unique<UiState>(); state->hwnd = hwnd; state->dpi = GetDpiForWindow(hwnd); state->font = makeUiFont(state->dpi); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
        make(hwnd, 0, L"STATIC", tr(L"&QGIS project", L"&QGIS projekt"), 0, 2001); state->project = make(hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ProjectEdit); make(hwnd, 0, L"BUTTON", tr(L"&Browse...", L"&Tallózás..."), BS_PUSHBUTTON | WS_TABSTOP, ProjectBrowse);
        make(hwnd, 0, L"STATIC", tr(L"Backup &destination", L"Mentés &célmappája"), 0, 2002); state->destination = make(hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, DestinationEdit); make(hwnd, 0, L"BUTTON", tr(L"Browse...", L"Tallózás..."), BS_PUSHBUTTON | WS_TABSTOP, DestinationBrowse);
        state->timestamp = make(hwnd, 0, L"BUTTON", tr(L"Create &timestamped backup folder", L"&Időbélyeges mentési mappa létrehozása"), BS_AUTOCHECKBOX | WS_TABSTOP, TimestampCheck); state->skip = make(hwnd, 0, L"BUTTON", tr(L"Use qgsbackup_skip.txt", L"qgsbackup_skip.txt használata"), BS_AUTOCHECKBOX | WS_TABSTOP, SkipCheck); state->verbose = make(hwnd, 0, L"BUTTON", tr(L"&Verbose log", L"&Részletes napló"), BS_AUTOCHECKBOX | WS_TABSTOP, VerboseCheck);
        state->skipLabel = make(hwnd, 0, L"STATIC", L"", SS_PATHELLIPSIS, SkipLabel); state->editSkip = make(hwnd, 0, L"BUTTON", tr(L"&Edit skip list...", L"Kihagyási &lista..."), BS_PUSHBUTTON | WS_TABSTOP, SkipEditButton); state->start = make(hwnd, 0, L"BUTTON", tr(L"Start &Backup", L"&Mentés indítása"), BS_DEFPUSHBUTTON | WS_TABSTOP, StartButton); state->cancel = make(hwnd, 0, L"BUTTON", tr(L"Cancel", L"Mégse"), BS_PUSHBUTTON | WS_TABSTOP, CancelButton);
        state->progress = make(hwnd, 0, PROGRESS_CLASSW, L"", PBS_SMOOTH, ProgressBar); SendMessageW(state->progress, PBM_SETRANGE32, 0, 1000);
        state->status = make(hwnd, 0, L"STATIC", tr(L"Ready", L"Kész"), 0, StatusLabel); state->stats = make(hwnd, 0, L"STATIC", copiedText(0, 0).c_str(), 0, StatsLabel);
        state->log = make(hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, LogEdit); state->open = make(hwnd, 0, L"BUTTON", tr(L"Open backup folder", L"Mentési mappa megnyitása"), BS_PUSHBUTTON | WS_TABSTOP, OpenButton);
        for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) setFont(child, state->font);
        EnableWindow(state->cancel, FALSE); EnableWindow(state->open, FALSE); DragAcceptFiles(hwnd, TRUE); loadSettings(*state); updateSkipLabel(*state); updateStart(*state);
        RECT rc{}; GetClientRect(hwnd, &rc); layout(*state, rc.right, rc.bottom); state.release(); return 0;
    }
    case WM_SIZE: if (s) layout(*s, LOWORD(lParam), HIWORD(lParam)); return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (s && usesTransparentBackground(*s, reinterpret_cast<HWND>(lParam)))
            return transparentControlColor(reinterpret_cast<HDC>(wParam));
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam); UINT dpi = s ? s->dpi : 96;
        RECT bounds{0, 0, scaled(680, dpi), scaled(450, dpi)};
        AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0, dpi);
        info->ptMinTrackSize = {bounds.right - bounds.left, bounds.bottom - bounds.top}; return 0;
    }
    case WM_DPICHANGED: if (s) {
        UINT dpi = LOWORD(wParam); updateFonts(*s, dpi);
        auto* suggested = reinterpret_cast<RECT*>(lParam); SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RECT rc{}; GetClientRect(hwnd, &rc); layout(*s, rc.right, rc.bottom); RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN); return 0;
    }
    case WM_DROPFILES: if (s) { HDROP drop = reinterpret_cast<HDROP>(wParam); wchar_t path[32768]{}; if (DragQueryFileW(drop, 0, path, 32768)) { fs::path p(path); auto ext = p.extension().native(); if (_wcsicmp(ext.c_str(), L".qgs") == 0 || _wcsicmp(ext.c_str(), L".qgz") == 0) SetWindowTextW(s->project, path); } DragFinish(drop); updateStart(*s); } return 0;
    case WM_COMMAND: if (s) {
        switch (LOWORD(wParam)) {
        case ProjectBrowse: chooseProject(*s); break; case DestinationBrowse: chooseDestination(*s); break;
        case StartButton: startBackup(*s); break; case CancelButton: s->cancellation = true; SetWindowTextW(s->status, tr(L"Cancelling safely...", L"Biztonságos megszakítás...")); EnableWindow(s->cancel, FALSE); break;
        case OpenButton: if (!s->completedDestination.empty()) ShellExecuteW(hwnd, L"open", s->completedDestination.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case SkipEditButton: editSkipList(*s); break;
        case SkipCheck: updateSkipLabel(*s); break;
        case IDCANCEL: SendMessageW(hwnd, WM_CLOSE, 0, 0); break;
        default: if (HIWORD(wParam) == EN_CHANGE) updateStart(*s); break;
        } return 0;
    }
    case MsgProgress: if (s) { std::unique_ptr<BackupProgress> p(reinterpret_cast<BackupProgress*>(lParam)); int pos = p->bytesTotal ? static_cast<int>(1000 * std::min(p->bytesCopied, p->bytesTotal) / p->bytesTotal) : p->totalItems ? static_cast<int>(1000 * p->currentItem / p->totalItems) : 0; SendMessageW(s->progress, PBM_SETPOS, pos, 0); std::wstring status = stageName(p->stage); if (!p->currentSource.empty()) status += L": " + p->currentSource; SetWindowTextW(s->status, status.c_str()); auto stats = copiedText(p->bytesCopied, p->bytesTotal); SetWindowTextW(s->stats, stats.c_str()); } return 0;
    case MsgLog: if (s) { std::unique_ptr<LogMessage> line(reinterpret_cast<LogMessage*>(lParam)); appendLog(*s, line->level, line->text); } return 0;
    case MsgDone: if (s) { std::unique_ptr<BackupResult> result(reinterpret_cast<BackupResult*>(lParam)); if (s->worker.joinable()) s->worker.join(); setRunning(*s, false); s->completedDestination = result->destination; EnableWindow(s->open, !result->destination.empty() && fs::exists(result->destination)); SendMessageW(s->progress, PBM_SETPOS, result->status == BackupStatus::Success || result->status == BackupStatus::SuccessWithWarnings ? 1000 : 0, 0); SetWindowTextW(s->status, result->message.c_str()); auto stats = std::to_wstring(result->filesCopied) + tr(L" files • ", L" fájl • ") + humanReadableSize(result->bytesCopied) + (result->bytesTotal ? L" / " + humanReadableSize(result->bytesTotal) : std::wstring()) + L" • " + std::to_wstring(result->warnings) + tr(L" warnings", L" figyelmeztetés"); SetWindowTextW(s->stats, stats.c_str()); appendLog(*s, result->status == BackupStatus::Failure ? LogLevel::Error : result->status == BackupStatus::SuccessWithWarnings ? LogLevel::Warning : LogLevel::Info, result->message + tr(L"\r\nDestination: ", L"\r\nCél: ") + result->destination.native());
        if (s->closeWhenDone) { DestroyWindow(hwnd); return 0; }
        UINT icon = result->status == BackupStatus::Failure ? MB_ICONERROR : result->status == BackupStatus::Cancelled ? MB_ICONWARNING : MB_ICONINFORMATION; MessageBoxW(hwnd, (result->message + tr(L"\r\n\r\nDestination:\r\n", L"\r\n\r\nCél:\r\n") + result->destination.native() + L"\r\n\r\n" + stats).c_str(), appTitle(), MB_OK | icon); } return 0;
    case WM_CLOSE: if (s && s->running) { if (MessageBoxW(hwnd, tr(L"A backup is running. Cancel it and close after cleanup?", L"Mentés van folyamatban. Megszakítja, és a takarítás után bezárja?"), appTitle(), MB_YESNO | MB_ICONWARNING) == IDYES) { s->cancellation = true; s->closeWhenDone = true; EnableWindow(s->cancel, FALSE); SetWindowTextW(s->status, tr(L"Cancelling safely...", L"Biztonságos megszakítás...")); } return 0; } DestroyWindow(hwnd); return 0;
    case WM_DESTROY: if (s) { saveSettings(*s); if (s->worker.joinable()) s->worker.join(); DeleteObject(s->font); delete s; SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); } PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
}

int runGui(const fs::path& initialProject) {
    FreeConsole();
    if (windowsRegionIsHungary()) setLanguage(Language::Hungarian);
    if (auto setDpiAwareness = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
        setDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    HINSTANCE instance = GetModuleHandleW(nullptr); WNDCLASSEXW wc{sizeof(wc)}; wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = windowProc; wc.hInstance = instance; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP)); wc.hIconSm = wc.hIcon; wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.lpszClassName = WindowClass; RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem(); RECT windowBounds{0, 0, scaled(740, dpi), scaled(520, dpi)};
    AdjustWindowRectExForDpi(&windowBounds, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0, dpi);
    RECT workArea{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int windowWidth = std::min(windowBounds.right - windowBounds.left, (workArea.right - workArea.left) - scaled(24, dpi));
    int windowHeight = std::min(windowBounds.bottom - windowBounds.top, (workArea.bottom - workArea.top) - scaled(24, dpi));
    HWND hwnd = CreateWindowExW(0, WindowClass, appTitle(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, windowWidth, windowHeight, nullptr, nullptr, instance, nullptr);
    if (!hwnd) { CoUninitialize(); return 1; }
    if (!initialProject.empty()) { if (auto* s = stateOf(hwnd)) { SetWindowTextW(s->project, initialProject.c_str()); updateStart(*s); } }
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize(); return static_cast<int>(msg.wParam);
}
} // namespace qgsbackup
