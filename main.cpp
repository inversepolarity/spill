#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <iostream>
#include <fstream> 

#include "broadcast_embed.h"
#include "clipboard_embed.h"
#include "resource.h"

// Window dimensions
#define WINDOW_WIDTH 400
#define WINDOW_HEIGHT 350
#define ID_ICON_BUTTON 1006  

#define ID_TRAY_SHOW 1001
#define ID_TRAY_EXIT 1002
#define ID_BTN_START 1003
#define ID_BTN_STOP 1004
#define ID_CHK_EXTERNAL 1005
#define WM_APP_EXIT (WM_APP + 1)
#define WM_APP_SHOW (WM_APP + 2)

// Unique identifiers for single instance enforcement
#define APP_MUTEX_NAME L"ClipboardBroadcastTrayMutex"
#define APP_WINDOW_CLASS L"ClipboardTrayWindow"

COLORREF versionLabelColor = RGB(22, 155, 22);
WNDPROC originalEditProc = NULL;
HWND hwndInputHost, hwndInputUser, hwndLogBox, hwndBtnStart, hwndBtnStop;
HWND hwndChkExternal, hwndInputExtHost, hwndInputExtPort, hwndLabelExtHost, hwndLabelExtPort;
HWND hwndVersionLabel;
PROCESS_INFORMATION piBroadcast, piClient;
HMENU hTrayMenu;
NOTIFYICONDATA nid = {};
bool processesStarted = false;
std::string broadcastFilePath, clipboardFilePath;
HANDLE hMutex = NULL;
HBRUSH hBackgroundBrush = NULL;
HBRUSH hLogBoxBrush = NULL;
HFONT hLogBoxFont = NULL;

LRESULT CALLBACK LogBoxProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rect;
            GetClientRect(hwnd, &rect);
            HBRUSH brush = CreateSolidBrush(RGB(200, 230, 200));
            FillRect((HDC)wParam, &rect, brush);
            DeleteObject(brush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // Get the client rect
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            // Fill background
            HBRUSH bgBrush = CreateSolidBrush(RGB(200, 230, 200));
            FillRect(hdc, &rect, bgBrush);
            DeleteObject(bgBrush);
            
            // Get the text
            int textLen = GetWindowTextLengthW(hwnd);
            if (textLen > 0) {
                wchar_t* buffer = new wchar_t[textLen + 1];
                GetWindowTextW(hwnd, buffer, textLen + 1);
                
                // Set up transparent text drawing
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(0, 0, 0));  // Black text
                
                // Select the font
                HFONT oldFont = NULL;
                if (hLogBoxFont) {
                    oldFont = (HFONT)SelectObject(hdc, hLogBoxFont);
                }
                
                // Draw the text with transparency
                RECT textRect = rect;
                textRect.left += 2;  // Small margin
                textRect.top += 2;
                
                DrawTextW(hdc, buffer, -1, &textRect, 
                         DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
                
                // Restore old font
                if (oldFont) {
                    SelectObject(hdc, oldFont);
                }
                
                delete[] buffer;
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETTEXT:
        case WM_CHAR:
        case EM_REPLACESEL:
            // Let the default handler process the message first
            LRESULT result = CallWindowProc(originalEditProc, hwnd, msg, wParam, lParam);
            // Then invalidate to trigger our custom paint
            InvalidateRect(hwnd, NULL, TRUE);
            return result;
    }
    return CallWindowProc(originalEditProc, hwnd, msg, wParam, lParam);
}

void AppendLog(const std::string& text, bool clear=true) {
    if (!hwndLogBox) return;

    if (clear == true){
        // Clear the log box completely
        SendMessageW(hwndLogBox, EM_SETREADONLY, FALSE, 0);  // Remove read-only flag
        SendMessageW(hwndLogBox, EM_SETSEL, 0, -1);          // Select all text
        SendMessageW(hwndLogBox, EM_REPLACESEL, FALSE, (LPARAM)L""); // Replace with empty string
        InvalidateRect(hwndLogBox, NULL, TRUE);              // Force redraw
        UpdateWindow(hwndLogBox);                            // Update immediately
        SendMessageW(hwndLogBox, EM_SETREADONLY, TRUE, 0);   // Restore read-only flag        
    }


    std::wstring wtext(text.begin(), text.end());
    wtext += L"\r\n";

    // CRITICAL: Remove read-only flag temporarily
    SendMessageW(hwndLogBox, EM_SETREADONLY, FALSE, 0);
    
    // Set selection to end of text
    SendMessageW(hwndLogBox, EM_SETSEL, -1, -1);
    
    // Replace selection (which is at the end) with new text
    SendMessageW(hwndLogBox, EM_REPLACESEL, FALSE, (LPARAM)wtext.c_str());
    
    // Restore read-only flag
    SendMessageW(hwndLogBox, EM_SETREADONLY, TRUE, 0);
    
    // Force scroll to bottom after text is added
    SendMessageW(hwndLogBox, WM_VSCROLL, SB_BOTTOM, 0);
}

// Utility to read from pipe and append to log window
void ReadPipeAndLog(HANDLE hPipe) {
    char buffer[512];
    DWORD bytesRead;

    while (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
        buffer[bytesRead] = '\0';
        
        // Split by newlines and append each line
        std::string line;
        for (DWORD i = 0; i < bytesRead; i++) {
            if (buffer[i] == '\n' || buffer[i] == '\r') {
                if (!line.empty()) {
                    AppendLog(line);
                    line.clear();
                }
            } else {
                line += buffer[i];
            }
        }
        // Append any remaining text
        if (!line.empty()) {
            AppendLog(line);
        }
    }
}

void UpdateButtonStates() {
    if (hwndBtnStart && hwndBtnStop) {
        EnableWindow(hwndBtnStart, !processesStarted);
        EnableWindow(hwndBtnStop, processesStarted);
    }
}

void UpdateExternalServerState() {
    if (!hwndChkExternal || !hwndInputHost || !hwndInputExtHost || !hwndInputExtPort) return;
    
    BOOL isExternal = (SendMessage(hwndChkExternal, BM_GETCHECK, 0, 0) == BST_CHECKED);
    
    // FIXED: Enable/disable controls based on checkbox state
    EnableWindow(hwndInputHost, !isExternal);        // Host URL enabled when external is OFF
    EnableWindow(hwndInputUser, TRUE);               // Username should ALWAYS be enabled
    EnableWindow(hwndInputExtHost, isExternal);      // Ext Host enabled when external is ON  
    EnableWindow(hwndInputExtPort, isExternal);      // Ext Port enabled when external is ON
    EnableWindow(hwndLabelExtHost, isExternal);      // Label enabled when external is ON
    EnableWindow(hwndLabelExtPort, isExternal);      // Label enabled when external is ON
    
    // Also update the visual appearance of disabled controls
    if (isExternal) {
        SetWindowTextW(hwndInputHost, L"");  // Clear when disabled
    } else {
        SetWindowTextW(hwndInputExtHost, L"192.168.1.100");  // Reset to default when disabled
        SetWindowTextW(hwndInputExtPort, L"8000");
    }
    
    // Force a visual update
    InvalidateRect(hwndInputExtHost, NULL, TRUE);
    InvalidateRect(hwndInputExtPort, NULL, TRUE);
    InvalidateRect(hwndInputHost, NULL, TRUE);
    UpdateWindow(hwndInputExtHost);
    UpdateWindow(hwndInputExtPort);
    UpdateWindow(hwndInputHost);
}

void HandleExternalCheckboxClick() {
    UpdateExternalServerState();
    
    // Optional: Show a message about what changed
    BOOL isExternal = (SendMessage(hwndChkExternal, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (isExternal) {
        AppendLog("Switched to external server mode", false);
    } else {
        AppendLog("Switched to local server mode", false);
    }
}

void CleanupTempFiles() {
    if (!broadcastFilePath.empty()) {
        if (DeleteFileA(broadcastFilePath.c_str())) {
            AppendLog("Deleted broadcast.py");
        } else {
            AppendLog("Could not delete broadcast.py");
        }
        broadcastFilePath.clear();
    }
    
    if (!clipboardFilePath.empty()) {
        if (DeleteFileA(clipboardFilePath.c_str())) {
            AppendLog("Deleted clipboard.py");
        } else {
            AppendLog("Could not delete clipboard.py");
        }
        clipboardFilePath.clear();
    }
}

void StopProcesses() {
    if (!processesStarted) return;

    AppendLog("Stopping processes...");

    BOOL isExternal = (SendMessage(hwndChkExternal, BM_GETCHECK, 0, 0) == BST_CHECKED);
    
    if (isExternal) {
        // External server mode - need to stop remote server via SSH
        wchar_t extHost[256];
        GetWindowTextW(hwndInputExtHost, extHost, 256);
        std::wstring wsExtHost(extHost);
        std::string extHostStr(wsExtHost.begin(), wsExtHost.end());
        
        if (extHostStr.empty()) extHostStr = "localhost";
        
        // Kill remote broadcast server
        std::string killRemoteCmd = "ssh -o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=no root@" + extHostStr + " \"pkill -f broadcast.py\"";
        
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        
        PROCESS_INFORMATION pi = {};
        if (CreateProcessA(NULL, (LPSTR)killRemoteCmd.c_str(), NULL, NULL, FALSE, 
                          CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 10000); // 10 second timeout
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            AppendLog("Remote broadcast server stopped");
        } else {
            AppendLog("Could not stop remote broadcast server");
        }
        
        // Clean up remote temp file
        std::string cleanupCmd = "ssh -o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=no root@" + extHostStr + " \"rm -f /tmp/broadcast.py /tmp/broadcast.log\"";
        if (CreateProcessA(NULL, (LPSTR)cleanupCmd.c_str(), NULL, NULL, FALSE, 
                          CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            AppendLog("Remote files cleaned up");
        }
        
        // Stop local clipboard client
        if (piClient.hProcess) {
            TerminateProcess(piClient.hProcess, 0);
            WaitForSingleObject(piClient.hProcess, 3000);
            CloseHandle(piClient.hProcess);
            AppendLog("Local client stopped");
        }
        
        // Close SSH process handle if we have it
        if (piBroadcast.hProcess) {
            CloseHandle(piBroadcast.hProcess);
        }
        
    } else {
        // Local server mode - original logic
        HANDLE processes[2] = { piBroadcast.hProcess, piClient.hProcess };

        for (int i = 0; i < 2; ++i) {
            if (processes[i]) {
                TerminateProcess(processes[i], 0);
                WaitForSingleObject(processes[i], 3000);
                CloseHandle(processes[i]);
            }
        }
    }

    // Common cleanup
    if (piBroadcast.hThread) CloseHandle(piBroadcast.hThread);
    if (piClient.hThread) CloseHandle(piClient.hThread);

    piBroadcast = {};
    piClient = {};
    processesStarted = false;

    CleanupTempFiles();
    AppendLog("All processes stopped");
    UpdateButtonStates();
}

std::string writeTempFile(const std::string& filename, const char* content) {
    std::ofstream out(filename);
    out << content;
    out.close();
    return filename;
}

void StartPythonProcesses(const std::string& host, const std::string& user) {
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE readPipe, writePipe;
    CreatePipe(&readPipe, &writePipe, &sa, 0);
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // Determine the server URL to use
    std::string serverUrl;
    BOOL isExternal = (SendMessage(hwndChkExternal, BM_GETCHECK, 0, 0) == BST_CHECKED);
    
    if (isExternal) {
        // External server mode - SSH setup
        wchar_t extHost[256], extPort[16];
        GetWindowTextW(hwndInputExtHost, extHost, 256);
        GetWindowTextW(hwndInputExtPort, extPort, 16);
        
        std::wstring wsExtHost(extHost);
        std::wstring wsExtPort(extPort);
        std::string extHostStr(wsExtHost.begin(), wsExtHost.end());
        std::string extPortStr(wsExtPort.begin(), wsExtPort.end());
        
        if (extHostStr.empty()) extHostStr = "localhost";
        if (extPortStr.empty()) extPortStr = "8000";
        
        serverUrl = "http://" + extHostStr + ":" + extPortStr;
        AppendLog("Setting up external server: " + extHostStr);

        // Create temp files locally first
        broadcastFilePath = writeTempFile("broadcast.py", broadcast_py);
        clipboardFilePath = writeTempFile("clipboard.py", clipboard_py);

        // Startup info with HIDDEN window
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.hStdOutput = si.hStdError = writePipe;
        si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};

        // Step 1: Test SSH connection
        std::string sshTestCmd = "ssh -o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=no root@" + extHostStr + " \"echo SSH connection successful\"";
        AppendLog("Testing SSH connection...");
        
        if (!CreateProcessA(NULL, (LPSTR)sshTestCmd.c_str(), NULL, NULL, TRUE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            AppendLog("Failed to test SSH connection");
            MessageBoxA(NULL, "Failed to test SSH connection", "Error", MB_OK | MB_ICONERROR);
            return;
        }
        
        WaitForSingleObject(pi.hProcess, 30000); // 30 second timeout
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        if (exitCode != 0) {
            AppendLog("SSH connection failed - check your SSH keys and host");
            MessageBoxA(NULL, "SSH connection failed. Please ensure:\n- SSH keys are set up\n- Host is reachable\n- Root access is available", "SSH Error", MB_OK | MB_ICONERROR);
            return;
        }
        
        AppendLog("SSH connection successful");

        // Step 2: Install Python dependencies on remote server
        std::string remotePipCmd = "ssh -o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=no root@" + extHostStr + " \"cd /tmp && python3 -m venv clipenv && source clipenv/bin/activate && pip install flask requests\"";
        AppendLog("Installing dependencies on remote server...");
        
        if (!CreateProcessA(NULL, (LPSTR)remotePipCmd.c_str(), NULL, NULL, TRUE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            AppendLog("Failed to install remote dependencies");
            return;
        }
        
        WaitForSingleObject(pi.hProcess, 60000); // 60 second timeout for pip install
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        if (exitCode != 0) {
            AppendLog("Warning: Remote pip install may have failed, continuing anyway...");
        } else {
            AppendLog("Remote dependencies installed");
        }

        // Step 3: Copy broadcast.py to remote server
        std::string scpCmd = "scp -o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=no \"" + broadcastFilePath + "\" root@" + extHostStr + ":/tmp/broadcast.py";
        AppendLog("Copying broadcast.py to remote server...");
        
        if (!CreateProcessA(NULL, (LPSTR)scpCmd.c_str(), NULL, NULL, TRUE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            AppendLog("Failed to copy broadcast.py to remote server");
            return;
        }
        
        WaitForSingleObject(pi.hProcess, 30000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        if (exitCode != 0) {
            AppendLog("Failed to copy broadcast.py - check SCP access");
            return;
        }
        
        AppendLog("broadcast.py copied to remote server");

        // Step 4: Start broadcast.py on remote server
        std::string remoteBroadcastCmd = "ssh -o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=no root@" + extHostStr + " \"cd /tmp && source clipenv/bin/activate && nohup python broadcast.py > broadcast.log 2>&1 &\"";
        AppendLog("Starting broadcast server on remote host...");
        
        if (!CreateProcessA(NULL, (LPSTR)remoteBroadcastCmd.c_str(), NULL, NULL, TRUE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &piBroadcast)) {
            AppendLog("Failed to start remote broadcast server");
            return;
        }
        
        // Give the remote server a moment to start
        Sleep(2000);
        AppendLog("Remote broadcast server started");

        // Step 5: Install local dependencies for clipboard client
        std::string localPipCmd = "pip install pywin32 requests";
        AppendLog("Installing local dependencies...");
        
        if (!CreateProcessA(NULL, (LPSTR)localPipCmd.c_str(), NULL, NULL, TRUE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            AppendLog("Failed to install local dependencies");
            return;
        }
        
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        if (exitCode != 0) {
            AppendLog("Warning: Local pip install may have failed, continuing anyway...");
        } else {
            AppendLog("Local dependencies installed");
        }

        // Step 6: Start local clipboard client
        std::string localClientCmd = "python \"" + clipboardFilePath + "\" \"" + serverUrl + "\" " + user;
        AppendLog("Starting local clipboard client...");
        
        if (!CreateProcessA(NULL, (LPSTR)localClientCmd.c_str(), NULL, NULL, TRUE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &piClient)) {
            AppendLog("Failed to start local clipboard client");
            return;
        }
        
        AppendLog("Local clipboard client started");
        AppendLog("External server setup complete!");        
    } else {
        // Local server mode (original logic)
        serverUrl = host;
        AppendLog("Using local server mode: " + serverUrl);

        // Create temp files
        broadcastFilePath = writeTempFile("broadcast.py", broadcast_py);
        clipboardFilePath = writeTempFile("clipboard.py", clipboard_py);
        
        // Construct command lines
        std::string cmd1 = "python \"" + broadcastFilePath + "\"";
        std::string cmd2 = "python \"" + clipboardFilePath + "\" \"" + serverUrl + "\" " + user;
        std::string pipCmd = "pip install pywin32 requests flask";

        // Startup info with HIDDEN window
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.hStdOutput = si.hStdError = writePipe;
        si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        
        // Install dependencies locally
        if (!CreateProcessA(NULL, (LPSTR)pipCmd.c_str(), NULL, NULL, TRUE, 
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            MessageBoxA(NULL, "Failed to run pip install", "Error", MB_OK | MB_ICONERROR);
            return;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (exitCode != 0) {
            MessageBoxA(NULL, "pip install failed", "Error", MB_OK | MB_ICONERROR);
            return;
        } else {
            AppendLog("Dependencies installed, starting Python processes...");
            
            // Start broadcast.py locally
            if (CreateProcessA(NULL, (LPSTR)cmd1.c_str(), NULL, NULL, TRUE, 
                              CREATE_NO_WINDOW, NULL, NULL, &si, &piBroadcast)) {
                AppendLog("Local broadcast server started");
                
                // Start clipboard.py locally
                if (CreateProcessA(NULL, (LPSTR)cmd2.c_str(), NULL, NULL, TRUE, 
                                  CREATE_NO_WINDOW, NULL, NULL, &si, &piClient)) {
                    AppendLog("Local clipboard client started");
                } else {
                    AppendLog("Failed to start local clipboard client");
                }        
            } else {
                AppendLog("Failed to start local broadcast server");
            }
        }
    }

    processesStarted = true;
    CloseHandle(writePipe);
    UpdateButtonStates();   
    
    // Start thread to read process output
    std::thread(ReadPipeAndLog, readPipe).detach();
}

void KillProcesses() {
    if (processesStarted) {
        TerminateProcess(piBroadcast.hProcess, 0);
        TerminateProcess(piClient.hProcess, 0);
        processesStarted = false;
    }
}

// Tray message handler
LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_SHOW:
                    ShowWindow(hwnd, SW_SHOW);
                    break;
            }
            break;

        case WM_USER + 1:
            if (lParam == WM_LBUTTONUP) {
                // Left click - show/restore window
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            } else if (lParam == WM_RBUTTONUP) {
                // Right click - show context menu
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hTrayMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            }
            break;

        case WM_DESTROY:
            if (hLogBoxFont) {
                DeleteObject(hLogBoxFont);
                hLogBoxFont = NULL;
            }
            if (hLogBoxBrush) {
                DeleteObject(hLogBoxBrush);
                hLogBoxBrush = NULL;
            }
            if (hBackgroundBrush) {
                DeleteObject(hBackgroundBrush);
                hBackgroundBrush = NULL;
            }
            StopProcesses();
            Shell_NotifyIcon(NIM_DELETE, &nid);
            if (hMutex) {
                ReleaseMutex(hMutex);
                CloseHandle(hMutex);
            }
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

void AddTrayIcon(HWND hwnd) {
    hTrayMenu = CreatePopupMenu();
    AppendMenu(hTrayMenu, MF_STRING, ID_TRAY_SHOW, L"Show");
    AppendMenu(hTrayMenu, MF_STRING, ID_TRAY_EXIT, L"Quit");

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 1;
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ALT_ICON));
    wcscpy_s(nid.szTip, ARRAYSIZE(nid.szTip), L"Spill Clipboard Server");

    Shell_NotifyIcon(NIM_ADD, &nid);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_SHOW:
            // Message from another instance to show this window
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            // Prevent window resizing by setting min and max size to the same value
            LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
            lpMMI->ptMinTrackSize.x = WINDOW_WIDTH;
            lpMMI->ptMinTrackSize.y = WINDOW_HEIGHT;
            lpMMI->ptMaxTrackSize.x = WINDOW_WIDTH;
            lpMMI->ptMaxTrackSize.y = WINDOW_HEIGHT;
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            // Make static text controls have transparent background
            if ((HWND)lParam == hwndVersionLabel) {
                SetTextColor((HDC)wParam, versionLabelColor);
                SetBkMode((HDC)wParam, TRANSPARENT);
                return (LRESULT)hBackgroundBrush;
            }
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)hBackgroundBrush;

        case WM_CTLCOLOREDIT:
            // Handle edit control colors
            if ((HWND)lParam == hwndLogBox) {
                // Special styling for log box
                SetTextColor((HDC)wParam, RGB(0, 0, 0));  // Black text
                SetBkColor((HDC)wParam, RGB(200, 230, 200));  // Light pistachio green
                return (LRESULT)hLogBoxBrush;
            }
            // For all other edit controls, use default white background
            SetTextColor((HDC)wParam, RGB(0, 0, 0));  // Black text
            SetBkColor((HDC)wParam, RGB(255, 255, 255));  // White background
            return (LRESULT)GetStockObject(WHITE_BRUSH);

        case WM_CTLCOLORDLG:
            // Set background color for the dialog itself
            return (LRESULT)hBackgroundBrush;

        case WM_ERASEBKGND:
            // Paint the background
            {
                RECT rect;
                GetClientRect(hwnd, &rect);
                FillRect((HDC)wParam, &rect, hBackgroundBrush);
                return 1;
            }

        case WM_CREATE: {
            // Create background brushes and font
            hBackgroundBrush = CreateSolidBrush(RGB(171, 201, 158));
            hLogBoxBrush = CreateSolidBrush(RGB(200, 230, 200));
            
            hLogBoxFont = CreateFontW(
                -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas"
            );
            
            // GET THE INSTANCE HANDLE FIRST!
            HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
            
            CreateWindowW(L"STATIC", L"Host URL:", WS_VISIBLE | WS_CHILD, 10, 10, 80, 20, hwnd, NULL, hInst, NULL);
            hwndInputHost = CreateWindowW(L"EDIT", L"http://localhost:8000", 
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | WS_TABSTOP | ES_AUTOHSCROLL, 
                100, 10, 200, 20, hwnd, NULL, hInst, NULL);  // <- NOW WITH hInst!

            CreateWindowW(L"STATIC", L"Username:", WS_VISIBLE | WS_CHILD, 10, 40, 80, 20, hwnd, NULL, hInst, NULL);
            hwndInputUser = CreateWindowW(L"EDIT", L"user1", 
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | WS_TABSTOP | ES_AUTOHSCROLL, 
                100, 40, 200, 20, hwnd, NULL, hInst, NULL);  // <- NOW WITH hInst!

            // External server checkbox
            hwndChkExternal = CreateWindowW(L"BUTTON", L"External Server", 
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP, 
                10, 70, 120, 20, hwnd, (HMENU)ID_CHK_EXTERNAL, hInst, NULL);

            // External server labels
            hwndLabelExtHost = CreateWindowW(L"STATIC", L"Ext Host:", WS_VISIBLE | WS_CHILD, 10, 100, 80, 20, hwnd, NULL, hInst, NULL);
            hwndLabelExtPort = CreateWindowW(L"STATIC", L"Port:", WS_VISIBLE | WS_CHILD, 230, 100, 40, 20, hwnd, NULL, hInst, NULL);

            // Your existing external edit controls are already correct
            hwndInputExtHost = CreateWindowExW(
                0, L"EDIT", L"192.168.1.100",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | WS_TABSTOP | ES_AUTOHSCROLL,
                100, 100, 120, 20, hwnd, NULL, hInst, NULL);

            hwndInputExtPort = CreateWindowExW(
                0, L"EDIT", L"8000",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | WS_TABSTOP | ES_AUTOHSCROLL,
                270, 100, 60, 20, hwnd, NULL, hInst, NULL);

            hwndBtnStart = CreateWindowW(L"BUTTON", L"Start", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 100, 130, 80, 25, hwnd, (HMENU)ID_BTN_START, hInst, NULL);
            hwndBtnStop = CreateWindowW(L"BUTTON", L"Stop", WS_VISIBLE | WS_CHILD | WS_TABSTOP, 190, 130, 80, 25, hwnd, (HMENU)ID_BTN_STOP, hInst, NULL);

            hwndLogBox = CreateWindowW(L"EDIT", L"", 
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY,
                10, 170, 370, 120, hwnd, NULL, hInst, NULL);

            hwndVersionLabel = CreateWindowW(L"STATIC", L"inversepolarity v0.0.3a", 
                                           WS_VISIBLE | WS_CHILD | SS_RIGHT, 
                                           200, 300, 180, 20, hwnd, NULL, hInst, NULL);
            // Subclass ONLY the log box
            originalEditProc = (WNDPROC)SetWindowLongPtr(hwndLogBox, GWLP_WNDPROC, (LONG_PTR)LogBoxProc);

            if (hLogBoxFont) {
                SendMessage(hwndLogBox, WM_SETFONT, (WPARAM)hLogBoxFont, TRUE);
            }

            HWND hwndIcon = CreateWindowW(L"BUTTON", NULL, 
                                        WS_VISIBLE | WS_CHILD | BS_ICON | BS_FLAT, 
                                        320, 10, 64, 64, hwnd, (HMENU)ID_ICON_BUTTON, NULL, NULL);

            HICON hIcon = (HICON)LoadImage(GetModuleHandle(NULL), 
                                           MAKEINTRESOURCE(IDI_MAIN_ICON),
                                           IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR);
            SendMessage(hwndIcon, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hIcon);

            UpdateExternalServerState();
            UpdateButtonStates();
            AddTrayIcon(hwnd);
        } break;

        case WM_APP_EXIT:
            if (hBackgroundBrush) {
                DeleteObject(hBackgroundBrush);
                hBackgroundBrush = NULL;
            }
            StopProcesses();
            Shell_NotifyIcon(NIM_DELETE, &nid);
            if (hMutex) {
                ReleaseMutex(hMutex);
                CloseHandle(hMutex);
            }
            DestroyWindow(hwnd);  // this triggers WM_DESTROY → PostQuitMessage
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_ICON_BUTTON: {
                    std::string urlToOpen;
                    
                    // Check if external server mode is enabled
                    BOOL isExternal = (SendMessage(hwndChkExternal, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    
                    if (isExternal) {
                        // External server mode - build URL from external host and port
                        wchar_t extHost[256], extPort[16];
                        GetWindowTextW(hwndInputExtHost, extHost, 256);
                        GetWindowTextW(hwndInputExtPort, extPort, 16);
                        
                        std::wstring wsExtHost(extHost);
                        std::wstring wsExtPort(extPort);
                        std::string extHostStr(wsExtHost.begin(), wsExtHost.end());
                        std::string extPortStr(wsExtPort.begin(), wsExtPort.end());
                        
                        // Use defaults if empty
                        if (extHostStr.empty()) extHostStr = "localhost";
                        if (extPortStr.empty()) extPortStr = "8000";
                        
                        // Build the external server URL
                        urlToOpen = "http://" + extHostStr + ":" + extPortStr;
                    } else {
                        // Local server mode - use the host URL field
                        wchar_t host[256];
                        GetWindowTextW(hwndInputHost, host, 256);
                        std::wstring wsHost(host);
                        urlToOpen = std::string(wsHost.begin(), wsHost.end());
                    }
                    
                    // Open the URL in the default browser
                    ShellExecuteA(NULL, "open", urlToOpen.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    break;
                }               
                case ID_BTN_START: {
                    wchar_t host[256], user[256];
                    GetWindowTextW(hwndInputHost, host, 256);
                    GetWindowTextW(hwndInputUser, user, 256);

                    std::wstring wsHost(host);
                    std::wstring wsUser(user);
                    std::string hostStr(wsHost.begin(), wsHost.end());
                    std::string userStr(wsUser.begin(), wsUser.end());

                    StartPythonProcesses(hostStr, userStr);
                    break;
                }

                case ID_BTN_STOP: {
                    StopProcesses();
                    break;
                }

                case ID_CHK_EXTERNAL: {
                    UpdateExternalServerState();
                    break;
                }

                case ID_TRAY_SHOW:
                    ShowWindow(hwnd, SW_SHOW);
                    break;

                case ID_TRAY_EXIT:
                    StopProcesses();                        // ⛔ Kill subprocesses
                    Shell_NotifyIcon(NIM_DELETE, &nid);     // 🧹 Remove tray icon
                    if (hMutex) {
                        ReleaseMutex(hMutex);
                        CloseHandle(hMutex);
                    }
                    PostMessage(hwnd, WM_APP_EXIT, 0, 0);   // 🔁 Trigger APP_EXIT
                    break;
            }
            break;

        case WM_CLOSE:
            {
                const wchar_t* message = 
                    L"What would you like to do?\n\n"
                    L"• 'Yes' -> Quit application\n"
                    L"• 'No' -> Minimize to system tray\n"
                    L"• 'Cancel' -> Return to application";
                    
                int response = MessageBoxW(hwnd, message, L"spill", 
                                          MB_YESNOCANCEL | MB_ICONQUESTION);
                                          
                if (response == IDYES) {
                    // Quit application
                    StopProcesses();
                    Shell_NotifyIcon(NIM_DELETE, &nid);
                    if (hMutex) {
                        ReleaseMutex(hMutex);
                        CloseHandle(hMutex);
                    }
                    PostMessage(hwnd, WM_APP_EXIT, 0, 0);
                }
                else if (response == IDNO) {
                    // Minimize to tray
                    ShowWindow(hwnd, SW_HIDE);
                }
                // IDCANCEL = Do nothing
            }
            return 0;            
        default:
            return TrayProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {

    // Create mutex for single instance enforcement
    hMutex = CreateMutexW(NULL, TRUE, APP_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance is already running
        CloseHandle(hMutex);
        
        // Try to find and activate the existing window
        HWND existingWindow = FindWindowW(APP_WINDOW_CLASS, NULL);
        if (existingWindow) {
            // Send message to show the existing window
            PostMessage(existingWindow, WM_APP_SHOW, 0, 0);
        } else {
            // Show a message if we can't find the window
            MessageBoxW(NULL, 
                       L"spill is already running.\n\nCheck your system tray for the application icon.", 
                       L"Already Running", 
                       MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }

    const wchar_t CLASS_NAME[] = APP_WINDOW_CLASS;

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);  // Standard dialog background color
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MAIN_ICON));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    // Create window with fixed size and no maximize button
    HWND hwnd = CreateWindowEx(0, CLASS_NAME, L"spill", 
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH, WINDOW_HEIGHT, 
                               NULL, NULL, hInst, NULL);

    if (!hwnd) {
        if (hMutex) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
        return -1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);  // Converts WM_KEYDOWN to WM_CHAR
        DispatchMessage(&msg);
    }

    // Cleanup mutex on exit
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return 0;
}