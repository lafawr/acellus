#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <UIAutomation.h>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "UIAutomationCore.lib")

static HHOOK g_hHook = NULL;
static HWND g_hWnd = NULL;
static IUIAutomation* g_pAuto = NULL;
static HWINEVENTHOOK g_hFocusHook = NULL;

static std::wstring g_baselineText;
static bool g_haveBaseline = false;

static const wchar_t* TARGET_PROCESS = L"AcellusWinUI3.exe";

#define WM_DO_COPY  (WM_APP + 1)
#define WM_DO_PASTE (WM_APP + 2)
#define WM_DO_PANIC (WM_APP + 3)

static void SendKey(WORD vk, bool ctrl = false) {
    INPUT in[4] = {};
    int n = 0;
    if (ctrl) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; n++; }
    in[n] = INPUT{}; in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; n++;
    in[n] = in[n - 1]; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    if (ctrl) { in[n] = INPUT{}; in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    SendInput(n, in, sizeof(INPUT));
}

static void TypeText(const std::wstring& text) {
    for (wchar_t ch : text) {
        if (ch == L'\r') continue;
        if (ch == L'\n') {
            INPUT in[2] = {};
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_RETURN;
            in[1] = in[0];
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
            continue;
        }
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

static bool IsForegroundTarget() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    bool match = false;
    if (QueryFullProcessImageNameW(hProc, 0, path, &size)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        match = (_wcsicmp(name, TARGET_PROCESS) == 0);
    }
    CloseHandle(hProc);
    return match;
}

static void SetClipboardTextW(const std::wstring& text) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (hMem) {
        wchar_t* p = (wchar_t*)GlobalLock(hMem);
        memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
}

static IUIAutomationElement* ResolveFocusedElement() {
    if (!g_pAuto) return NULL;
    IUIAutomationElement* elem = NULL;
    g_pAuto->GetFocusedElement(&elem);
    return elem;
}

static bool GetElementFullText(IUIAutomationElement* elem, std::wstring& out) {
    IUIAutomationTextPattern* pText = NULL;
    if (SUCCEEDED(elem->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pText))) && pText) {
        IUIAutomationTextRange* pDoc = NULL;
        bool ok = false;
        if (SUCCEEDED(pText->get_DocumentRange(&pDoc)) && pDoc) {
            BSTR full = NULL;
            if (SUCCEEDED(pDoc->GetText(-1, &full)) && full) {
                out = full;
                SysFreeString(full);
                ok = true;
            }
            pDoc->Release();
        }
        pText->Release();
        if (ok) return true;
    }
    IUIAutomationValuePattern* pValue = NULL;
    if (SUCCEEDED(elem->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pValue))) && pValue) {
        BSTR val = NULL;
        bool ok = SUCCEEDED(pValue->get_CurrentValue(&val)) && val;
        if (ok) {
            out = val;
            SysFreeString(val);
        }
        pValue->Release();
        return ok;
    }
    return false;
}

static void CALLBACK OnFocusChanged(HWINEVENTHOOK, DWORD, HWND, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_CLIENT) return;
    if (!IsForegroundTarget()) { g_haveBaseline = false; return; }
    IUIAutomationElement* elem = ResolveFocusedElement();
    if (!elem) return;
    std::wstring text;
    g_haveBaseline = GetElementFullText(elem, text);
    if (g_haveBaseline) g_baselineText = text;
    elem->Release();
}

static void DoCopy() {
    if (!IsForegroundTarget()) return;
    IUIAutomationElement* pFocused = ResolveFocusedElement();
    if (!pFocused) return;

    std::wstring text;
    bool got = false;

    IUIAutomationTextPattern* pText = NULL;
    if (SUCCEEDED(pFocused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pText))) && pText) {
        IUIAutomationTextRangeArray* pSel = NULL;
        if (SUCCEEDED(pText->GetSelection(&pSel)) && pSel) {
            int count = 0;
            pSel->get_Length(&count);
            if (count > 0) {
                IUIAutomationTextRange* pRange = NULL;
                if (SUCCEEDED(pSel->GetElement(0, &pRange)) && pRange) {
                    BSTR sel = NULL;
                    if (SUCCEEDED(pRange->GetText(-1, &sel)) && sel) {
                        if (wcslen(sel) > 0) { text = sel; got = true; }
                        SysFreeString(sel);
                    }
                    pRange->Release();
                }
            }
            pSel->Release();
        }
        pText->Release();
    }

    if (!got) got = GetElementFullText(pFocused, text);
    pFocused->Release();

    if (got && !text.empty()) SetClipboardTextW(text);
}

static void DoPaste() {
    if (!IsForegroundTarget()) return;

    if (!OpenClipboard(NULL)) return;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    std::wstring clip;
    if (hData) {
        wchar_t* p = (wchar_t*)GlobalLock(hData);
        if (p) { clip = p; GlobalUnlock(hData); }
    }
    CloseClipboard();
    if (clip.empty()) return;

    IUIAutomationElement* pFocused = ResolveFocusedElement();
    if (!pFocused) return;

    IUIAutomationValuePattern* pValue = NULL;
    if (SUCCEEDED(pFocused->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pValue))) && pValue) {
        BOOL readOnly = FALSE;
        pValue->get_CurrentIsReadOnly(&readOnly);
        if (!readOnly) {
            BSTR val = SysAllocString(clip.c_str());
            if (val) { pValue->SetValue(val); SysFreeString(val); }
        }
        pValue->Release();
    } else {
        std::wstring current;
        bool haveCurrent = GetElementFullText(pFocused, current);
        if (g_haveBaseline && haveCurrent && current == g_baselineText) {
            size_t originalLen = g_baselineText.size();
            SendKey(VK_END, true);
            TypeText(clip);
            SendKey(VK_HOME, true);
            for (size_t i = 0; i < originalLen; i++) SendKey(VK_DELETE);
            g_haveBaseline = false;
        }
    }
    pFocused->Release();
}

static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DO_COPY:
            DoCopy();
            return 0;
        case WM_DO_PASTE:
            DoPaste();
            return 0;
        case WM_DO_PANIC:
            if (g_hHook) { UnhookWindowsHookEx(g_hHook); g_hHook = NULL; }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (kb->vkCode == VK_END) {
            PostMessage(g_hWnd, WM_DO_PANIC, 0, 0);
        } else if (kb->vkCode == 'V' && ctrlDown && IsForegroundTarget()) {
            PostMessage(g_hWnd, WM_DO_PASTE, 0, 0);
            return 1;
        } else if (kb->vkCode == 'C' && ctrlDown && IsForegroundTarget()) {
            PostMessage(g_hWnd, WM_DO_COPY, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void**)&g_pAuto);

    const wchar_t* className = L"UnblockPasteHiddenWnd";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    RegisterClassW(&wc);

    g_hWnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (!g_hWnd) return 1;

    g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    if (!g_hHook) { DestroyWindow(g_hWnd); return 1; }

    g_hFocusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, NULL,
        OnFocusChanged, 0, 0, WINEVENT_OUTOFCONTEXT);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hFocusHook) UnhookWinEvent(g_hFocusHook);
    if (g_hHook) UnhookWindowsHookEx(g_hHook);
    if (g_pAuto) g_pAuto->Release();
    CoUninitialize();
    return 0;
}
