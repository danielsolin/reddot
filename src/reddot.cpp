#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define TIMER_ID 1

static NOTIFYICONDATA nid = {};
static HICON trayIcon = nullptr;
static bool pulse = false;

HICON CreateDotIcon(bool bright)
{
   const int size = 32;

   HDC screen = GetDC(nullptr);
   HDC mem = CreateCompatibleDC(screen);

   HBITMAP color = CreateCompatibleBitmap(screen, size, size);
   HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);

   HGDIOBJ old = SelectObject(mem, color);

   HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
   RECT rc{ 0, 0, size, size };
   FillRect(mem, &rc, bg);
   DeleteObject(bg);

   HBRUSH dot = CreateSolidBrush(bright ? RGB(255, 0, 0) : RGB(120, 0, 0));
   HGDIOBJ oldBrush = SelectObject(mem, dot);
   Ellipse(mem, 10, 10, 22, 22);
   SelectObject(mem, oldBrush);
   DeleteObject(dot);

   SelectObject(mem, old);

   ICONINFO ii = {};
   ii.fIcon = TRUE;
   ii.hbmColor = color;
   ii.hbmMask = mask;

   HICON icon = CreateIconIndirect(&ii);

   DeleteObject(color);
   DeleteObject(mask);
   DeleteDC(mem);
   ReleaseDC(nullptr, screen);

   return icon;
}

void UpdateTrayIcon(HWND hwnd)
{
   if (trayIcon)
      DestroyIcon(trayIcon);

   trayIcon = CreateDotIcon(pulse);

   nid.hIcon = trayIcon;
   wcscpy_s(nid.szTip, L"Red Dot");

   Shell_NotifyIcon(NIM_MODIFY, &nid);
}

void ShowTrayMenu(HWND hwnd)
{
   POINT pt;
   GetCursorPos(&pt);

   HMENU menu = CreatePopupMenu();
   AppendMenu(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

   SetForegroundWindow(hwnd);

   TrackPopupMenu(
      menu,
      TPM_RIGHTBUTTON,
      pt.x,
      pt.y,
      0,
      hwnd,
      nullptr
   );

   DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
   switch (msg)
   {
   case WM_CREATE:
      trayIcon = CreateDotIcon(true);

      nid.cbSize = sizeof(nid);
      nid.hWnd = hwnd;
      nid.uID = 1;
      nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
      nid.uCallbackMessage = WM_TRAYICON;
      nid.hIcon = trayIcon;
      wcscpy_s(nid.szTip, L"Red Dot");

      Shell_NotifyIcon(NIM_ADD, &nid);
      SetTimer(hwnd, TIMER_ID, 1000, nullptr);
      return 0;

   case WM_TIMER:
      pulse = !pulse;
      UpdateTrayIcon(hwnd);
      return 0;

   case WM_TRAYICON:
      if (lp == WM_RBUTTONUP)
         ShowTrayMenu(hwnd);
      return 0;

   case WM_COMMAND:
      if (LOWORD(wp) == ID_TRAY_EXIT)
         DestroyWindow(hwnd);
      return 0;

   case WM_DESTROY:
      KillTimer(hwnd, TIMER_ID);
      Shell_NotifyIcon(NIM_DELETE, &nid);

      if (trayIcon)
         DestroyIcon(trayIcon);

      PostQuitMessage(0);
      return 0;
   }

   return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
   const wchar_t CLASS_NAME[] = L"reddot_window";

   WNDCLASS wc = {};
   wc.lpfnWndProc = WndProc;
   wc.hInstance = instance;
   wc.lpszClassName = CLASS_NAME;

   RegisterClass(&wc);

   HWND hwnd = CreateWindowEx(
      0,
      CLASS_NAME,
      L"Red Dot",
      0,
      0, 0, 0, 0,
      nullptr,
      nullptr,
      instance,
      nullptr
   );

   MSG msg;
   while (GetMessage(&msg, nullptr, 0, 0))
   {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
   }

   return 0;
}