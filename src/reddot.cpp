#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <pdh.h>
#include <pdhmsg.h>

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define TIMER_ID 1
#define TOOLTIP_TIMER_ID 2
#define TOOLTIP_PADDING_X 10
#define TOOLTIP_PADDING_Y 7

static NOTIFYICONDATA nid = {};
static HWND mainWindow = nullptr;
static HICON trayIcon = nullptr;
static HWND tooltipWindow = nullptr;
static wchar_t tooltipText[200] = L"CPU ...\nGPU ...\nRAM ...\nHDD ...\nNET ...";
static bool pulse = false;

static ULONGLONG prevIdle = 0;
static ULONGLONG prevKernel = 0;
static ULONGLONG prevUser = 0;
static int cpuPercent = -1;
static int ramPercent = -1;
static int diskPercent = -1;
static int netPercent = -1;

static PDH_HQUERY gpuQuery = nullptr;
static PDH_HCOUNTER cpuCounter = nullptr;
static PDH_HCOUNTER diskCounter = nullptr;
static PDH_HCOUNTER netBytesCounter = nullptr;
static PDH_HCOUNTER gpuCounter = nullptr;
static bool gpuReady = false;
static int gpuPercent = -1;

void FormatStatusText(wchar_t* buffer, size_t size)
{
   (void)size;

   wchar_t cpu[16];
   wchar_t gpu[16];
   wchar_t ram[16];
   wchar_t disk[16];
   wchar_t net[16];

   if (cpuPercent >= 0)
      wsprintfW(cpu, L"%d%%", cpuPercent);
   else
      lstrcpynW(cpu, L"...", _countof(cpu));

   if (gpuPercent >= 0)
      wsprintfW(gpu, L"%d%%", gpuPercent);
   else
      lstrcpynW(gpu, L"...", _countof(gpu));

   if (ramPercent >= 0)
      wsprintfW(ram, L"%d%%", ramPercent);
   else
      lstrcpynW(ram, L"...", _countof(ram));

   if (diskPercent >= 0)
      wsprintfW(disk, L"%d%%", diskPercent);
   else
      lstrcpynW(disk, L"...", _countof(disk));

   if (netPercent >= 0)
      wsprintfW(net, L"%d%%", netPercent);
   else
      lstrcpynW(net, L"...", _countof(net));

   wsprintfW(buffer, L"CPU %s\nGPU %s\nRAM %s\nHDD %s\nNET %s",
      cpu, gpu, ram, disk, net);
}

void MeasureText(HDC hdc, const wchar_t* text, SIZE* size)
{
   RECT rc = { 0, 0, 1, 1 };

   DrawTextW(
      hdc,
      text,
      -1,
      &rc,
      DT_CALCRECT | DT_LEFT | DT_NOPREFIX
   );

   size->cx = rc.right - rc.left;
   size->cy = rc.bottom - rc.top;
}

void MoveTooltipNearCursor(HWND hwnd)
{
   POINT pt;
   GetCursorPos(&pt);

   HDC hdc = GetDC(hwnd);
   HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
   HGDIOBJ oldFont = SelectObject(hdc, font);

   SIZE textSize = {};
   MeasureText(hdc, tooltipText, &textSize);

   SelectObject(hdc, oldFont);
   ReleaseDC(hwnd, hdc);

   int width = textSize.cx + TOOLTIP_PADDING_X * 2;
   int height = textSize.cy + TOOLTIP_PADDING_Y * 2;
   int x = pt.x + 14;
   int y = pt.y - height - 8;

   HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
   MONITORINFO mi = {};
   mi.cbSize = sizeof(mi);

   if (GetMonitorInfoW(monitor, &mi))
   {
      if (x + width > mi.rcWork.right)
         x = pt.x - width - 14;

      if (y < mi.rcWork.top)
         y = pt.y + 18;
   }

   SetWindowPos(
      hwnd,
      HWND_TOPMOST,
      x,
      y,
      width,
      height,
      SWP_NOACTIVATE
   );
}

LRESULT CALLBACK TooltipProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
   switch (msg)
   {
      case WM_PAINT:
      {
         PAINTSTRUCT ps;
         HDC hdc = BeginPaint(hwnd, &ps);

         RECT rc;
         GetClientRect(hwnd, &rc);

         HBRUSH bg = CreateSolidBrush(RGB(16, 16, 16));
         FillRect(hdc, &rc, bg);
         DeleteObject(bg);

         HPEN border = CreatePen(PS_SOLID, 1, RGB(210, 40, 40));
         HGDIOBJ oldPen = SelectObject(hdc, border);
         HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
         Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
         SelectObject(hdc, oldBrush);
         SelectObject(hdc, oldPen);
         DeleteObject(border);

         HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
         HGDIOBJ oldFont = SelectObject(hdc, font);

         SetBkMode(hdc, TRANSPARENT);
         SetTextColor(hdc, RGB(245, 245, 245));

         RECT textRc = {
            TOOLTIP_PADDING_X,
            TOOLTIP_PADDING_Y,
            rc.right - TOOLTIP_PADDING_X,
            rc.bottom - TOOLTIP_PADDING_Y
         };

         DrawTextW(hdc, tooltipText, -1, &textRc, DT_LEFT | DT_NOPREFIX);

         SelectObject(hdc, oldFont);
         EndPaint(hwnd, &ps);
         return 0;
      }
   }

   return DefWindowProc(hwnd, msg, wp, lp);
}

void ShowCustomTooltip(HWND owner)
{
   FormatStatusText(tooltipText, _countof(tooltipText));
   mainWindow = owner;

   if (!tooltipWindow)
   {
      tooltipWindow = CreateWindowExW(
         WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
         L"reddot_tooltip",
         L"",
         WS_POPUP,
         0, 0, 0, 0,
         owner,
         nullptr,
         GetModuleHandleW(nullptr),
         nullptr
      );
   }

   if (!tooltipWindow)
      return;

   MoveTooltipNearCursor(tooltipWindow);
   ShowWindow(tooltipWindow, SW_SHOWNOACTIVATE);
   InvalidateRect(tooltipWindow, nullptr, TRUE);
   SetTimer(owner, TOOLTIP_TIMER_ID, 75, nullptr);
}

void HideCustomTooltip()
{
   if (tooltipWindow)
      ShowWindow(tooltipWindow, SW_HIDE);

   if (mainWindow)
      KillTimer(mainWindow, TOOLTIP_TIMER_ID);
}

bool IsCursorOverTrayIcon(HWND hwnd)
{
   NOTIFYICONIDENTIFIER iconId = {};
   iconId.cbSize = sizeof(iconId);
   iconId.hWnd = hwnd;
   iconId.uID = nid.uID;

   RECT iconRect;

   if (Shell_NotifyIconGetRect(&iconId, &iconRect) != S_OK)
      return false;

   InflateRect(&iconRect, 2, 2);

   POINT pt;
   GetCursorPos(&pt);

   return PtInRect(&iconRect, pt);
}

void UpdateCustomTooltipVisibility(HWND hwnd)
{
   if (IsCursorOverTrayIcon(hwnd))
      ShowCustomTooltip(hwnd);
   else
      HideCustomTooltip();
}

//Convert FILETIME to ULONGLONG for easier calculations
ULONGLONG FileTimeToUInt64(const FILETIME& ft)
{
   ULARGE_INTEGER uli;
   uli.LowPart = ft.dwLowDateTime;
   uli.HighPart = ft.dwHighDateTime;
   return uli.QuadPart;
}

// Get CPU usage percentage since the last call
// "Time" is not real time, but the amount of time
// the CPU has spent in different states since boot.
int GetCpuPercent()
{
   if (cpuCounter)
   {
      PDH_FMT_COUNTERVALUE value = {};

      if (PdhGetFormattedCounterValue(
         cpuCounter,
         PDH_FMT_DOUBLE,
         nullptr,
         &value) == ERROR_SUCCESS)
      {
         int percent = static_cast<int>(value.doubleValue + 0.5);

         if (percent < 0)
            return 0;

         if (percent > 100)
            return 100;

         return percent;
      }
   }

   FILETIME idleTicksSinceBoot;
   FILETIME kernelTickSinceBoot;
   FILETIME userTicksSinceboot;

   if (!GetSystemTimes(
      &idleTicksSinceBoot, &kernelTickSinceBoot, &userTicksSinceboot))
      return -1;

   ULONGLONG idle = FileTimeToUInt64(idleTicksSinceBoot);
   ULONGLONG kernel = FileTimeToUInt64(kernelTickSinceBoot);
   ULONGLONG user = FileTimeToUInt64(userTicksSinceboot);

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

   ULONGLONG kernelBusyDelta = kernelDelta - idleDelta;
   ULONGLONG total = kernelBusyDelta + userDelta;

   if (total == 0)
      return 0;

   int percent = static_cast<int>(total * 100 / (total + idleDelta));

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

int GetDiskPercent()
{
   if (!diskCounter)
      return -1;

   DWORD bufferSize = 0;
   DWORD itemCount = 0;

   PDH_STATUS status = PdhGetFormattedCounterArrayW(
      diskCounter,
      PDH_FMT_DOUBLE,
      &bufferSize,
      &itemCount,
      nullptr
   );

   if (status != PDH_MORE_DATA)
      return -1;

   auto items = static_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(
      HeapAlloc(GetProcessHeap(), 0, bufferSize));

   if (!items)
      return -1;

   status = PdhGetFormattedCounterArrayW(
      diskCounter,
      PDH_FMT_DOUBLE,
      &bufferSize,
      &itemCount,
      items
   );

   if (status != ERROR_SUCCESS)
   {
      HeapFree(GetProcessHeap(), 0, items);
      return -1;
   }

   double maxValue = 0.0;

   for (DWORD i = 0; i < itemCount; i++)
   {
      if (items[i].szName && lstrcmpW(items[i].szName, L"_Total") == 0)
         continue;

      double value = items[i].FmtValue.doubleValue;

      if (value > maxValue)
         maxValue = value;
   }

   HeapFree(GetProcessHeap(), 0, items);

   if (maxValue < 0.0)
      maxValue = 0.0;

   if (maxValue > 100.0)
      maxValue = 100.0;

   return static_cast<int>(maxValue + 0.5);
}

bool IsNetworkInstanceIgnored(const wchar_t* name)
{
   if (!name)
      return true;

   if (lstrcmpW(name, L"_Total") == 0)
      return true;

   return wcsstr(name, L"Loopback") != nullptr;
}

int GetNetworkPercent()
{
   if (!netBytesCounter)
      return -1;

   DWORD bufferSize = 0;
   DWORD itemCount = 0;

   PDH_STATUS status = PdhGetFormattedCounterArrayW(
      netBytesCounter,
      PDH_FMT_DOUBLE,
      &bufferSize,
      &itemCount,
      nullptr
   );

   if (status != PDH_MORE_DATA)
      return -1;

   auto items = static_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(
      HeapAlloc(GetProcessHeap(), 0, bufferSize));

   if (!items)
      return -1;

   status = PdhGetFormattedCounterArrayW(
      netBytesCounter,
      PDH_FMT_DOUBLE,
      &bufferSize,
      &itemCount,
      items
   );

   if (status != ERROR_SUCCESS)
   {
      HeapFree(GetProcessHeap(), 0, items);
      return -1;
   }

   double maxBytesPerSecond = 0.0;

   for (DWORD i = 0; i < itemCount; i++)
   {
      const wchar_t* name = items[i].szName;

      if (IsNetworkInstanceIgnored(name))
         continue;

      double value = items[i].FmtValue.doubleValue;

      if (value > maxBytesPerSecond)
         maxBytesPerSecond = value;
   }

   HeapFree(GetProcessHeap(), 0, items);

   if (maxBytesPerSecond <= 0.0)
      return 0;

   const double kb = 1024.0;
   const double mb = kb * 1024.0;
   double percent = 0.0;

   if (maxBytesPerSecond < 100.0 * kb)
      percent = maxBytesPerSecond * 20.0 / (100.0 * kb);
   else if (maxBytesPerSecond < 1.0 * mb)
      percent = 20.0 + ((maxBytesPerSecond - 100.0 * kb) * 30.0 / (924.0 * kb));
   else if (maxBytesPerSecond < 10.0 * mb)
      percent = 50.0 + ((maxBytesPerSecond - 1.0 * mb) * 30.0 / (9.0 * mb));
   else if (maxBytesPerSecond < 100.0 * mb)
      percent = 80.0 + ((maxBytesPerSecond - 10.0 * mb) * 20.0 / (90.0 * mb));
   else
      percent = 100.0;

   if (percent < 0.0)
      return 0;

   if (percent > 100.0)
      return 100;

   return static_cast<int>(percent + 0.5);
}

bool InitGpuCounter()
{
   if (PdhOpenQuery(nullptr, 0, &gpuQuery) != ERROR_SUCCESS)
      return false;

   PdhAddEnglishCounterW(
      gpuQuery,
      L"\\Processor Information(_Total)\\% Processor Utility",
      0,
      &cpuCounter);

   PdhAddEnglishCounterW(
      gpuQuery,
      L"\\PhysicalDisk(*)\\% Disk Time",
      0,
      &diskCounter);

   PdhAddEnglishCounterW(
      gpuQuery,
      L"\\Network Interface(*)\\Bytes Total/sec",
      0,
      &netBytesCounter);

   if (PdhAddEnglishCounterW(
      gpuQuery,
      L"\\GPU Engine(*)\\Utilization Percentage",
      0,
      &gpuCounter) != ERROR_SUCCESS)
   {
      PdhCloseQuery(gpuQuery);
      gpuQuery = nullptr;
      cpuCounter = nullptr;
      diskCounter = nullptr;
      netBytesCounter = nullptr;
      gpuCounter = nullptr;
      return false;
   }

   PdhCollectQueryData(gpuQuery);
   return true;
}

// Get GPU usage percentage by querying the PDH counter for GPU utilization.
// Measures all GPU engines and returns the maximum utilization among them,
// which is a common way to estimate overall GPU usage. The counter used is
// "\\GPU Engine(*)\\Utilization Percentage", which provides the percentage
// of time the GPU engine is busy. The function collects the data, retrieves
// the formatted counter values for all instances, and calculates the maximum
// utilization percentage. If any step fails, it returns -1 to indicate an
// error.
int GetGpuPercent()
{
   if (!gpuQuery || !gpuCounter)
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

   auto items = static_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(
      HeapAlloc(GetProcessHeap(), 0, bufferSize));

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
      HeapFree(GetProcessHeap(), 0, items);
      return -1;
   }

   double maxValue = 0.0;

   for (DWORD i = 0; i < itemCount; i++)
   {
      double value = items[i].FmtValue.doubleValue;

      if (value > maxValue)
         maxValue = value;
   }

   HeapFree(GetProcessHeap(), 0, items);

   if (maxValue < 0.0)
      maxValue = 0.0;

   if (maxValue > 100.0)
      maxValue = 100.0;

   return static_cast<int>(maxValue + 0.5);
}

// Clean up the GPU counter resources by closing the PDH query.
void CleanupGpuCounter()
{
   if (gpuQuery)
   {
      PdhCloseQuery(gpuQuery);
      gpuQuery = nullptr;
      cpuCounter = nullptr;
      diskCounter = nullptr;
      netBytesCounter = nullptr;
      gpuCounter = nullptr;
   }
}

// Convert a percentage value (0-100) to a red color intensity for the icon.
int PercentToRed(int percent, bool bright)
{
   int level = percent;

   if (level < 0)
      level = 0;

   if (level > 100)
      level = 100;

   int baseRed = 80 + (level * 175 / 100);
   return bright ? baseRed : 20 + (level * 60 / 100);
}

// Draw a red dot on the provided device context (HDC) within the specified
// rectangle.
void DrawRedDot(HDC hdc, int left, int top, int right, int bottom, int red)
{
   HBRUSH brush = CreateSolidBrush(RGB(red, 0, 0));
   HGDIOBJ oldBrush = SelectObject(hdc, brush);

   Ellipse(hdc, left, top, right, bottom);

   SelectObject(hdc, oldBrush);
   DeleteObject(brush);
}

// Create an icon with red dots representing CPU and GPU usage percentages.
HICON CreateDotIcon(int cpuUsage, int gpuUsage, bool bright)
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

   int cpuRed = PercentToRed(cpuUsage, bright);
   int gpuRed = PercentToRed(gpuUsage, bright);

   DrawRedDot(mem, 3, 3, 29, 29, cpuRed);
   DrawRedDot(mem, 9, 9, 23, 23, gpuRed);

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

// Update the tray icon and tooltip with the current CPU and GPU
// usage percentages.
void UpdateTrayIcon()
{
   if (trayIcon)
      DestroyIcon(trayIcon);

   trayIcon = CreateDotIcon(cpuPercent, gpuPercent, pulse);

   nid.hIcon = trayIcon;
   FormatStatusText(tooltipText, _countof(tooltipText));
   lstrcpynW(nid.szTip, tooltipText, _countof(nid.szTip));

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

// Window procedure to handle messages for the hidden window that
// manages the tray icon and system resource monitoring.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
   switch (msg)
   {
      case WM_CREATE:
         mainWindow = hwnd;
         trayIcon = CreateDotIcon(0, 0, true);

         nid.cbSize = sizeof(nid);
         nid.hWnd = hwnd;
         nid.uID = 1;
         nid.uFlags = NIF_MESSAGE | NIF_ICON;
         nid.uCallbackMessage = WM_TRAYICON;
         nid.hIcon = trayIcon;
         FormatStatusText(tooltipText, _countof(tooltipText));
         lstrcpynW(nid.szTip, tooltipText, _countof(nid.szTip));

         Shell_NotifyIcon(NIM_ADD, &nid);
         SetTimer(hwnd, TIMER_ID, 1000, nullptr);
         gpuReady = InitGpuCounter();

         return 0;

      case WM_TIMER:
         if (wp == TOOLTIP_TIMER_ID)
         {
            UpdateCustomTooltipVisibility(hwnd);
            return 0;
         }

         if (gpuReady)
            PdhCollectQueryData(gpuQuery);

         cpuPercent = GetCpuPercent();
         ramPercent = GetRamPercent();
         diskPercent = GetDiskPercent();
         netPercent = GetNetworkPercent();

         if (gpuReady)
            gpuPercent = GetGpuPercent();

         pulse = !pulse;
         UpdateTrayIcon();
         UpdateCustomTooltipVisibility(hwnd);

         return 0;

      case WM_TRAYICON:
         if (lp == WM_MOUSEMOVE)
            UpdateCustomTooltipVisibility(hwnd);
         else if (lp == WM_MOUSELEAVE)
            HideCustomTooltip();
         else if (lp == WM_RBUTTONUP)
         {
            HideCustomTooltip();
            ShowTrayMenu(hwnd);
         }
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

         if (tooltipWindow)
            DestroyWindow(tooltipWindow);

         CleanupGpuCounter();
         PostQuitMessage(0);
         return 0;
   }

   return DefWindowProc(hwnd, msg, wp, lp);
}

// Entry point for the application, which creates a hidden window
// to manage the tray icon and starts the message loop.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
   const wchar_t CLASS_NAME[] = L"reddot_window";
   const wchar_t TOOLTIP_CLASS_NAME[] = L"reddot_tooltip";

   WNDCLASS wc = {};
   wc.lpfnWndProc = WndProc;
   wc.hInstance = instance;
   wc.lpszClassName = CLASS_NAME;

   RegisterClass(&wc);

   WNDCLASS tooltipClass = {};
   tooltipClass.lpfnWndProc = TooltipProc;
   tooltipClass.hInstance = instance;
   tooltipClass.lpszClassName = TOOLTIP_CLASS_NAME;
   tooltipClass.hCursor = LoadCursor(nullptr, IDC_ARROW);

   RegisterClass(&tooltipClass);

   CreateWindowEx(
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
