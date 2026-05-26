#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define TIMER_ID 1

static NOTIFYICONDATA nid = {};
static HICON trayIcon = nullptr;
static bool pulse = false;

static ULONGLONG prevIdle = 0;
static ULONGLONG prevKernel = 0;
static ULONGLONG prevUser = 0;
static int cpuPercent = -1;
static int ramPercent = -1;

static PDH_HQUERY gpuQuery = nullptr;
static PDH_HCOUNTER gpuCounter = nullptr;
static bool gpuReady = false;
static int gpuPercent = -1;

ULONGLONG FileTimeToUInt64(const FILETIME& ft)
{
   ULARGE_INTEGER uli;
   uli.LowPart = ft.dwLowDateTime;
   uli.HighPart = ft.dwHighDateTime;
   return uli.QuadPart;
}

int GetCpuPercent()
{
   FILETIME idleTime;
   FILETIME kernelTime;
   FILETIME userTime;

   if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
      return -1;

   ULONGLONG idle = FileTimeToUInt64(idleTime);
   ULONGLONG kernel = FileTimeToUInt64(kernelTime);
   ULONGLONG user = FileTimeToUInt64(userTime);

   if (prevIdle == 0 && prevKernel == 0 && prevUser == 0)
   {
      prevIdle = idle;
      prevKernel = kernel;
      prevUser = user;
      return -1;
   }

   ULONGLONG idleDelta = idle - prevIdle;
   ULONGLONG kernelDelta = kernel - prevKernel;
   ULONGLONG userDelta = user - prevUser;

   prevIdle = idle;
   prevKernel = kernel;
   prevUser = user;

   ULONGLONG total = kernelDelta + userDelta;

   if (total == 0)
      return 0;

   int percent = static_cast<int>((total - idleDelta) * 100 / total);

   if (percent < 0)
      return 0;

   if (percent > 100)
      return 100;

   return percent;
}

int GetRamPercent()
{
   MEMORYSTATUSEX memory = {};
   memory.dwLength = sizeof(memory);

   if (!GlobalMemoryStatusEx(&memory))
      return -1;

   return static_cast<int>(memory.dwMemoryLoad);
}

bool InitGpuCounter()
{
   if (PdhOpenQuery(nullptr, 0, &gpuQuery) != ERROR_SUCCESS)
      return false;

   if (PdhAddEnglishCounterW(
      gpuQuery,
      L"\\GPU Engine(*)\\Utilization Percentage",
      0,
      &gpuCounter) != ERROR_SUCCESS)
   {
      PdhCloseQuery(gpuQuery);
      gpuQuery = nullptr;
      gpuCounter = nullptr;
      return false;
   }

   PdhCollectQueryData(gpuQuery);
   return true;
}

int GetGpuPercent()
{
   if (!gpuQuery || !gpuCounter)
      return -1;

   if (PdhCollectQueryData(gpuQuery) != ERROR_SUCCESS)
      return -1;

   DWORD bufferSize = 0;
   DWORD itemCount = 0;

   PDH_STATUS status = PdhGetFormattedCounterArrayW(
      gpuCounter,
      PDH_FMT_DOUBLE,
      &bufferSize,
      &itemCount,
      nullptr
   );

   if (status != PDH_MORE_DATA)
      return -1;

   auto items = static_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(malloc(bufferSize));

   if (!items)
      return -1;

   status = PdhGetFormattedCounterArrayW(
      gpuCounter,
      PDH_FMT_DOUBLE,
      &bufferSize,
      &itemCount,
      items
   );

   if (status != ERROR_SUCCESS)
   {
      free(items);
      return -1;
   }

   double maxValue = 0.0;

   for (DWORD i = 0; i < itemCount; i++)
   {
      double value = items[i].FmtValue.doubleValue;

      if (value > maxValue)
         maxValue = value;
   }

   free(items);

   if (maxValue < 0.0)
      maxValue = 0.0;

   if (maxValue > 100.0)
      maxValue = 100.0;

   return static_cast<int>(maxValue + 0.5);
}

void CleanupGpuCounter()
{
   if (gpuQuery)
   {
      PdhCloseQuery(gpuQuery);
      gpuQuery = nullptr;
      gpuCounter = nullptr;
   }
}

HICON CreateDotIcon(int cpuPercent, bool bright)
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

	// Map CPU percent to a red color intensity
   int level = cpuPercent;

   if (level < 0)
      level = 0;

   if (level > 100)
      level = 100;

   int baseRed = 80 + (level * 175 / 100);
   int red = bright ? baseRed : 20 + (level * 60 / 100);

   HBRUSH dot = CreateSolidBrush(RGB(red, 0, 0));
   
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

   int dotPercent = cpuPercent;

   if (gpuPercent > dotPercent)
      dotPercent = gpuPercent;

   trayIcon = CreateDotIcon(dotPercent, pulse);

   nid.hIcon = trayIcon;

   if (cpuPercent >= 0 && ramPercent >= 0 && gpuPercent >= 0)
      swprintf_s(nid.szTip, L"CPU %d%%\nGPU %d%%\nRAM %d%%", cpuPercent, gpuPercent, ramPercent);
   else if (cpuPercent >= 0 && ramPercent >= 0)
      swprintf_s(nid.szTip, L"CPU %d%%\nGPU ...\nRAM %d%% ", cpuPercent, ramPercent);
   else
      wcscpy_s(nid.szTip, L"CPU ...\nGPU ...\nRAM ...");

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
      trayIcon = CreateDotIcon(0, true);

      nid.cbSize = sizeof(nid);
      nid.hWnd = hwnd;
      nid.uID = 1;
      nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
      nid.uCallbackMessage = WM_TRAYICON;
      nid.hIcon = trayIcon;
      wcscpy_s(nid.szTip, L"CPU ...");

      Shell_NotifyIcon(NIM_ADD, &nid);
      SetTimer(hwnd, TIMER_ID, 1000, nullptr);
      gpuReady = InitGpuCounter();

      return 0;

   case WM_TIMER:
      cpuPercent = GetCpuPercent();
      ramPercent = GetRamPercent();

      if (gpuReady)
         gpuPercent = GetGpuPercent();

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

      CleanupGpuCounter();
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
      L"red dot",
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