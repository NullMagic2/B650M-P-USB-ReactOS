// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tools.gpu_setup_wizard
Purpose: Graphical front end for the Radeon package's command-line installer.
Created: 2026-11-14
Architecture: ReactOS/Win32 user-mode wizard placed beside radeonctl.exe in the
"XTX 7900 drivers" desktop package. It changes nothing itself: every action is
a radeonctl command whose output and exit code are shown unaltered. radeonctl
remains the owner of installation policy, journaling and recovery.
*/
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <string>
#include <vector>

namespace {
// radeonctl exit codes, as documented in the package's INSTALL.txt.
constexpr DWORD exit_ok = 0, exit_refused = 3, exit_no_gpu = 4, exit_restart = 3010;
constexpr DWORD exit_not_started = 0xffffffff;
// Sized to fit a 640x480 desktop with its taskbar: the display the system
// uses before any graphics driver is installed.
constexpr int client_width = 560, client_height = 376, margin = 12;

enum class Page { welcome, status, options, result };
enum class Task { install, remove, rollback };
enum : int {
    id_back = 100, id_next, id_cancel, id_restart, id_copy,
    id_task_install = 200, id_task_remove, id_task_rollback,
    id_firmware = 300, id_first_option
};

// One installer opt-in: radeonctl switch, label and the option it depends on
// (index into this table, or none). The dependencies are radeonctl's own
// feature rules; it refuses a set that breaks them. radeonctl's --rom-override
// is left to the command line: it makes the driver ignore the card's own ROM
// and needs a ROM image that the package cannot contain.
constexpr int none = -1, option_asic = 0, option_2d = 2;
struct Option {
    const wchar_t *argument, *label;
    int needs;
};
constexpr Option options[] = {
    {L"--asic", L"Initialise the card with AMD firmware (--asic)", none},
    {L"--vbe", L"Set display modes through the video BIOS (--vbe)", option_asic},
    {L"--2d", L"Accelerated 2D drawing (--2d)", option_asic},
    {L"--2d-compute", L"2D drawing on the compute engine (--2d-compute)", option_2d},
    {L"--cursor", L"Hardware mouse cursor (--cursor)", option_asic},
    {L"--opengl-icd", L"Install the OpenGL driver (--opengl-icd)", none},
};
constexpr int option_count = sizeof(options) / sizeof(options[0]);

struct Wizard {
    HWND window{}, title{}, body{}, output{}, back{}, next{}, cancel{}, restart{}, copy{};
    HWND tasks[3]{}, firmware{}, boxes[option_count]{}, options_note{};
    HFONT font{}, heading{}, fixed{};
    Page page{Page::welcome};
    Task task{Task::install};
    std::wstring directory;
    bool has_firmware{}, gpu_present{};
    DWORD last_exit{exit_not_started};
};
Wizard wizard;

struct RunResult {
    DWORD exit_code{exit_not_started};
    std::wstring text;
};

std::wstring package_path(const wchar_t *name) {
    return wizard.directory + L"\\" + name;
}

bool exists(const std::wstring &path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool checked(int option) {
    return SendMessageW(wizard.boxes[option], BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void set_checked(int option, bool on) {
    SendMessageW(wizard.boxes[option], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Keeps the ticked set one the installer accepts: ticking a feature ticks
// what it depends on, clearing one clears what depends on it.
void apply_dependencies(int changed) {
    if (checked(changed)) {
        for (int need = options[changed].needs; need != none; need = options[need].needs) {
            set_checked(need, true);
        }
    } else {
        for (bool again = true; again;) {
            again = false;
            for (int i = 0; i < option_count; ++i) {
                if (options[i].needs != none && checked(i) && !checked(options[i].needs)) {
                    set_checked(i, false);
                    again = true;
                }
            }
        }
    }
}

// Puts the whole report on the clipboard, so it can be pasted into a text
// file or message without selecting it by hand.
void copy_report() {
    const int length = GetWindowTextLengthW(wizard.output);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(length) + 1) * sizeof(wchar_t));
    if (!memory) {
        return;
    }
    GetWindowTextW(wizard.output, static_cast<wchar_t *>(GlobalLock(memory)), length + 1);
    GlobalUnlock(memory);
    bool placed = false;
    if (OpenClipboard(wizard.window)) {
        EmptyClipboard();
        placed = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        CloseClipboard();
    }
    if (!placed) {
        GlobalFree(memory);
    }
    SetWindowTextW(wizard.copy, placed ? L"Copied" : L"Copy failed");
}

// The installer writes console text. Accept either byte text in the console
// code page or UTF-16, and give the edit control CR LF line ends.
std::wstring decode(const std::string &bytes) {
    std::wstring wide;
    size_t zeros = 0;
    for (size_t i = 1; i < bytes.size(); i += 2) {
        zeros += bytes[i] == 0;
    }
    if (bytes.size() >= 2 && zeros * 4 > bytes.size()) {
        wide.assign(reinterpret_cast<const wchar_t *>(bytes.data()), bytes.size() / 2);
    } else if (!bytes.empty()) {
        const int length = MultiByteToWideChar(CP_OEMCP, 0, bytes.data(),
                                               static_cast<int>(bytes.size()), nullptr, 0);
        wide.resize(length > 0 ? length : 0);
        if (length > 0) {
            MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), static_cast<int>(bytes.size()),
                                &wide[0], length);
        }
    }
    std::wstring text;
    for (size_t i = 0; i < wide.size(); ++i) {
        if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r')) {
            text += L'\r';
        }
        if (wide[i] != 0) {
            text += wide[i];
        }
    }
    return text;
}

void append_output(const std::wstring &text) {
    const int length = GetWindowTextLengthW(wizard.output);
    SendMessageW(wizard.output, EM_SETSEL, length, length);
    SendMessageW(wizard.output, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    UpdateWindow(wizard.output);
}

// Runs radeonctl with the given arguments in the package directory, hidden,
// and returns its exit code with everything it printed. The report box shows
// each line as it arrives, so the last step reached stays visible if the
// machine stops responding.
RunResult run_installer(const std::wstring &arguments) {
    RunResult result;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end{}, write_end{};
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
        result.text = L"Could not create a pipe for the installer output.\r\n";
        append_output(result.text);
        return result;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + package_path(L"radeonctl.exe") + L"\" " + arguments;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = startup.hStdError = write_end;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, wizard.directory.c_str(),
                                        &startup, &process);
    CloseHandle(write_end);
    if (!started) {
        wchar_t message[160];
        wsprintfW(message, L"radeonctl.exe could not be started (Win32 error %lu).\r\n",
                  GetLastError());
        result.text = message;
        append_output(result.text);
        CloseHandle(read_end);
        return result;
    }
    std::string pending;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(read_end, chunk, sizeof(chunk), &got, nullptr) && got) {
        pending.append(chunk, got);
        const size_t line_end = pending.rfind('\n');
        if (line_end != std::string::npos) {
            const std::wstring lines = decode(pending.substr(0, line_end + 1));
            pending.erase(0, line_end + 1);
            result.text += lines;
            append_output(lines);
        }
    }
    if (!pending.empty()) {
        const std::wstring rest = decode(pending);
        result.text += rest;
        append_output(rest);
    }
    CloseHandle(read_end);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &result.exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

// radeonctl names the driver it would replace in its refusal:
// "... Re-run with --force-replace <Service> to replace that binding explicitly."
std::wstring service_to_replace(const std::wstring &text) {
    const std::wstring marker = L"--force-replace ";
    const size_t refusal = text.find(L"Re-run with " + marker);
    if (refusal == std::wstring::npos) {
        return {};
    }
    const size_t start = refusal + 12 + marker.size();
    size_t end = start;
    while (end < text.size() && text[end] > L' ') {
        ++end;
    }
    return text.substr(start, end - start);
}

void show(HWND control, bool visible) {
    ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

void set_text(HWND control, const wchar_t *text) {
    SetWindowTextW(control, text);
}

void show_page(Page page) {
    wizard.page = page;
    const bool welcome = page == Page::welcome, status = page == Page::status;
    const bool choose = page == Page::options, result = page == Page::result;
    for (HWND task : wizard.tasks) {
        show(task, welcome);
    }
    show(wizard.output, status || result);
    show(wizard.firmware, choose && wizard.has_firmware);
    show(wizard.options_note, choose);
    for (HWND box : wizard.boxes) {
        show(box, choose);
    }
    const bool restart = result && wizard.last_exit == exit_restart;
    show(wizard.restart, restart);
    // The copy button sits at the left, after "Restart now" when that is shown.
    show(wizard.copy, status || result);
    set_text(wizard.copy, L"Copy report");
    MoveWindow(wizard.copy, restart ? margin + 106 : margin, client_height - 38, 100, 26, TRUE);
    // The report box starts higher than the option pages' controls, so the
    // explanatory text above it is kept to two lines there.
    MoveWindow(wizard.body, margin, margin + 22, client_width - 2 * margin,
               status || result ? 34 : 84, TRUE);
    EnableWindow(wizard.back, !welcome && !result);
    set_text(wizard.cancel, result ? L"Close" : L"Cancel");
    if (welcome) {
        set_text(wizard.title, L"Radeon RX 7900 driver setup");
        set_text(wizard.body,
                 L"This wizard installs or removes the experimental ReactOS display driver "
                 L"for the AMD Radeon RX 7900 (Navi31). It runs radeonctl.exe from this "
                 L"folder and shows you everything that program reports.\r\n\r\n"
                 L"The driver is experimental and has not been proven on this card. If the "
                 L"screen stays black after a restart, choose \"ReactOS persistent USB "
                 L"(basic VGA)\" in the boot menu and run this wizard again to remove it.");
        set_text(wizard.next, L"Next >");
        EnableWindow(wizard.next, TRUE);
    } else if (status) {
        set_text(wizard.title, L"Current state");
        set_text(wizard.body, L"This is what the installer reports about the card, the driver "
                              L"files and the firmware. Nothing has been changed.");
        const bool installing = wizard.task == Task::install;
        set_text(wizard.next, installing ? L"Next >" : (wizard.task == Task::remove
                                                            ? L"Remove" : L"Roll back"));
        // The installer itself refuses when no card is present; the wizard
        // only explains that early and never blocks on its own reading.
        EnableWindow(wizard.next, TRUE);
    } else if (choose) {
        set_text(wizard.title, L"Features");
        set_text(wizard.body, L"For a first installation leave every feature off, restart, and "
                              L"check that the desktop appears. Run this wizard again to turn "
                              L"features on one at a time.");
        set_text(wizard.next, L"Install");
        EnableWindow(wizard.next, TRUE);
    } else {
        set_text(wizard.next, L"Finish");
        EnableWindow(wizard.next, TRUE);
    }
    SetFocus(wizard.next);
}

void run_status() {
    set_text(wizard.output, L"");
    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const RunResult status = run_installer(L"status");
    SetCursor(previous);
    wizard.gpu_present = status.exit_code != exit_not_started &&
                         status.text.find(L"No Navi31 GPU") == std::wstring::npos;
    if (!wizard.gpu_present && wizard.task == Task::install) {
        append_output(L"\r\nNo Radeon RX 7900 was found. Installing will be refused on this "
                      L"machine.\r\n");
    }
}

const wchar_t *summary(DWORD code) {
    switch (code) {
    case exit_ok:
        return L"Finished. No restart is needed.";
    case exit_restart:
        return L"Finished. Restart ReactOS to use the change.";
    case exit_refused:
        return L"The installer refused the request and changed nothing. Its reason is in "
               L"the report.";
    case exit_no_gpu:
        return L"No Radeon RX 7900 was found. Nothing was changed.";
    case exit_not_started:
        return L"The installer could not be run.";
    default:
        return L"The installer reported a failure. The details are in the report.";
    }
}

// Performs the chosen task and leaves the result page showing what happened.
void run_task() {
    set_text(wizard.title, L"Working");
    set_text(wizard.body, L"Please wait while the installer runs.");
    set_text(wizard.output, L"");
    wizard.last_exit = exit_not_started;
    show_page(Page::result);
    EnableWindow(wizard.next, FALSE);
    UpdateWindow(wizard.window);
    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    RunResult last;
    if (wizard.task == Task::remove) {
        append_output(L"> radeonctl remove\r\n");
        last = run_installer(L"remove");
    } else if (wizard.task == Task::rollback) {
        append_output(L"> radeonctl rollback\r\n");
        last = run_installer(L"rollback");
    } else {
        std::wstring arguments = L"install";
        for (int i = 0; i < option_count; ++i) {
            if (checked(i)) {
                arguments += L" ";
                arguments += options[i].argument;
            }
        }
        if (wizard.has_firmware &&
            SendMessageW(wizard.firmware, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            append_output(L"> radeonctl firmware-copy Firmware\r\n");
            run_installer(L"firmware-copy \"" + package_path(L"Firmware") + L"\"");
            append_output(L"\r\n");
        }
        // Ask the installer for its plan first. When it names the driver now
        // bound to the card, that driver is replaced without a question: the
        // card keeps using it until the restart, and removal restores it.
        append_output(L"> radeonctl " + arguments + L" --dry-run\r\n");
        last = run_installer(arguments + L" --dry-run");
        const std::wstring current = service_to_replace(last.text);
        bool proceed = last.exit_code == exit_ok || last.exit_code == exit_restart;
        if (!current.empty()) {
            append_output(L"\r\nThe card is using the display driver \"" + current +
                          L"\". Setup replaces it automatically; the Radeon driver takes over "
                          L"at the next restart.\r\n");
            arguments += L" --force-replace " + current;
            proceed = true;
        }
        if (proceed) {
            append_output(L"\r\n> radeonctl " + arguments + L"\r\n");
            last = run_installer(arguments);
        }
    }
    SetCursor(previous);
    wizard.last_exit = last.exit_code;
    const wchar_t *outcome = summary(last.exit_code);
    append_output(std::wstring(L"\r\n") + outcome + L"\r\n");
    set_text(wizard.title, last.exit_code == exit_ok || last.exit_code == exit_restart
                               ? L"Completed" : L"Not completed");
    set_text(wizard.body, outcome);
    show_page(Page::result);
}

void restart_system() {
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &privileges.Privileges[0].Luid);
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
        CloseHandle(token);
    }
    if (!ExitWindowsEx(EWX_REBOOT, 0)) {
        MessageBoxW(wizard.window, L"ReactOS could not be restarted from here. Use Start, "
                                   L"Shut Down instead.", L"Restart", MB_OK | MB_ICONWARNING);
    }
}

void go_next() {
    switch (wizard.page) {
    case Page::welcome:
        wizard.task = SendMessageW(wizard.tasks[1], BM_GETCHECK, 0, 0) == BST_CHECKED
                          ? Task::remove
                          : SendMessageW(wizard.tasks[2], BM_GETCHECK, 0, 0) == BST_CHECKED
                                ? Task::rollback : Task::install;
        show_page(Page::status);
        UpdateWindow(wizard.window);
        run_status();
        show_page(Page::status);
        break;
    case Page::status:
        if (wizard.task == Task::install) {
            show_page(Page::options);
        } else {
            run_task();
        }
        break;
    case Page::options:
        run_task();
        break;
    case Page::result:
        DestroyWindow(wizard.window);
        break;
    }
}

void go_back() {
    if (wizard.page == Page::status) {
        show_page(Page::welcome);
    } else if (wizard.page == Page::options) {
        show_page(Page::status);
    }
}

HWND control(const wchar_t *type, const wchar_t *text, DWORD style, int x, int y, int width,
             int height, int id, HFONT font) {
    HWND window = CreateWindowExW(0, type, text, WS_CHILD | style, x, y, width, height,
                                  wizard.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return window;
}

void create_controls() {
    wizard.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    LOGFONTW description{};
    GetObjectW(wizard.font, sizeof(description), &description);
    description.lfWeight = FW_BOLD;
    wizard.heading = CreateFontIndirectW(&description);
    wizard.fixed = static_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT));
    const int width = client_width - 2 * margin;
    wizard.title = control(L"STATIC", L"", WS_VISIBLE, margin, margin, width, 18, 0,
                           wizard.heading);
    wizard.body = control(L"STATIC", L"", WS_VISIBLE, margin, margin + 22, width, 84, 0,
                          wizard.font);
    const wchar_t *task_labels[] = {L"Install the driver, or change its features",
                                    L"Remove the driver", L"Roll back to the previous driver files"};
    for (int i = 0; i < 3; ++i) {
        wizard.tasks[i] = control(L"BUTTON", task_labels[i],
                                  BS_AUTORADIOBUTTON | WS_TABSTOP | (i == 0 ? WS_GROUP : 0),
                                  margin + 10, 128 + i * 24, width - 20, 20, id_task_install + i,
                                  wizard.font);
    }
    SendMessageW(wizard.tasks[0], BM_SETCHECK, BST_CHECKED, 0);
    wizard.output = control(L"EDIT", L"",
                            WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                            margin, 72, width, client_height - 72 - 50, 0, wizard.fixed);
    SendMessageW(wizard.output, EM_SETLIMITTEXT, 0x100000, 0);
    wizard.firmware = control(L"BUTTON", L"Copy the AMD firmware from this package's Firmware folder",
                              BS_AUTOCHECKBOX | WS_TABSTOP, margin + 10, 84, width - 20, 20,
                              id_firmware, wizard.font);
    SendMessageW(wizard.firmware, BM_SETCHECK, BST_CHECKED, 0);
    wizard.options_note = control(L"STATIC", L"Optional features (all off is the safe choice):",
                                  0, margin + 10, 110, width - 20, 18, 0, wizard.font);
    for (int i = 0; i < option_count; ++i) {
        wizard.boxes[i] = control(L"BUTTON", options[i].label, BS_AUTOCHECKBOX | WS_TABSTOP,
                                  margin + 24, 130 + i * 22, width - 34, 20, id_first_option + i,
                                  wizard.font);
    }
    const int row = client_height - 38;
    wizard.restart = control(L"BUTTON", L"Restart now", WS_TABSTOP, margin, row, 100, 26,
                             id_restart, wizard.font);
    wizard.copy = control(L"BUTTON", L"Copy report", WS_TABSTOP, margin, row, 100, 26, id_copy,
                          wizard.font);
    wizard.back = control(L"BUTTON", L"< Back", WS_VISIBLE | WS_TABSTOP,
                          client_width - margin - 3 * 92, row, 86, 26, id_back, wizard.font);
    wizard.next = control(L"BUTTON", L"Next >", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                          client_width - margin - 2 * 92, row, 86, 26, id_next, wizard.font);
    wizard.cancel = control(L"BUTTON", L"Cancel", WS_VISIBLE | WS_TABSTOP,
                            client_width - margin - 92, row, 86, 26, id_cancel, wizard.font);
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case id_next:
        case IDOK:
            go_next();
            return 0;
        case id_back:
            go_back();
            return 0;
        case id_cancel:
        case IDCANCEL:
            DestroyWindow(window);
            return 0;
        case id_restart:
            restart_system();
            return 0;
        case id_copy:
            copy_report();
            return 0;
        default:
            if (LOWORD(w) >= id_first_option && LOWORD(w) < id_first_option + option_count &&
                HIWORD(w) == BN_CLICKED) {
                apply_dependencies(LOWORD(w) - id_first_option);
                return 0;
            }
        }
        break;
    case WM_CTLCOLORSTATIC:
        // Read-only edit controls and labels share the dialog background.
        SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    wizard.directory = module;
    wizard.directory.erase(wizard.directory.find_last_of(L'\\'));
    if (!exists(package_path(L"radeonctl.exe"))) {
        MessageBoxW(nullptr, L"radeonctl.exe was not found next to this program. Keep Setup.exe "
                             L"inside the \"XTX 7900 drivers\" folder.",
                    L"Radeon RX 7900 driver setup", MB_OK | MB_ICONERROR);
        return 2;
    }
    wizard.has_firmware = exists(package_path(L"Firmware"));
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    window_class.lpszClassName = L"RadeonSetupWizard";
    if (!RegisterClassW(&window_class)) {
        return 1;
    }
    RECT frame{0, 0, client_width, client_height};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&frame, style, FALSE);
    // Centre in the work area so the buttons never sit under the taskbar.
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int window_width = frame.right - frame.left, window_height = frame.bottom - frame.top;
    int left = work.left + (work.right - work.left - window_width) / 2;
    int top = work.top + (work.bottom - work.top - window_height) / 2;
    left = left < work.left ? work.left : left;
    top = top < work.top ? work.top : top;
    wizard.window = CreateWindowExW(WS_EX_CONTROLPARENT, window_class.lpszClassName,
                                    L"Radeon RX 7900 driver setup", style, left, top,
                                    window_width, window_height, nullptr, nullptr, instance,
                                    nullptr);
    if (!wizard.window) {
        return 1;
    }
    create_controls();
    show_page(Page::welcome);
    ShowWindow(wizard.window, show_command);
    UpdateWindow(wizard.window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Ctrl+A selects the whole report; a read-only edit box has no such key.
        if (message.message == WM_KEYDOWN && message.wParam == 'A' &&
            GetKeyState(VK_CONTROL) < 0 && message.hwnd == wizard.output) {
            SendMessageW(wizard.output, EM_SETSEL, 0, -1);
            continue;
        }
        // Dialog navigation: Tab moves, Enter presses the default button.
        if (!IsDialogMessageW(wizard.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return 0;
}
