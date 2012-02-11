// ------------------------------------------------
// File : gui.cpp
// Date: 4-apr-2002
// Author: giles
// Desc: 
//		Windows front end GUI, PeerCast core is not dependant on any of this. 
//		Its very messy at the moment, but then again Windows UI always is.
//		I really don`t like programming win32 UI.. I want my borland back..
//
// (c) 2002 peercast.org
// ------------------------------------------------
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// ------------------------------------------------

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>	//JP-MOD
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

#if defined(NTDDI_VERSION) && (NTDDI_VERSION >= NTDDI_LONGHORN)
  #include <vssym32.h>
#else
  #include <tmschema.h>
#endif

#include <uxtheme.h>	// for XP
#pragma comment(lib, "uxtheme.lib")

#include <dwmapi.h>		// for Vista
#pragma comment(lib, "dwmapi.lib")

#if (_MSC_VER < 1300)
  #pragma comment(lib, "delayimp.lib")
  #pragma comment(linker, "/delayload:uxtheme.dll")
  #pragma comment(linker, "/delayload:dwmapi.dll")
#endif // (_MSC_VER < 1300)

#include "stdio.h"
#include "string.h"
#include "stdarg.h"
#include "resource.h"
#include "socket.h"
#include "win32/wsys.h"
#include "servent.h"
#include "win32/wsocket.h"
#include "inifile.h"
#include "gui.h"
#include "servmgr.h"
#include "peercast.h"
#include "simple.h"
#include "stats.h" //JP-MOD
#ifdef _DEBUG
#include "chkMemoryLeak.h"
#define DEBUG_NEW new(__FILE__, __LINE__)
#define new DEBUG_NEW
#endif

const int logID = IDC_LIST1, statusID = IDC_LIST2, hitID = IDC_LIST4, chanID = IDC_LIST3;

class WSingleLock
{
private:
	WLock *m_pLock;
public:
	WSingleLock(WLock *lock) : m_pLock(lock) { m_pLock->on(); }
	~WSingleLock()							 { m_pLock->off(); }
};

class HostName //JP-MOD
{
private:
	unsigned int	ip;
	String name;
public:
	HostName() : ip(0) {}

	const char *set(Host& host) throw()
	{
		if(ip != host.ip)
		{
			ip = host.ip;

			if(!(servMgr->enableGetName && host.globalIP() && ClientSocket::getHostname(name.cstr(), String::MAX_LEN, ip)))
				host.IPtoStr(name.cstr());
		}

		return name.cstr();
	}

	const char *cstr() throw()
	{
		return name.cstr();
	}
};

class ListData{
public:
	int channel_id;
	char name[21];
	int bitRate;
	int status;
	const char *statusStr;
	int totalListeners;
	int totalRelays;
	int totalClaps; //JP-MOD
	int localListeners;
	int localRelays;
	bool stayConnected;
	bool stealth; //JP-MOD
	int linkQuality; //JP-MOD
	unsigned int skipCount; //JP-MOD
	ChanHit chDisp;
	bool bTracker;

	bool flg;
	ListData *next;
};

class ServentData{
public:
	int servent_id;
	unsigned int tnum;
	int type;
	int status;
	String agent;
	Host h;
	HostName hostName; //JP-MOD
	unsigned int syncpos;
	char *typeStr;
	char *statusStr;
	bool infoFlg;
	bool relay;
	bool firewalled;
	unsigned int numRelays;
	unsigned int totalRelays;
	unsigned int totalListeners;
	int vp_ver;
	char ver_ex_prefix[2];
	int ver_ex_number;

	bool flg;
	ServentData *next;

	unsigned int lastSkipTime;
	unsigned int lastSkipCount;
};

ThreadInfo guiThread;
bool sleep_skip = false;
ListData *list_top = NULL;
WLock ld_lock;
ServentData *servent_top = NULL;
WLock sd_lock;
GnuID selchanID;
WLock selchanID_lock;

WINDOWPLACEMENT winPlace;
bool guiFlg = false;

//JP-MOD
enum GUIProc_CtrlID
{
	cid_taskbar = 5000,
	cid_cmdbar
};

enum GUIProc_ThemeID
{
	tid_button,
	tid_combobox,
	tid_valid
};

extern HWND mainWnd;
#define MAX_LOADSTRING 100
extern TCHAR szTitle[MAX_LOADSTRING];

static const TBBUTTON tbDJButtons[] =
{
{0, IDC_APPLY,		0,					BTNS_AUTOSIZE | BTNS_BUTTON,	{0}, 0, 0}
};

static const TBBUTTON tbButtons[] =
{
{7, IDC_BUTTON8,	TBSTATE_ENABLED,	BTNS_AUTOSIZE | BTNS_BUTTON,	{0}, 0, 0},
{0, IDC_BUTTON3,	TBSTATE_ENABLED,	BTNS_AUTOSIZE | BTNS_BUTTON,	{0}, 0, 1},
{1, IDC_BUTTON5,	TBSTATE_ENABLED,	BTNS_AUTOSIZE | BTNS_BUTTON,	{0}, 0, 2},
{2, IDC_STEALTH,	0,					BTNS_AUTOSIZE | BTNS_BUTTON,	{0}, 0, 3},
{3, IDC_CLAP,		0,					BTNS_AUTOSIZE | BTNS_BUTTON,	{0}, 0, 4},

{0, 0,				TBSTATE_ENABLED,	BTNS_SEP,						{0}, 0, 0},

{4, IDC_BUTTON6,	TBSTATE_ENABLED,	BTNS_AUTOSIZE | BTNS_BUTTON,	{0}, 0, 5},

{0, 0,				TBSTATE_ENABLED,	BTNS_SEP,						{0}, 0, 0},

{5, IDC_CHECK1,		TBSTATE_ENABLED,	BTNS_AUTOSIZE | BTNS_CHECK,		{0}, 0, 6},
{6, IDC_CHECK9,		TBSTATE_ENABLED,	BTNS_AUTOSIZE | BTNS_DROPDOWN,	{0}, 0, 7}
};

class ThemeHelper
{
private:
	static bool TryLoadLibrary(LPCTSTR lpLibFileName)
	{
		HMODULE hModule = ::LoadLibrary(lpLibFileName);
		if(hModule != NULL)
			::FreeLibrary(hModule);

		return (hModule != NULL);
	}
public:
	static bool IsThemingSupported()
	{
		static char nIsThemingSupported = -1;
		if(nIsThemingSupported == -1)
			nIsThemingSupported = (TryLoadLibrary(TEXT("uxtheme.dll"))) ? 1 : 0;

		return (nIsThemingSupported == 1);
	}

	static bool IsDwmApiSupported()
	{
		static char nIsDwmApiSupported = -1;
		if(nIsDwmApiSupported == -1)
			nIsDwmApiSupported = (TryLoadLibrary(TEXT("dwmapi.dll"))) ? 1 : 0;

		return (nIsDwmApiSupported == 1);
	}

	static bool IsBufferedPaintSupported()
	{
		static char nIsBufferedPaintSupported = -1;
		if(nIsBufferedPaintSupported == -1)
		{
			nIsBufferedPaintSupported = 0;

			HMODULE hThemeDLL = ::LoadLibrary(TEXT("uxtheme.dll"));
			if(hThemeDLL != NULL)
			{
				if(::GetProcAddress(hThemeDLL, TEXT("BufferedPaintInit")) != NULL)
					nIsBufferedPaintSupported = 1;
				::FreeLibrary(hThemeDLL);
			}
		}

		return (nIsBufferedPaintSupported == 1);
	}

	static BOOL SetLayeredWindowAttributes(HWND hwnd, COLORREF crKey, BYTE bAlpha, DWORD dwFlags)
	{
		typedef BOOL (WINAPI *PFNSETLAYEREDWINDOWATTRIBUTES)(HWND hwnd, COLORREF crKey, BYTE bAlpha, DWORD dwFlags);
		PFNSETLAYEREDWINDOWATTRIBUTES pfnSetLayeredWindowAttributes =
			(PFNSETLAYEREDWINDOWATTRIBUTES)::GetProcAddress(::GetModuleHandle(TEXT("user32.dll")), TEXT("SetLayeredWindowAttributes"));

		return (pfnSetLayeredWindowAttributes != NULL) ? pfnSetLayeredWindowAttributes(hwnd, crKey, bAlpha, dwFlags) : FALSE;
	}

	static bool DrawThemeClientEdge(HTHEME hTheme, HWND hWnd, HRGN hRgnUpdate = NULL, HBRUSH hBrush = NULL, int iPartID = 0, int iStateID = 0);
};

bool ThemeHelper::DrawThemeClientEdge(HTHEME hTheme, HWND hWnd, HRGN hRgnUpdate, HBRUSH hBrush, int iPartID, int iStateID)
{
	if(!ThemeHelper::IsThemingSupported())
		return false;

	bool result = false;
	HRGN hRgn = NULL;

	HDC hDC = GetWindowDC(hWnd);
	if(hDC == NULL)
		return false;

	int cxBorder = GetSystemMetrics(SM_CXBORDER);
	int cyBorder = GetSystemMetrics(SM_CYBORDER);
	if(SUCCEEDED(::GetThemeInt(hTheme, iPartID, iStateID, TMT_SIZINGBORDERWIDTH, &cxBorder)))
		cyBorder = cxBorder;

	RECT rc;
	if(!::GetWindowRect(hWnd, &rc))
		goto end;

	int cxEdge = GetSystemMetrics(SM_CXEDGE);
	int cyEdge = GetSystemMetrics(SM_CYEDGE);
	::InflateRect(&rc, -cxEdge, -cyEdge);
	hRgn = ::CreateRectRgnIndirect(&rc);
	if(hRgn == NULL)
		goto end;

	if(hRgnUpdate != NULL)
		::CombineRgn(hRgn, hRgn, hRgnUpdate, RGN_AND);

	::OffsetRect(&rc, -rc.left, -rc.top);

	::OffsetRect(&rc, cxEdge, cyEdge);
	::ExcludeClipRect(hDC, rc.left, rc.top, rc.right, rc.bottom);
	::InflateRect(&rc, cxEdge, cyEdge);

	::DrawThemeBackground(hTheme, hDC, iPartID, iStateID, &rc, NULL);

	if (cxBorder < cxEdge &&
		cyBorder < cyEdge)
	{
		if(hBrush == NULL)
			hBrush = (HBRUSH)::GetClassLongPtr(hWnd, GCLP_HBRBACKGROUND);

		::InflateRect(&rc, cxBorder - cxEdge, cyBorder - cyEdge);
		::FillRect(hDC, &rc, hBrush);
	}

	::DefWindowProc(hWnd, WM_NCPAINT, (WPARAM)hRgn, 0);

	result = true;
end:
	if(hRgn != NULL)
		::DeleteObject(hRgn);
	::ReleaseDC(hWnd, hDC);
	return result;
}

class ClipboardMgr //JP-MOD
{
private:
	static bool set(HWND hwndOwner, const char *text);
public:
	static void setPlayURL(HWND hwndChanList);
	static void setContactURL(HWND hwndChanList);
	static void setAddress(HWND hwndConnList);
};

class EnumSelectedChannels //JP-MOD
{
private:
	virtual bool discovered(Channel *c) = 0;
public:
	virtual ~EnumSelectedChannels()	{}

	int enumerate(HWND hwnd);
};
#if 0
class SpinEditImpl
{
private:
	enum {cid_text, cid_spin};

	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
public:
	static ATOM Register()
	{
		static WNDCLASSEX wc = {sizeof(WNDCLASSEX), 0, WindowProc, 0, 0, NULL, NULL, NULL, NULL, NULL, TEXT("SPINEDIT"), NULL};
		static ATOM atom = ::RegisterClassEx(&wc);

		return atom;
	}
};

LRESULT CALLBACK SpinEditImpl::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_COMMAND:
		if(LOWORD(wParam) == cid_text && HIWORD(wParam) == EN_CHANGE)
			return ::SendMessage(::GetParent(hwnd), WM_COMMAND, MAKEWPARAM(::GetDlgCtrlID(hwnd), EN_CHANGE), (LPARAM)hwnd);
		break;
	case WM_GETTEXT:
	case WM_SETTEXT:
	case WM_GETFONT:
	case WM_SETFONT:
		return ::SendDlgItemMessage(hwnd, cid_text, uMsg, wParam, lParam);
	case WM_ENABLE:
		::EnableWindow(GetDlgItem(hwnd, cid_text), (BOOL)wParam);
		::EnableWindow(GetDlgItem(hwnd, cid_spin), (BOOL)wParam);
		return 0;
	case WM_CREATE:
		{
			LPCREATESTRUCT lpcs = (LPCREATESTRUCT)lParam;

			if(ThemeHelper::IsBufferedPaintSupported())
			{
				if(FAILED(::BufferedPaintInit()))
					return -1;
			}

			HWND hText = ::CreateWindowEx(WS_EX_CLIENTEDGE | WS_EX_CONTROLPARENT, WC_EDIT, NULL, WS_CHILD | WS_TABSTOP | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER | ES_RIGHT,
										0, 0, lpcs->cx, lpcs->cy, hwnd, (HMENU)cid_text, lpcs->hInstance, 0);

			HWND hSpin = ::CreateWindowEx(WS_EX_CONTROLPARENT, UPDOWN_CLASS, NULL, WS_CHILD | WS_TABSTOP | WS_VISIBLE |
										UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_AUTOBUDDY | UDS_NOTHOUSANDS | UDS_SETBUDDYINT,
										0, 0, 0, 0, hwnd, (HMENU)cid_spin, lpcs->hInstance, 0);
			::SendMessage(hSpin, UDM_SETRANGE32, 1, 99);

			return !(hText && hSpin);
		}
	case WM_NCDESTROY:
		if(ThemeHelper::IsBufferedPaintSupported())
			::BufferedPaintUnInit();
		break;
	}

	return ::DefWindowProc(hwnd, uMsg, wParam, lParam);
}
#endif // 0
class DJEditImpl
{
private:
	enum CtrlID {
		cid_text,
		cid_cmds,
	};

	enum WindowLongIndexes {
		wlid_state = 0,
		wlid_valid,
	};

	enum SEARCHBOXPARTS {
		SBBACKGROUND = 1,
	};

	enum SEARCHBOXSTATES {
		SB_NORMAL = 1,
		SB_HOT = 2,
		SB_DISABLED = 3,
		SB_FOCUSED = 4,
	};

	static HTHEME GetBestTheme(HWND hwnd)
	{
		if(!ThemeHelper::IsThemingSupported())
			return NULL;

		HTHEME hTheme = ::GetWindowTheme(hwnd);
		if(hTheme == NULL) // for XP (Alternate Background)
		{
			HWND hText = ::GetDlgItem(hwnd, cid_text);
			if(::IsWindow(hText))
				hTheme = ::GetWindowTheme(hText);
		}

		return hTheme;
	}

	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static BOOL OnEraseBkgnd(HWND hwnd, HDC hdc);
	static void OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify);
	static void OnSize(HWND hwnd, UINT state, int cx, int cy);
	static void OnDestroy(HWND hwnd);
	static BOOL OnCreate(HWND hwnd, LPCREATESTRUCT lpCreateStruct);
	static void OnThemeChanged(HWND hwnd);
	static void OnDwmCompositionChanged(HWND hwnd);
public:
	static ATOM Register()
	{
		static WNDCLASSEX wc = {sizeof(WNDCLASSEX), 0, WindowProc, 0, sizeof(LONG_PTR) * wlid_valid, NULL, NULL, NULL, (HBRUSH)(COLOR_WINDOW + 1), NULL, TEXT("DJEDIT"), NULL};
		static ATOM atom = ::RegisterClassEx(&wc);

		return atom;
	}
};

/* void Cls_OnThemeChanged(HWND hwnd) */
#define HANDLE_WM_THEMECHANGED(hwnd, wParam, lParam, fn) \
    ((fn)(hwnd), 0L)
#define FORWARD_WM_THEMECHANGED(hwnd, fn) \
    (void)(fn)((hwnd), WM_THEMECHANGED, 0L, 0L)

/* void Cls_OnDwmCompositionChanged(HWND hwnd) */
#define HANDLE_WM_DWMCOMPOSITIONCHANGED(hwnd, wParam, lParam, fn) \
    ((fn)(hwnd), 0L)
#define FORWARD_WM_DWMCOMPOSITIONCHANGED(hwnd, fn) \
    (void)(fn)((hwnd), WM_DWMCOMPOSITIONCHANGED, 0L, 0L)

LRESULT CALLBACK DJEditImpl::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	HANDLE_MSG(hwnd, WM_ERASEBKGND, OnEraseBkgnd);
	HANDLE_MSG(hwnd, WM_COMMAND, OnCommand);
	HANDLE_MSG(hwnd, WM_SIZE, OnSize);

	case WM_GETTEXT:
	case WM_GETFONT:
	case WM_SETFONT:
		return ::SendDlgItemMessage(hwnd, cid_text, uMsg, wParam, lParam);
	case WM_SETTEXT:
		{
			BOOL result = ::SendDlgItemMessage(hwnd, cid_text, uMsg, wParam, lParam);
			::SendDlgItemMessage(hwnd, cid_cmds, TB_ENABLEBUTTON, IDC_APPLY, MAKELONG(FALSE, 0));

			return result;
		}

	HANDLE_MSG(hwnd, WM_THEMECHANGED, OnThemeChanged);
	HANDLE_MSG(hwnd, WM_DWMCOMPOSITIONCHANGED, OnDwmCompositionChanged);
	HANDLE_MSG(hwnd, WM_CREATE, OnCreate);
	HANDLE_MSG(hwnd, WM_DESTROY, OnDestroy);
	}

	return ::DefWindowProc(hwnd, uMsg, wParam, lParam);
}

BOOL DJEditImpl::OnEraseBkgnd(HWND hwnd, HDC hdc)
{
	RECT rc;
	if(!::GetClientRect(hwnd, &rc))
		return FALSE;

	if(ThemeHelper::IsThemingSupported())
	{
		HTHEME hTheme = GetBestTheme(hwnd);
		if(hTheme != NULL)
		{
			int stateId = (int)::GetWindowLongPtr(hwnd, sizeof(LONG_PTR) * wlid_state);

			if(::IsThemeBackgroundPartiallyTransparent(hTheme, SBBACKGROUND, stateId))
				::DrawThemeParentBackground(hwnd, hdc, &rc);
			::DrawThemeBackground(hTheme, hdc, SBBACKGROUND, stateId, &rc, &rc);
		}
	}
	else
	{
		::FrameRect(hdc, &rc, ::GetSysColorBrush(COLOR_INACTIVEBORDER));
		::InflateRect(&rc, -1, -1);
		::FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
	}

	return TRUE;
}

void DJEditImpl::OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify)
{
	switch(id)
	{
	case cid_text:
		if(ThemeHelper::IsDwmApiSupported())
		{
			switch(codeNotify)
			{
			case EN_KILLFOCUS:
			case EN_SETFOCUS:
				::SetWindowLongPtr(hwnd, sizeof(LONG_PTR) * wlid_state,
					(codeNotify == EN_SETFOCUS) ? SB_FOCUSED : SB_NORMAL);
				::InvalidateRect(hwnd, NULL, TRUE);
				{
					HWND hCommands = ::GetDlgItem(hwnd, cid_cmds);
					if(::IsWindow(hCommands))
						::InvalidateRect(hCommands, NULL, TRUE);
				}
				break;
			}
		}

		if(codeNotify == EN_CHANGE)
			::SendDlgItemMessage(hwnd, cid_cmds, TB_ENABLEBUTTON, IDC_APPLY, MAKELONG(TRUE, 0));
		break;
	case IDC_APPLY:
		::SendMessage(GetParent(hwnd), WM_COMMAND, IDC_CHECK11, NULL);
		::SendDlgItemMessage(hwnd, cid_cmds, TB_ENABLEBUTTON, IDC_APPLY, MAKELONG(FALSE, 0));
		break;
	}
}

void DJEditImpl::OnSize(HWND hwnd, UINT state, int cx, int cy)
{
	HWND hCommands = GetDlgItem(hwnd, cid_cmds);
	RECT rc = {0};
	if(::IsWindow(hCommands))
		::SendMessage(hCommands, TB_GETITEMRECT, ::SendMessage(hCommands, TB_BUTTONCOUNT, 0, 0) - 1, (LPARAM)&rc);

	int cxTextBorder = ::GetSystemMetrics(SM_CXEDGE);
	int cxBorder = ::GetSystemMetrics(SM_CXBORDER);
	if(ThemeHelper::IsThemingSupported())
	{
		HTHEME hTheme = GetBestTheme(hwnd);
		if(hTheme != NULL)
		{
			::GetThemeInt(hTheme, SBBACKGROUND, SB_NORMAL, TMT_TEXTBORDERSIZE, &cxTextBorder);
			::GetThemeInt(hTheme, SBBACKGROUND, SB_NORMAL, TMT_BORDERSIZE, &cxBorder);
		}
	}

	HDWP hDwp = ::BeginDeferWindowPos(2);
	{
		HWND hText = ::GetDlgItem(hwnd, cid_text);
		if(::IsWindow(hText))
		{
			hDwp = ::DeferWindowPos(hDwp, hText, HWND_BOTTOM,
				cxTextBorder,
				cxTextBorder,
				cx - rc.right - cxBorder - cxTextBorder,
				cy - cxTextBorder * 2,
				0);
		}
	}

	if(::IsWindow(hCommands))
	{
		WORD cyInner = cy - cxBorder * 2;

		::SendMessage(hCommands, TB_SETBUTTONSIZE, 0, MAKELONG(cyInner, cyInner));
		hDwp = ::DeferWindowPos(hDwp, hCommands, HWND_BOTTOM,
			cx - rc.right - cxBorder,
			cxBorder,
			rc.right,
			cyInner,
			0);
	}
	::EndDeferWindowPos(hDwp);
}


void DJEditImpl::OnThemeChanged(HWND hwnd)
{
	if(ThemeHelper::IsThemingSupported())
	{
		HTHEME hTheme = ::GetWindowTheme(hwnd);
		if(hTheme != NULL)
			::CloseThemeData(hTheme);
		::OpenThemeData(hwnd, L"SearchBoxComposited::SearchBox");

		::SendDlgItemMessage(hwnd, cid_cmds, TB_SETWINDOWTHEME, 0, (LPARAM)L"SearchButton");
	}
}

void DJEditImpl::OnDwmCompositionChanged(HWND hwnd)
{
	if(ThemeHelper::IsDwmApiSupported())
	{
		BOOL fEnabled;
		HRESULT hr = ::DwmIsCompositionEnabled(&fEnabled);
		if(SUCCEEDED(hr))
		{
			HWND hText = ::GetDlgItem(hwnd, cid_text);
			if(::IsWindow(hText))
				::SetWindowTheme(hText, (fEnabled) ? L"SearchBoxEditComposited" : L"SearchBoxEdit", NULL);
		}
	}
}

BOOL DJEditImpl::OnCreate(HWND hwnd, LPCREATESTRUCT lpCreateStruct)
{
	HWND hText = ::CreateWindowEx(WS_EX_CONTROLPARENT, WC_EDIT, NULL, WS_CHILD | WS_TABSTOP | WS_VISIBLE | ES_AUTOHSCROLL,
		0, 0, lpCreateStruct->cx, lpCreateStruct->cy, hwnd, (HMENU)cid_text, lpCreateStruct->hInstance, 0);
	Edit_SetCueBannerText(hText, L"DJメッセージ"); //Windows XP Bug: It doesn't work in Japanese.

	{ // for Vista
		HFONT hFont = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);

		LOGFONTW logfont;
		if(ThemeHelper::IsThemingSupported() && SUCCEEDED(::GetThemeSysFont(NULL, TMT_MSGBOXFONT, &logfont)))
			hFont = ::CreateFontIndirectW(&logfont);

		::SendMessage(hText, WM_SETFONT, (WPARAM)hFont, 0);
	}

	HWND hCommands = ::CreateWindowEx(WS_EX_CONTROLPARENT, TOOLBARCLASSNAME, NULL, WS_CHILD | WS_TABSTOP | WS_VISIBLE |
		CCS_NODIVIDER | CCS_NORESIZE | CCS_TOP | /*TBSTYLE_CUSTOMERASE |*/ TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS | TBSTYLE_TRANSPARENT,
		0, 0, 0, 0, hwnd, (HMENU)cid_cmds, lpCreateStruct->hInstance, 0);

	::SendMessage(hCommands, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);
	::SendMessage(hCommands, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);
	::SendMessage(hCommands, TB_ADDSTRING, (WPARAM)NULL, (LPARAM)"適用\0");
	HIMAGELIST hImage = ImageList_LoadImage(lpCreateStruct->hInstance, MAKEINTRESOURCE(IDB_DJ), 0, 1, CLR_DEFAULT, IMAGE_BITMAP, LR_CREATEDIBSECTION | LR_DEFAULTSIZE);
	if(hImage != NULL)
	{
		::SendMessage(hCommands, TB_SETIMAGELIST, 0, (LPARAM)hImage);
		::SendMessage(hCommands, TB_SETDISABLEDIMAGELIST, 0, (LPARAM)hImage);
	}
	::SendMessage(hCommands, TB_ADDBUTTONS, (WPARAM)ARRAYSIZE(tbDJButtons), (LPARAM)tbDJButtons);

	OnThemeChanged(hwnd);
	OnDwmCompositionChanged(hwnd);

	return hText && hCommands;
}

void DJEditImpl::OnDestroy(HWND hwnd)
{
	if(ThemeHelper::IsThemingSupported())
	{
		HTHEME hTheme = ::GetWindowTheme(hwnd);
		if(hTheme != NULL)
			::CloseThemeData(hTheme);
	}

	{
		HFONT hFont = (HFONT)::SendDlgItemMessage(hwnd, cid_text, WM_GETFONT, 0, 0);
		::SendDlgItemMessage(hwnd, cid_text, WM_SETFONT, NULL, 0);

		if(hFont != NULL)
			::DeleteObject(hFont);
	}

	FORWARD_WM_DESTROY(hwnd, ::DefWindowProc);
}

class GUIConfig //JP-MOD
{
private:
	static LRESULT CALLBACK PropGeneralProc		(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK PropTitleProc		(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK PropLayoutProc		(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK PropExpansionProc	(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
public:
	int Show(HWND hWnd);
};


INT_PTR pp_formatTitle(char *str, size_t size, bool minimized)
{
	const char *format = minimized ? servMgr->guiTitleModifyMinimized.cstr() : servMgr->guiTitleModifyNormal.cstr();
	size_t destPos = 0;

	enum
	{
		normal,
		percent,
		variable
	} state = normal;

	char partStr[16];
	size_t partPos = 0;
	unsigned int level = 0;

	enum
	{
		error = -1,
		rx,
		tx
	} var = error;

	struct
	{
		double prefix;
		int precision;
	} rxtx;
	memset(&rxtx, 0, sizeof(rxtx));

	for(;*format != '\0' && destPos < size && partPos < sizeof(partStr);format++)
	{
		switch(state)
		{
		case normal:
			if(*format == '%')
				state = percent;
			else
				str[destPos++] = *format;
			break;
		case percent:
			if(*format == '%'){
				state = normal;
				str[destPos++] = *format;
				break;
			}

			partPos = level = rxtx.precision = 0;
			rxtx.prefix = 1024.0f / 8;
			var = error;
			state = variable;
			// breakなしで続行
		case variable:
			switch(*format){
			case '%':
				state = normal;
				// breakなしで続行
			case '.':
				partStr[partPos] = '\0';

				switch(level++){
				case 0:
					if(!strcmp(partStr, "rx"))
						var = rx;
					else if(!strcmp(partStr, "tx"))
						var = tx;
					else
						return -1;

					break;
				case 1:
					{
						const char *readPointer = partStr;

						switch(*readPointer){
						case 'k':
							readPointer++;
							// breakなしで続行
						case '\0':
							rxtx.prefix = 1024.0f / 8;
							break;
						case 'm':
							readPointer++;
							rxtx.prefix = 1024.0f * 1024 / 8;
							break;
						default:
							rxtx.prefix = 1.0f / 8;
							break;
						}

						if(!strcmp(readPointer, "bytes"))
							rxtx.prefix *= 8;
					}
					break;
				case 2:
					rxtx.precision = atoi(partStr);
					break;
				default:
					return -1;
				}

				partPos = 0;

				if(state == normal){
					int bandwidth = (var == rx) ?	stats.getPerSecond(Stats::BYTESIN) - stats.getPerSecond(Stats::LOCALBYTESIN):
													stats.getPerSecond(Stats::BYTESOUT) - stats.getPerSecond(Stats::LOCALBYTESOUT);
					int ret = _snprintf_s(&str[destPos], size - destPos, _TRUNCATE, "%.*f", rxtx.precision, bandwidth / rxtx.prefix);
					if(ret > 0)
						destPos += ret;
				}
				break;
			default:
				if(!isalnum(*format))
					return -1;

				partStr[partPos++] = *format;
				break;
			}
			break;
		}
	}

	str[destPos] = '\0';
	return (state == normal) ? destPos : -1;
}



// --------------------------------------------------
// for PCRaw (connection list) start
LRESULT CALLBACK ListBoxProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch(message)
	{
		case WM_LBUTTONDOWN:
		{
			LRESULT ret = ::CallWindowProc((WNDPROC)GetWindowLongPtr(hwnd, GWLP_USERDATA), hwnd, message, wParam, lParam);
			sleep_skip = true;
			return ret;
		}
			break;

		case WM_LBUTTONDBLCLK:
		{
			LRESULT ret = ::CallWindowProc((WNDPROC)GetWindowLongPtr(hwnd, GWLP_USERDATA), hwnd, message, wParam, lParam);
			::SendMessage(::GetParent(hwnd), WM_COMMAND, IDC_BUTTON8, NULL);
			return ret;
		}
			break;

		case WM_RBUTTONDOWN:
		{
			POINT pos;
			MENUITEMINFO info, separator;
			HMENU hMenu;
			DWORD dwID;

			LRESULT ret = ::CallWindowProc((WNDPROC)GetWindowLongPtr(hwnd, GWLP_USERDATA), hwnd, message, wParam, lParam);
			if(!ListView_GetSelectedCount(hwnd))
			{
				sleep_skip = true;
				return ret;
			}
			
			hMenu = ::CreatePopupMenu();

			memset(&separator, 0, sizeof(MENUITEMINFO));
			separator.cbSize = sizeof(MENUITEMINFO);
			separator.fMask = MIIM_ID | MIIM_TYPE;
			separator.fType = MFT_SEPARATOR;
			separator.wID = 8000;

			memset(&info, 0, sizeof(MENUITEMINFO));
			info.cbSize = sizeof(MENUITEMINFO);
			info.fMask = MIIM_ID | MIIM_TYPE;
			info.fType = MFT_STRING;

			info.wID = 1001;
			info.dwTypeData = "切断(&X)";
			::InsertMenuItem(hMenu, -1, true, &info);

			::InsertMenuItem(hMenu, -1, true, &separator);

			info.wID = 1000;
			info.dwTypeData = "再生(&P)";
			::InsertMenuItem(hMenu, -1, true, &info);

			::InsertMenuItem(hMenu, -1, true, &separator);

			info.wID = 1002;
			info.dwTypeData = "再接続(&R)";
			::InsertMenuItem(hMenu, -1, true, &info);

			info.wID = 1003;
			info.dwTypeData = "キープ(&K)";
			::InsertMenuItem(hMenu, -1, true, &info);

			::InsertMenuItem(hMenu, -1, true, &separator);

			info.wID = 2000;
			info.dwTypeData = "選択解除(&D)";
			::InsertMenuItem(hMenu, -1, true, &info);

			::InsertMenuItem(hMenu, -1, true, &separator);

			info.wID = 3000; //JP-MOD
			info.dwTypeData = "隠蔽(&S)";
			::InsertMenuItem(hMenu, -1, true, &info);

			info.wID = 3001;
			info.dwTypeData = "拍手(&A)";
			::InsertMenuItem(hMenu, -1, true, &info);

			::InsertMenuItem(hMenu, -1, true, &separator);

			info.wID = 4000;
			info.dwTypeData = "再生URLをコピー(&L)";
			::InsertMenuItem(hMenu, -1, true, &info);

			info.wID = 4001;
			info.dwTypeData = "コンタクトURLをコピー(&C)";
			::InsertMenuItem(hMenu, -1, true, &info);

			::GetCursorPos(&pos);
			dwID = ::TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RETURNCMD, pos.x, pos.y, 0, hwnd, NULL);

			::DestroyMenu(hMenu);

			switch(dwID)
			{
			case 1000:
				::SendMessage(::GetParent(hwnd), WM_COMMAND, IDC_BUTTON8, NULL);
				break;

			case 1001:
				::SendMessage(::GetParent(hwnd), WM_COMMAND, IDC_BUTTON5, NULL);
				break;

			case 1002:
				::SendMessage(::GetParent(hwnd), WM_COMMAND, IDC_BUTTON3, NULL);
				break;

			case 1003:
				::SendMessage(::GetParent(hwnd), WM_COMMAND, IDC_BUTTON9, NULL);
				break;

			case 2000:
				ListView_SetItemState(hwnd, -1, 0, LVIS_FOCUSED | LVIS_SELECTED);
				sleep_skip = true;
				break;

			case 3000: //JP-MOD
				::SendMessage(::GetParent(hwnd), WM_COMMAND, IDC_STEALTH, NULL);
				break;

			case 3001:
				::SendMessage(::GetParent(hwnd), WM_COMMAND, IDC_CLAP, NULL);
				break;

			case 4000:
				ClipboardMgr::setPlayURL(hwnd);
				break;

			case 4001:
				ClipboardMgr::setContactURL(hwnd);
				break;
			}

			return ret;
		}
			break;

		case WM_KEYDOWN:
			sleep_skip = true;
			break;

		case WM_CHAR:
		{
			switch((int)wParam)
			{
			case VK_CANCEL:
				ClipboardMgr::setContactURL(hwnd);
				return 0;
			}
		}
			break;
	}

	return ::CallWindowProc((WNDPROC)::GetWindowLongPtr(hwnd, GWLP_USERDATA), hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ConnListBoxProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch(message)
	{
		case WM_RBUTTONDOWN:
		{
			POINT pos;
			MENUITEMINFO info, separator;
			HMENU hMenu;
			DWORD dwID;

			LRESULT ret = ::CallWindowProc((WNDPROC)::GetWindowLongPtr(hwnd, GWLP_USERDATA), hwnd, message, wParam, lParam);
			if(!ListView_GetSelectedCount(hwnd))
				return ret;
			
			hMenu = ::CreatePopupMenu();

			memset(&separator, 0, sizeof(MENUITEMINFO));
			separator.cbSize = sizeof(MENUITEMINFO);
			separator.fMask = MIIM_ID | MIIM_TYPE;
			separator.fType = MFT_SEPARATOR;
			separator.wID = 8000;

			memset(&info, 0, sizeof(MENUITEMINFO));
			info.cbSize = sizeof(MENUITEMINFO);
			info.fMask = MIIM_ID | MIIM_TYPE;
			info.fType = MFT_STRING;

			info.wID = 1001;
			info.dwTypeData = "切断(&X)";
			::InsertMenuItem(hMenu, -1, true, &info);

			::InsertMenuItem(hMenu, -1, true, &separator);

			info.wID = 1002;
			info.dwTypeData = "アドレスをコピー(&C)";
			::InsertMenuItem(hMenu, -1, true, &info);

			::GetCursorPos(&pos);
			dwID = ::TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RETURNCMD, pos.x, pos.y, 0, hwnd, NULL);

			::DestroyMenu(hMenu);

			switch(dwID)
			{
			case 1001:
				::SendMessage(::GetParent(hwnd), WM_COMMAND, IDC_BUTTON6, NULL);
				break;
			case 1002:
				ClipboardMgr::setAddress(hwnd);
				break;
			}

			return ret;
		}
			break;

		case WM_CHAR:
		{
			switch((int)wParam)
			{
			case VK_CANCEL:
				ClipboardMgr::setAddress(hwnd);
				return 0;
			}
		}
			break;
	}

	return ::CallWindowProc((WNDPROC)::GetWindowLongPtr(hwnd, GWLP_USERDATA), hwnd, message, wParam, lParam);
}
// for PCRaw (connection list) end
// --------------------------------------------------

// --------------------------------------------------
bool getButtonState(HWND hwnd, int id) //JP-MOD
{
	return ::SendDlgItemMessage(hwnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// --------------------------------------------------
void *getListBoxSelData(HWND hwnd, int id)
{
	LRESULT sel = ::SendDlgItemMessage(hwnd, id, LB_GETCURSEL, 0, 0);
	if (sel >= 0)
		return (void *)::SendDlgItemMessage(hwnd, id, LB_GETITEMDATA, sel, 0);
	return NULL;
}

Channel *getListBoxChannel(HWND hwndChanList){
	int sel = ListView_GetNextItem(hwndChanList, -1, LVNI_FOCUSED | LVNI_SELECTED); //JP-MOD
	if (sel >= 0){
		WSingleLock lock(&ld_lock); //JP-MOD
		ListData *ld = list_top;
		int idx = 0;

		while(ld){
			if (sel == idx){
				return chanMgr->findChannelByChannelID(ld->channel_id);
			}
			ld = ld->next;
			idx++;
		}
	}
	return NULL;
}

int EnumSelectedChannels::enumerate(HWND hwnd) //JP-MOD
{
	int count = 0;
	HWND chanWnd = ::GetDlgItem(hwnd, IDC_LIST3);
	if(!chanWnd) return 0;

	int sel = ListView_GetNextItem(chanWnd, -1, LVNI_SELECTED);

	if(sel >= 0)
	{
		ListData *ld;
		int index;

		WSingleLock lock(&chanMgr->channellock);
		WSingleLock lock_2nd(&ld_lock);
		for(ld = list_top, index = 0;
			ld != NULL;
			ld = ld->next, ++index)
		{
			if(sel == index)
			{
				sel = ListView_GetNextItem(chanWnd, sel, LVNI_SELECTED);

				Channel *c = chanMgr->findChannelByChannelID(ld->channel_id);
				if(!c)
					continue;

				++count;

				if(!discovered(c) || sel < 0)
					break;
			}
		}
	}

	return count;
}

Servent *getListBoxServent(HWND hwnd) //JP-MOD
{
	int sel = ListView_GetNextItem(::GetDlgItem(hwnd, IDC_LIST2), -1, LVNI_FOCUSED | LVNI_SELECTED);
	if (sel >= 0)
	{
		WSingleLock lock(&sd_lock);
		ServentData *sd = servent_top;

		int idx = 0;

		while(sd){
			if (sel == idx){
				return servMgr->findServentByServentID(sd->servent_id);
			}
			sd = sd->next;
			idx++;
		}
	}
	return NULL;
}

// --------------------------------------------------
void setButtonStateEx(HWND hwnd, int id, bool on) //JP-MOD
{
	::SendDlgItemMessage(hwnd, id, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}
// --------------------------------------------------
typedef struct tagADDLOGINFO {
	String strMessage;
	int id;
	bool sel;
	void *data;
	LogBuffer::TYPE type;
} ADDLOGINFO, *LPADDLOGINFO;

void ADDLOG(const char *str, int id, bool sel, void *data, LogBuffer::TYPE type) //JP-MOD: Thread safe fix
{
	HWND hwnd = guiWnd;

	String sjis; //JP-Patch
	sjis = str;
	sjis.convertTo(String::T_SJIS);

	if (type != LogBuffer::T_NONE)
	{
#if _DEBUG
		::OutputDebugString(sjis.cstr());
		::OutputDebugString("\n");
#endif
	}

	if(::IsWindow(hwnd))
	{
		LPADDLOGINFO pInfo = new ADDLOGINFO;

		pInfo->strMessage = sjis;
		pInfo->id = id;
		pInfo->sel = sel;
		pInfo->data = data;
		pInfo->type = type;

		//他スレッドへのSendMessageはデッドロックを引き起こす場合があり危険なので
		//非同期メッセージ関数を使用する(メモリの解放は受け取り側で行う)
		if(!::SendNotifyMessage(hwnd, WM_ADDLOG, (WPARAM)pInfo, 0))
			delete pInfo;
	}
}

static void GUIProc_OnAddLog(HWND hwnd, LPADDLOGINFO pInfo)
{
	if(pInfo == NULL)
		return;

	LRESULT num = ::SendDlgItemMessage(hwnd, pInfo->id, LB_GETCOUNT, 0, 0);
	if (num > 100)
	{
		::SendDlgItemMessage(hwnd, pInfo->id, LB_DELETESTRING, 0, 0);
		num--;
	}

	LRESULT idx = ::SendDlgItemMessage(hwnd, pInfo->id, LB_ADDSTRING, 0, (LPARAM)(LPSTR)pInfo->strMessage.cstr());
	::SendDlgItemMessage(hwnd, pInfo->id, LB_SETITEMDATA, idx, (LPARAM)pInfo->data);

	if (pInfo->sel)
		::SendDlgItemMessage(hwnd, pInfo->id, LB_SETCURSEL, num, 0);

	delete pInfo;
}

// --------------------------------------------------
void ADDLOG2(const char *fmt,va_list ap,int id,bool sel,void *data, LogBuffer::TYPE type)
{
	char str[4096];
	vsprintf(str,fmt,ap);

	ADDLOG(str,id,sel,data,type);
}

// --------------------------------------------------
void ADDCHAN(void *d, const char *fmt,...)
{
	va_list ap;
  	va_start(ap, fmt);
	ADDLOG2(fmt,ap,chanID,false,d,LogBuffer::T_NONE);
   	va_end(ap);	
}
// --------------------------------------------------
void ADDHIT(void *d, const char *fmt,...)
{
	va_list ap;
  	va_start(ap, fmt);
	ADDLOG2(fmt,ap,hitID,false,d,LogBuffer::T_NONE);
   	va_end(ap);	
}
// --------------------------------------------------
void ADDCONN(void *d, const char *fmt,...)
{
	va_list ap;
  	va_start(ap, fmt);
	ADDLOG2(fmt,ap,statusID,false,d,LogBuffer::T_NONE);
   	va_end(ap);	
}


// --------------------------------------------------
int checkLinkQuality(Channel *c) //JP-MOD
{
	if(!c->isPlaying())
		return -1;
	if(!c->skipCount)
		return 3;

	unsigned int timeago = sys->getTime() - c->lastSkipTime;

	if(timeago >= 120){
		c->skipCount = 0;
		return 3;
	}

	int ret = (timeago / 4) - (c->skipCount / 2);

	return	ret < 0 ?	0:
			ret < 2 ?	ret:
						2;
}

// --------------------------------------------------
THREAD_PROC showConnections(ThreadInfo *thread)
{
	HWND hwnd = (HWND)thread->data;
	//	bool shownChannels = false;
	//	thread->lock();

	while (thread->active)
	{
		int diff = 0;
		bool changed = false; //JP-MOD

		LRESULT sel = ListView_GetNextItem(::GetDlgItem(hwnd, IDC_LIST2), -1, LVNI_FOCUSED | LVNI_SELECTED); //JP-MOD

		ServentData *sd = servent_top;
		while(sd){
			sd->flg = false;
			sd = sd->next;
		}

		{ //JP-MOD
			GnuID show_chanID;
			{ //JP-MOD: Thread safe fix
				WSingleLock lock(&selchanID_lock);
				show_chanID = selchanID;
			}

			WSingleLock lock(&servMgr->lock);
			Servent *s = servMgr->servents;

			while(s){
				Servent *next;
				bool foundFlg = false;
				bool infoFlg = false;
				bool relay = true;
				bool firewalled = false;
				unsigned int numRelays = 0;
				unsigned int totalRelays = 0;
				unsigned int totalListeners = 0;
				int vp_ver = 0;
				char ver_ex_prefix[2] = {0};
				int ver_ex_number = 0;

				next = s->next;

				// for PCRaw (connection list) start
				if(show_chanID.isSet() && !show_chanID.isSame(s->chanID))
				{
					s = next;
					continue;
				}
				// for PCRaw (connection list) end

				//JP-MOD
				if (servMgr->guiSimpleConnectionList && (
					((s->type == Servent::T_DIRECT) && (s->status == Servent::S_CONNECTED)) ||
					((s->type == Servent::T_SERVER) && (s->status == Servent::S_LISTENING))
					)) {
						s = next;
						continue;
				}

				if (s->type != Servent::T_NONE){

					WSingleLock lock(&chanMgr->hitlistlock); //JP-MOD
					ChanHitList *chl = chanMgr->findHitListByID(s->chanID);

					if (chl){
						ChanHit *hit = chl->hit;
						while(hit){
							if (hit->servent_id == s->servent_id){
								if ((hit->numHops == 1)/* && (hit->host.ip == s->getHost().ip)*/){
									infoFlg = true;
									relay = hit->relay;
									firewalled = hit->firewalled;
									numRelays = hit->numRelays;
									vp_ver = hit->version_vp;
									ver_ex_prefix[0] = hit->version_ex_prefix[0];
									ver_ex_prefix[1] = hit->version_ex_prefix[1];
									ver_ex_number = hit->version_ex_number;
								}
								totalRelays += hit->numRelays;
								totalListeners += hit->numListeners;
							}
							hit = hit->next;
						}
					}
				}

				{ //JP-MOD
					WSingleLock lock(&sd_lock); //JP-MOD: new lock
					ServentData *sd = servent_top;

					while(sd){
						if (sd->servent_id == s->servent_id){
							foundFlg = true;
							if (s->thread.finish) break;
							sd->flg = true;
							sd->type = s->type;
							sd->status = s->status;
							sd->agent = s->agent;
							sd->h = s->getHost();
							sd->hostName.set(sd->h);
							sd->syncpos = s->syncPos;
							sd->tnum = (s->lastConnect) ? sys->getTime()-s->lastConnect : 0;
							sd->typeStr = s->getTypeStr();
							sd->statusStr = s->getStatusStr();
							sd->infoFlg = infoFlg;
							sd->relay = relay;
							sd->firewalled = firewalled;
							sd->numRelays = numRelays;
							sd->totalRelays = totalRelays;
							sd->totalListeners = totalListeners;
							sd->vp_ver = vp_ver;
							sd->lastSkipTime = s->lastSkipTime;
							sd->lastSkipCount = s->lastSkipCount;
							sd->ver_ex_prefix[0] = ver_ex_prefix[0];
							sd->ver_ex_prefix[1] = ver_ex_prefix[1];
							sd->ver_ex_number = ver_ex_number;
							break;
						}
						sd = sd->next;
					}
					if (!foundFlg && (s->type != Servent::T_NONE) && !s->thread.finish){
						ServentData *newData = new ServentData();
						newData->next = servent_top;
						servent_top = newData;
						newData->flg = true;
						newData->servent_id = s->servent_id;
						newData->type = s->type;
						newData->status = s->status;
						newData->agent = s->agent;
						newData->h = s->getHost();
						newData->hostName.set(newData->h);
						newData->syncpos = s->syncPos;
						newData->tnum = (s->lastConnect) ? sys->getTime()-s->lastConnect : 0;
						newData->typeStr = s->getTypeStr();
						newData->statusStr = s->getStatusStr();
						newData->infoFlg = infoFlg;
						newData->relay = relay;
						newData->firewalled = firewalled;
						newData->numRelays = numRelays;
						newData->totalRelays = totalRelays;
						newData->totalListeners = totalListeners;
						newData->vp_ver = vp_ver;
						newData->lastSkipTime = s->lastSkipTime;
						newData->lastSkipCount = s->lastSkipCount;
						newData->ver_ex_prefix[0] = ver_ex_prefix[0];
						newData->ver_ex_prefix[1] = ver_ex_prefix[1];
						newData->ver_ex_number = ver_ex_number;

						diff++;
						changed = true;
					}
				}
				s = next;
			}
		}

		{ //JP-MOD
			int idx = 0;

			{
				WSingleLock lock(&sd_lock);
				ServentData *sd = servent_top;
				ServentData *prev = NULL;
				while(sd){
					if (!sd->flg || (sd->type == Servent::T_NONE)){
						ServentData *next = sd->next;
						if (!prev){
							servent_top = next;
						} else {
							prev->next = next;
						}
						delete sd;

						changed = true;

						sd = next;
						//				diff--;
					} else {
						idx++;
						prev = sd;
						sd = sd->next;
					}
				}
			}

			if((sel >= 0) && (diff != 0))
				ListView_SetItemState(::GetDlgItem(hwnd, IDC_LIST2), sel+diff, LVNI_FOCUSED | LVIS_SELECTED, LVNI_FOCUSED | LVIS_SELECTED);
			ListView_SetItemCountEx(::GetDlgItem(hwnd, IDC_LIST2), idx, LVSICF_NOSCROLL);
		}

		{
			ListData *ld = list_top;
			while(ld){
				ld->flg = false;
				ld = ld->next;
			}

			{ //JP-MOD
				WSingleLock lock(&chanMgr->channellock); //JP-MOD: new lock
				Channel *c = chanMgr->channel;

				while (c)
				{
					Channel *next;
					bool foundFlg = false;
					String sjis;
					sjis = c->getName();
					sjis.convertTo(String::T_SJIS);

					next = c->next;

					{
						WSingleLock lock(&ld_lock); //JP-MOD: new lock
						ListData *ld = list_top;

						while(ld){
							if (ld->channel_id == c->channel_id){
								foundFlg = true;
								if (c->thread.finish) break;
								ld->flg = true;
								strncpy(ld->name, sjis, 20);
								ld->name[20] = '\0';
								ld->bitRate = c->info.bitrate;
								ld->status = c->status;
								ld->statusStr = c->getStatusStr();
								ld->totalListeners = c->totalListeners();
								ld->totalRelays = c->totalRelays();
								ld->totalClaps = c->totalClaps();
								ld->localListeners = c->localListeners();
								ld->localRelays = c->localRelays();
								ld->stayConnected = c->stayConnected;
								ld->stealth = c->stealth;
								ld->linkQuality = checkLinkQuality(c);
								ld->skipCount = c->skipCount;
								ld->chDisp = c->chDisp;
								ld->bTracker = c->sourceHost.tracker;
								break;
							}
							ld = ld->next;
						}
						if (!foundFlg && !c->thread.finish){
							ListData *newData = new ListData();
							newData->next = list_top;
							list_top = newData;
							newData->flg = true;
							newData->channel_id = c->channel_id;
							strncpy(newData->name, sjis, 20);
							newData->name[20] = '\0';
							newData->bitRate = c->info.bitrate;
							newData->status = c->status;
							newData->statusStr = c->getStatusStr();
							newData->totalListeners = c->totalListeners();
							newData->totalRelays = c->totalRelays();
							newData->totalClaps = c->totalClaps();
							newData->localListeners = c->localListeners();
							newData->localRelays = c->localRelays();
							newData->stayConnected = c->stayConnected;
							newData->stealth = c->stealth;
							newData->linkQuality = checkLinkQuality(c);
							newData->skipCount = c->skipCount;
							newData->chDisp = c->chDisp;
							newData->bTracker = c->sourceHost.tracker;

							changed = true;
						}
					}
					c = next;
				}
			}

			{ //JP-MOD
				int idx = 0;
				{
					WSingleLock lock(&ld_lock);
					ListData *ld = list_top;
					ListData *prev = NULL;

					while(ld){
						if (!ld->flg){
							ListData *next = ld->next;
							if (!prev){
								list_top = next;
							} else {
								prev->next = next;
							}
							delete ld;

							changed = true;
							ld = next;
						} else {
							idx++;
							prev = ld;
							ld = ld->next;
						}
					}
				}
				ListView_SetItemCountEx(::GetDlgItem(hwnd, IDC_LIST3), idx, LVSICF_NOSCROLL);
			}

			if (changed &&
				(servMgr->guiConnListDisplays < 0 ||
				servMgr->guiChanListDisplays < 0)
				)
				::PostMessage(hwnd, WM_REFRESH, 0, 0);
		}
#if 0
		bool update = ((sys->getTime() - chanMgr->lastHit) < 3)||(!shownChannels);

		if (update)
		{
			shownChannels = true;

			sel = ::SendDlgItemMessage(hwnd, hitID,LB_GETCURSEL, 0, 0);
			LRESULT top = ::SendDlgItemMessage(hwnd, hitID,LB_GETTOPINDEX, 0, 0);
			::SendDlgItemMessage(hwnd, hitID, LB_RESETCONTENT, 0, 0);

			{
				WSingleLock lock(&chanMgr->hitlistlock); //JP-MOD
				ChanHitList *chl = chanMgr->hitlist;

				while (chl)
				{
					if (chl->isUsed())
					{
						if (chl->info.match(chanMgr->searchInfo))
						{
							char cname[34];
							strncpy(cname,chl->info.name.cstr(),16);
							cname[16] = 0;
							ADDHIT(chl,"%s - %d kb/s - %d/%d",cname,chl->info.bitrate,chl->numListeners(),chl->numHits());
						}
					}
					chl = chl->next;
				}
			}

			if (sel >= 0)
				::SendDlgItemMessage(hwnd, hitID,LB_SETCURSEL, sel, 0);
			if (top >= 0)
				::SendDlgItemMessage(hwnd, hitID,LB_SETTOPINDEX, top, 0);
		}

		{
			switch (servMgr->getFirewall())
			{
			case ServMgr::FW_ON:
				::SendDlgItemMessage(hwnd, IDC_EDIT4,WM_SETTEXT, 0, (LPARAM)"Firewalled");
				break;
			case ServMgr::FW_UNKNOWN:
				::SendDlgItemMessage(hwnd, IDC_EDIT4,WM_SETTEXT, 0, (LPARAM)"Unknown");
				break;
			case ServMgr::FW_OFF:
				::SendDlgItemMessage(hwnd, IDC_EDIT4,WM_SETTEXT, 0, (LPARAM)"Normal");
				break;
			}
		}
#endif
		char buf[64]; //JP-MOD
		if(servMgr->guiTitleModify && pp_formatTitle(buf, sizeof(buf), ::IsIconic(hwnd) != 0) > 0)
			::SetWindowText(hwnd, buf);

		// sleep for 1 second .. check every 1/10th for shutdown
		for(int i=0; i<10; i++)
		{
			if(sleep_skip)	// for PCRaw (connection list)
			{
				sleep_skip = false;
				break;
			}

			if (!thread->active)
				break;
			sys->sleep(100);
		}
	}

	ListData *ld = list_top;

	while(ld){
		ListData *next;
		next = ld->next;

		delete ld;

		ld = next;
	}
	list_top = NULL;

	ServentData *sd = servent_top;

	while(sd){
		ServentData *next;
		next = sd->next;

		delete sd;

		sd = next;
	}
	servent_top = NULL;

	//	thread->unlock();
	return 0;
}


// --------------------------------------------------
void tryConnect()
{
#if 0
	ClientSocket sock;

	char tmp[32];

	char *sendStr = "GET / HTTP/1.1\n\n";

	try {
		sock.open("taiyo",80);
		sock.write(sendStr,strlen(sendStr));
		sock.read(tmp,32);
		LOG("Connected: %s",tmp);
	}catch(IOException &e)
	{
		LOG(e.msg);
	}
#endif
}


// ---------------------------------
void APICALL MyPeercastApp ::printLog(LogBuffer::TYPE t, const char *str)
{
	ADDLOG(str,logID,true,NULL,t);
	if (logFile.isOpen())
	{
		logFile.writeLine(str);
		logFile.flush();
	}
}


// --------------------------------------------------
static void setControls(HWND hwnd, bool fromGUI) //JP-MOD
{
	HWND hwndTaskbar = ::GetDlgItem(hwnd, cid_taskbar);
	if(hwndTaskbar)
	{
		String sjis;
		sjis = chanMgr->broadcastMsg;
		sjis.convertTo(String::T_SJIS);
		::SetDlgItemText(hwndTaskbar, IDC_EDIT9, sjis.cstr());
	}

	::SetWindowPos(hwnd, servMgr->guiTopMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	if(!servMgr->guiTitleModify)
		::SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)szTitle);

	{
		HWND chanWnd = ::GetDlgItem(hwnd, IDC_LIST3);

		LONG_PTR dwStyle = ::GetWindowLongPtr(chanWnd, GWL_STYLE);
		dwStyle = (servMgr->guiSimpleChannelList) ? dwStyle | LVS_NOCOLUMNHEADER : dwStyle & ~LVS_NOCOLUMNHEADER;
		::SetWindowLongPtr(chanWnd, GWL_STYLE, dwStyle);
	}

	HWND hwndCmdbar = ::GetDlgItem(hwnd, cid_cmdbar);
	if (!fromGUI && hwndCmdbar)
	{
		::SendDlgItemMessage(hwndCmdbar, 0, TB_CHECKBUTTON, IDC_CHECK1, MAKELONG(servMgr->autoServe, 0));
		::SendMessage(hwnd, WM_COMMAND, IDC_CHECK1, 0);
	}
}
// --------------------------------------------------
void APICALL MyPeercastApp::updateSettings() //JP-MOD: Thread safe fix
{
	HWND hwnd = guiWnd;
	::SendNotifyMessage(hwnd, WM_UPDATESETTINGS, 0, 0);
}

int convertScaleX(int src) //JP-MOD
{
	struct Local
	{
		static double get()
		{
			HDC hScreen = GetDC(0);
			if(!hScreen)
				return 1.0;

			double ret = GetDeviceCaps(hScreen, LOGPIXELSX) / 96.0;
			ReleaseDC(0, hScreen);

			return ret;
		}
	};

	static double scaleX = Local::get();

	return (int)(src * scaleX);
}
#define convertScaleY(y)	convertScaleX(y)

static BOOL GUIProc_OnEraseBkgnd(HWND hwnd, HDC hdc);
static BOOL GUIProc_OnCreate(HWND hwnd, LPCREATESTRUCT lpCreateStruct);
static void GUIProc_OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify);
static void GUIProc_OnSysCommand(HWND hwnd, UINT cmd, int x, int y);
static LRESULT GUIProc_OnNotify(HWND hwnd, int idCtrl, LPNMHDR pnmh);
static void GUIProc_OnRefresh(HWND hwnd);
static void GUIProc_OnChanInfoChanged(HWND hwnd);
static void GUIProc_OnClose(HWND hwnd);
static void GUIProc_OnDestroy(HWND hwnd);
static void GUIProc_OnActivate(HWND hwnd, UINT state, HWND hwndActDeact, BOOL fMinimized);
static bool GUIProc_GetTransparentMargins(HWND hwnd, MARGINS &margins);
static void GUIProc_OnThemeChanged(HWND hwnd);
static void GUIProc_OnDwmCompositionChanged(HWND hwnd);

// --------------------------------------------------
static void GUIProc_OnSize(HWND hwnd, UINT state, int cx, int cy) //JP-MOD
{
	UNREFERENCED_PARAMETER(state);
	HDWP hDwp = ::BeginDeferWindowPos(3);
	if(!hDwp) return;

	{
		int cyDoubleEdge = ::GetSystemMetrics(SM_CYEDGE) * 2;
		int y;
		{
			RECT rc;

			HWND hwndTaskbar = ::GetDlgItem(hwnd, cid_taskbar);
			if(!::IsWindow(hwndTaskbar)) goto END_MOVECONTROLS;

			::SendMessage(hwndTaskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(cx, cy));
			if(!::GetWindowRect(hwndTaskbar, &rc)) goto END_MOVECONTROLS;
			::MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rc, 2);

			HWND hwndCmdbar = ::GetDlgItem(hwnd, cid_cmdbar);
			if(!::IsWindow(hwndCmdbar)) goto END_MOVECONTROLS;
			UINT barHeight = ::SendMessage(hwndCmdbar, RB_GETBARHEIGHT, 0, 0);
			if((::GetWindowLongPtr(hwndCmdbar, GWL_STYLE) & WS_BORDER) != 0)
				barHeight += cyDoubleEdge;
			::MoveWindow(hwndCmdbar, rc.left, rc.bottom, cx, barHeight, TRUE);

			GUIProc_OnDwmCompositionChanged(hwnd);

			y = rc.bottom + barHeight;
		}

		{
			DWORD dwSize = ListView_ApproximateViewRect(::GetDlgItem(hwnd, IDC_LIST3), -1, -1, (servMgr->guiChanListDisplays > -1) ? servMgr->guiChanListDisplays : -1);
			int nHeight = HIWORD(dwSize) + cyDoubleEdge;
			hDwp = ::DeferWindowPos(hDwp, ::GetDlgItem(hwnd, IDC_LIST3), HWND_BOTTOM, 0, y, cx, nHeight, SWP_SHOWWINDOW);
			y += nHeight;
		}

		{
			DWORD dwSize = ListView_ApproximateViewRect(::GetDlgItem(hwnd, IDC_LIST2), -1, -1, (servMgr->guiConnListDisplays > -1) ? servMgr->guiConnListDisplays : -1);
			int nHeight = HIWORD(dwSize) + cyDoubleEdge;
			hDwp = ::DeferWindowPos(hDwp, ::GetDlgItem(hwnd, IDC_LIST2), HWND_BOTTOM, 0, y, cx, nHeight, SWP_SHOWWINDOW);
			y += nHeight;

			nHeight = cy - y;
			hDwp = ::DeferWindowPos(hDwp, ::GetDlgItem(hwnd, IDC_LIST1), HWND_BOTTOM, 0, y, cx, nHeight, SWP_SHOWWINDOW);
		}
	}

END_MOVECONTROLS:
	::EndDeferWindowPos(hDwp);
}

ATOM GUIProc_RegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEX wc = {sizeof(WNDCLASSEX)};

	wc.lpfnWndProc = GUIProc;
	wc.cbWndExtra = sizeof(LONG_PTR) * tid_valid; // for Stock theme
	wc.hInstance = hInstance;
	wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SIMPLE));
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = "MainWindow";
	wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));

	return ::RegisterClassEx(&wc);
}

static void GUIProc_ResetToolbar(HWND hwndToolbar) //JP-MOD
{
	LRESULT nButtons = ::SendMessage(hwndToolbar, TB_BUTTONCOUNT, 0, 0);
	while(nButtons--)
		::SendMessage(hwndToolbar, TB_DELETEBUTTON, 0, 0);
	::SendMessage(hwndToolbar, TB_ADDBUTTONS, (WPARAM)ARRAYSIZE(tbButtons), (LPARAM)tbButtons);

	::SendMessage(hwndToolbar, TB_CHECKBUTTON, IDC_CHECK1,  MAKELONG(servMgr->autoServe, 0));
}

static void GUIProc_InitChannelListColumn(HWND chanWnd) //JP-MOD
{
	LV_COLUMN column;

	column.mask = LVCF_FMT | LVCF_SUBITEM | LVCF_TEXT | LVCF_WIDTH;
	column.fmt = LVCFMT_LEFT;
	column.cx = convertScaleX(130);
	column.pszText = "チャンネル名";
	column.iSubItem = 0;
	ListView_InsertColumn(chanWnd, column.iSubItem, &column);

	column.fmt = LVCFMT_RIGHT;
	column.cx = convertScaleX(68);
	column.pszText = "ビットレート";
	column.iSubItem++;
	ListView_InsertColumn(chanWnd, column.iSubItem, &column);

	column.fmt = LVCFMT_LEFT;
	column.pszText = "状態";
	column.iSubItem++;
	ListView_InsertColumn(chanWnd, column.iSubItem, &column);

	column.cx = convertScaleX(50);
	column.pszText = "リスナー/リレー";
	column.iSubItem++;
	ListView_InsertColumn(chanWnd, column.iSubItem, &column);

	column.pszText = "ローカルリスナー/リレー";
	column.iSubItem++;
	ListView_InsertColumn(chanWnd, column.iSubItem, &column);

	column.fmt = LVCFMT_RIGHT;
	column.cx = convertScaleX(26);
	column.pszText = "拍手";
	column.iSubItem++;
	ListView_InsertColumn(chanWnd, column.iSubItem, &column);

	column.cx = convertScaleX(24);
	column.pszText = "スキップ";
	column.iSubItem++;
	ListView_InsertColumn(chanWnd, column.iSubItem, &column);

	column.fmt = LVCFMT_CENTER;
	column.cx = convertScaleX(50);
	column.pszText = "キープ";
	column.iSubItem++;
	ListView_InsertColumn(chanWnd, column.iSubItem, &column);
}

static void GUIProc_InitConnectionListColumn(HWND connWnd) //JP-MOD
{
	LV_COLUMN column;

	column.mask = LVCF_FMT | LVCF_SUBITEM | LVCF_TEXT | LVCF_WIDTH;
	column.fmt = LVCFMT_LEFT;
	column.cx = convertScaleX(80);
	column.pszText = "状態";
	column.iSubItem = 0;
	ListView_InsertColumn(connWnd, column.iSubItem, &column);

	column.fmt = LVCFMT_RIGHT;
	column.cx = convertScaleX(50);
	column.pszText = "時間";
	column.iSubItem++;
	ListView_InsertColumn(connWnd, column.iSubItem, &column);

	column.fmt = LVCFMT_LEFT;
	column.cx = convertScaleX(30);
	column.pszText = "リスナー/リレー";
	column.iSubItem++;
	ListView_InsertColumn(connWnd, column.iSubItem, &column);

	column.cx = convertScaleX(110);
	column.pszText = "アドレス";
	column.iSubItem++;
	ListView_InsertColumn(connWnd, column.iSubItem, &column);

	column.fmt = LVCFMT_RIGHT;
	column.cx = convertScaleX(43);
	column.pszText = "ポート";
	column.iSubItem++;
	ListView_InsertColumn(connWnd, column.iSubItem, &column);

	column.pszText = "同期位置";
	column.iSubItem++;
	ListView_InsertColumn(connWnd, column.iSubItem, &column);

	column.fmt = LVCFMT_LEFT;
	column.cx = convertScaleX(100);
	column.pszText = "エージェント";
	column.iSubItem++;
	ListView_InsertColumn(connWnd, column.iSubItem, &column);

	column.cx = convertScaleX(40);
	column.pszText = "拡張情報";
	column.iSubItem++;
	ListView_InsertColumn(connWnd, column.iSubItem, &column);
}

static HTHEME GUIProc_GetWindowLongTheme(HWND hwnd, GUIProc_ThemeID themeId)
{
	return (HTHEME)::GetWindowLongPtr(hwnd, sizeof(LONG_PTR) * themeId);
}

static HTHEME GUIProc_SetWindowLongTheme(HWND hwnd, GUIProc_ThemeID themeId, HTHEME hNewTheme)
{
	return (HTHEME)::SetWindowLongPtr(hwnd, sizeof(LONG_PTR) * themeId, (LONG_PTR)hNewTheme);
}

static HTHEME GUIProc_UpdateTheme(HWND hwnd, GUIProc_ThemeID themeId, LPCWSTR pszClassList)
{
	if(!ThemeHelper::IsThemingSupported())
		return NULL;

	HTHEME hOldTheme = GUIProc_SetWindowLongTheme(hwnd, themeId, NULL);
	if(hOldTheme != NULL)
		::CloseThemeData(hOldTheme);

	if(pszClassList == NULL)
		return NULL;

	HTHEME hNewTheme = ::OpenThemeData(hwnd, pszClassList);
	GUIProc_SetWindowLongTheme(hwnd, themeId, hNewTheme);
	return hNewTheme;
}

static void GUIProc_CloseThemes(HWND hwnd)
{
	if(!ThemeHelper::IsThemingSupported())
		return;

	int i;
	for(i = 0; i < tid_valid; i++)
		GUIProc_UpdateTheme(hwnd, (GUIProc_ThemeID)i, NULL);
}

ListData *getListData(DWORD_PTR index) //JP-MOD
{
	ListData *ld;
	for(ld = list_top;
		ld != NULL && index;
		ld = ld->next, --index);

	return ld;
}

ServentData *getServentData(DWORD_PTR index) //JP-MOD
{
	ServentData *sd;
	for(sd = servent_top;
		sd != NULL && index;
		sd = sd->next, --index);

	return sd;
}

bool ClipboardMgr::set(HWND hwndOwner, const char *text) //JP-MOD
{
	bool ret = false;

	if(!hwndOwner || !text)
		return false;

	if(OpenClipboard(hwndOwner))
	{
		EmptyClipboard();

		size_t size = strlen(text) + 1;
		HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, size);
		if(hGlobal)
		{
			char *pString = (char*)GlobalLock(hGlobal);
			if(pString)
			{
				memcpy(pString, text, size);
				GlobalUnlock(hGlobal);

				ret = (SetClipboardData(CF_TEXT, hGlobal) != NULL);
			}
		}
		CloseClipboard();

		if(hGlobal)
			GlobalFree(hGlobal);
	}

	return ret;
}

void ClipboardMgr::setPlayURL(HWND hwndChanList) //JP-MOD
{
	WSingleLock lock(&chanMgr->channellock);
	Channel *c = getListBoxChannel(hwndChanList);

	if(c && c->info.id.isSet())
	{
		char idStr[64];
		char buf[128];

		c->getIDStr(idStr);
		_snprintf_s(buf, _countof(buf), _TRUNCATE, "http://localhost:%d/pls/%s", servMgr->serverHost.port, idStr);
		set(hwndChanList, buf);
	}
}

void ClipboardMgr::setContactURL(HWND hwndChanList) //JP-MOD
{
	WSingleLock lock(&chanMgr->channellock);
	Channel *c = getListBoxChannel(hwndChanList);

	if(c && !c->info.url.isEmpty())
		set(hwndChanList, c->info.url.cstr());
}

void ClipboardMgr::setAddress(HWND hwndConnList) //JP-MOD
{
	int sel = ListView_GetNextItem(hwndConnList, -1, LVNI_FOCUSED | LVNI_SELECTED);

	if(sel >= 0)
	{
		WSingleLock lock(&sd_lock);
		ServentData *sd = getServentData(sel);

		if(sd)
			set(hwndConnList, sd->hostName.cstr());
	}
}

// --------------------------------------------------
LRESULT CALLBACK GUIProc (HWND hwnd, UINT message,
						  WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_ADDLOG:				return GUIProc_OnAddLog(hwnd, (LPADDLOGINFO)wParam), 0;

	HANDLE_MSG(hwnd, WM_ERASEBKGND, GUIProc_OnEraseBkgnd);
	HANDLE_MSG(hwnd, WM_CREATE, GUIProc_OnCreate);
	HANDLE_MSG(hwnd, WM_COMMAND, GUIProc_OnCommand);
	HANDLE_MSG(hwnd, WM_SYSCOMMAND, GUIProc_OnSysCommand);
	HANDLE_MSG(hwnd, WM_NOTIFY, GUIProc_OnNotify);
	HANDLE_MSG(hwnd, WM_SIZE, GUIProc_OnSize);
	HANDLE_MSG(hwnd, WM_ACTIVATE, GUIProc_OnActivate);
	HANDLE_MSG(hwnd, WM_THEMECHANGED, GUIProc_OnThemeChanged);
	HANDLE_MSG(hwnd, WM_DWMCOMPOSITIONCHANGED, GUIProc_OnDwmCompositionChanged);

	case WM_REFRESH:			return GUIProc_OnRefresh(hwnd), 0;
	case WM_CHANINFOCHANGED:	return GUIProc_OnChanInfoChanged(hwnd), 0;
	case WM_UPDATESETTINGS:		return setControls(hwnd, true), 0;

	HANDLE_MSG(hwnd, WM_CLOSE, GUIProc_OnClose);
	HANDLE_MSG(hwnd, WM_DESTROY, GUIProc_OnDestroy);
	}

	return DefWindowProc(hwnd, message, wParam, lParam);
}

BOOL GUIProc_OnEraseBkgnd(HWND hwnd, HDC hdc)
{
	FORWARD_WM_ERASEBKGND(hwnd, hdc, ::DefWindowProc);

	if(ThemeHelper::IsDwmApiSupported())
	{
		BOOL fEnabled;
		HRESULT hr = ::DwmIsCompositionEnabled(&fEnabled);

		if(SUCCEEDED(hr) && fEnabled)
		{
			MARGINS margins = {0};
			RECT rect;
			if(GUIProc_GetTransparentMargins(hwnd, margins) && ::GetClientRect(hwnd, &rect))
			{
				rect.bottom = margins.cyTopHeight;
				::FillRect(hdc, &rect, (HBRUSH)::GetStockObject(BLACK_BRUSH));
			}
		}
	}

	return TRUE;
}

static bool GUIProc_CreateControls(HWND hwnd, HINSTANCE hInstance)
{
	HWND chanWnd = ::CreateWindowEx(WS_EX_CONTROLPARENT, WC_LISTVIEW, TEXT("チャンネルリストビュー"), WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VISIBLE |
		LVS_AUTOARRANGE | LVS_NOSORTHEADER | LVS_OWNERDATA | LVS_REPORT | LVS_SHAREIMAGELISTS | LVS_SHOWSELALWAYS,
		0, 0, 0, 0, hwnd, (HMENU)chanID,	hInstance, NULL);
	if (!::IsWindow(chanWnd))
	{
		LOG_ERROR("Channel list creation failed.");
		return false;
	}
	if (!::CreateWindowEx(WS_EX_CONTROLPARENT, WC_LISTVIEW, TEXT("コネクションリストビュー"), WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VISIBLE |
		LVS_AUTOARRANGE | LVS_NOSORTHEADER | LVS_OWNERDATA | LVS_REPORT | LVS_SHAREIMAGELISTS | LVS_SINGLESEL,
		0, 0, 0, 0, hwnd, (HMENU)statusID,	hInstance, NULL))
	{
		LOG_ERROR("Connection list creation failed.");
		return false;
	}
	if (!::CreateWindowEx(WS_EX_CONTROLPARENT, WC_LISTBOX, TEXT("ログリスト"), WS_CHILD | WS_TABSTOP | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
		0, 0, 0, 0, hwnd, (HMENU)logID,		hInstance, NULL))
	{
		LOG_ERROR("Log list creation failed.");
		return false;
	}

	if (!::CreateWindowEx(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, REBARCLASSNAME, TEXT("タスクバー"), WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VISIBLE |
		CCS_NODIVIDER | RBS_AUTOSIZE | RBS_VARHEIGHT,
		0, 0, 0, 0, hwnd, (HMENU)cid_taskbar,	hInstance, NULL))
	{
		LOG_ERROR("Taskbar creation failed.");
		return false;
	}

	HWND hwndCmdbar = ::CreateWindowEx(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, REBARCLASSNAME, TEXT("コマンドバー"), WS_BORDER | WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VISIBLE |
		CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE | RBS_AUTOSIZE | RBS_VARHEIGHT,
		0, 0, 0, 0, hwnd, (HMENU)cid_cmdbar,	hInstance, NULL);
	if(!::IsWindow(hwndCmdbar))
	{
		LOG_ERROR("Commandbar creation failed.");
		return false;
	}
	if (!::CreateWindowEx(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, TOOLBARCLASSNAME, TEXT("ツールバー"), WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VISIBLE |
		CCS_ADJUSTABLE | CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE | TBSTYLE_ALTDRAG | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS | TBSTYLE_TRANSPARENT,
		0, 0, 0, 0, hwndCmdbar, (HMENU)0,		hInstance, NULL))
	{
		LOG_ERROR("Toolbar creation failed.");
		return false;
	}

	return true;
}

static bool GUIProc_InitializeBar(HWND hwnd, HINSTANCE hInstance)
{
	HWND hwndTaskbar = ::GetDlgItem(hwnd, cid_taskbar);
	if(!::IsWindow(hwndTaskbar))
	{
		LOG_ERROR("Taskbar is not found.");
		return false;
	}

	HWND hwndCmdbar = ::GetDlgItem(hwnd, cid_cmdbar);
	if(!::IsWindow(hwndCmdbar))
	{
		LOG_ERROR("Commandbar is not found.");
		return false;
	}

	HWND hwndToolbar = ::GetDlgItem(hwndCmdbar, 0);
	if(!::IsWindow(hwndToolbar))
	{
		LOG_ERROR("Toolbar is not found.");
		return false;
	}

	if(ThemeHelper::IsDwmApiSupported()) // for Vista
	{
		::SendMessage(hwndCmdbar, RB_SETWINDOWTHEME, 0, (LPARAM)L"Media");
		::SendMessage(hwndToolbar, TB_SETWINDOWTHEME, 0, (LPARAM)L"Alternate");
	}

	::SendMessage(hwndToolbar, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);
	::SendMessage(hwndToolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS | TBSTYLE_EX_DOUBLEBUFFER);
	::SendMessage(hwndToolbar, TB_ADDSTRING, (WPARAM)NULL, (LPARAM)"再生\0再接続\0切断\0隠蔽\0拍手\0接続切断\0サーバー\0ログ\0");
	{
		HIMAGELIST hImage;
		if(ThemeHelper::IsThemingSupported())	// for XP
			hImage = ::ImageList_LoadImage(hInstance, MAKEINTRESOURCE(IDB_CMDBAR_32), 0, 8, CLR_NONE, IMAGE_BITMAP, LR_CREATEDIBSECTION | LR_DEFAULTSIZE);
		else
			hImage = ::ImageList_LoadImage(hInstance, MAKEINTRESOURCE(IDB_CMDBAR_24), 0, 8, CLR_DEFAULT, IMAGE_BITMAP, LR_CREATEDIBSECTION | LR_DEFAULTSIZE);

		if(hImage == NULL)
		{
			LOG_ERROR("Toolbar's image list creation failed.");
			return false;
		}

		TCHAR szPlayerPath[MAX_PATH];
		DWORD cchOut = _countof(szPlayerPath);
		LPCTSTR pszMediaPlayerExtensions[] =
		{
			TEXT(".asx"),
			TEXT(".asf"),
			TEXT(".wmv"),
			TEXT(".wma"),
			TEXT(".pls"),
			TEXT(".mp3"),
			TEXT(".ogg"),
			TEXT(".ram")
		};

		int i;
		HRESULT hr;
		for(i = 0, hr = E_FAIL;
			i < _countof(pszMediaPlayerExtensions) && FAILED(hr);
			i++)
			hr = ::AssocQueryString(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, pszMediaPlayerExtensions[i], TEXT("open"), szPlayerPath, &cchOut);

		if(SUCCEEDED(hr))
		{
			// 既定の再生プレイヤーアイコンを読み込む
			SHFILEINFO sfInfo;
			if(::SHGetFileInfo(szPlayerPath, 0, &sfInfo, sizeof(sfInfo), SHGFI_ICON | SHGFI_SMALLICON))
			{
				::ImageList_AddIcon(hImage, sfInfo.hIcon);
				::DestroyIcon(sfInfo.hIcon);
			}
		}
		else
		{
			LOG_ERROR("Media player application is not found.");
			LOG_ERROR("The default application icon is used.");

			HICON hIcon = ::LoadIcon(NULL, IDI_APPLICATION);
			if(hIcon != NULL)
				::ImageList_AddIcon(hImage, hIcon);
		}

		::SendMessage(hwndToolbar, TB_SETIMAGELIST, 0, (LPARAM)hImage);
	}
	GUIProc_ResetToolbar(hwndToolbar);

	{
		REBARINFO barInfo;
		barInfo.cbSize = sizeof(barInfo);
		barInfo.fMask = 0;
		barInfo.himl = NULL;
		::SendMessage(hwndTaskbar, RB_SETBARINFO, 0, (LPARAM)&barInfo);
		::SendMessage(hwndCmdbar, RB_SETBARINFO, 0, (LPARAM)&barInfo);
	}

	{
		REBARBANDINFO bandInfo = {REBARBANDINFO_V6_SIZE};
		bandInfo.fMask = RBBIM_CHILD | RBBIM_CHILDSIZE | RBBIM_SIZE | RBBIM_STYLE;
		bandInfo.fStyle = RBBS_CHILDEDGE | RBBS_NOGRIPPER;

		{ //Taskbar
			bandInfo.cxMinChild = 0;
			bandInfo.cyMinChild = convertScaleY(23);
			bandInfo.cx = 100;
			bandInfo.hwndChild = ::CreateWindowEx(WS_EX_CONTROLPARENT, MAKEINTRESOURCE(DJEditImpl::Register()), NULL,
				WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VISIBLE,
				0, 0, bandInfo.cxMinChild, bandInfo.cyMinChild, hwndTaskbar, (HMENU)IDC_EDIT9, hInstance, 0);
			if(bandInfo.hwndChild)
				::SendMessage(hwndTaskbar, RB_INSERTBAND, -1, (LPARAM)&bandInfo);
		}

		{ //Cmdbar
			bandInfo.fStyle = RBBS_CHILDEDGE;
			bandInfo.hwndChild = hwndToolbar;
			{
				RECT rc = {0};
				LRESULT count = ::SendMessage(bandInfo.hwndChild, TB_BUTTONCOUNT, 0, 0);
				if(count > 0)
				{
					::SendMessage(bandInfo.hwndChild, TB_GETITEMRECT, count - 1, (LPARAM)&rc);
					bandInfo.cxMinChild = 0;
					bandInfo.cx = rc.right;
				}
				else
				{
					::GetWindowRect(bandInfo.hwndChild, &rc);
					bandInfo.cxMinChild = rc.right - rc.left;
					bandInfo.cx = bandInfo.cxMinChild;
				}
				bandInfo.cyMinChild = rc.bottom - rc.top;
			}
			::SendMessage(hwndCmdbar, RB_INSERTBAND, -1, (LPARAM)&bandInfo);
		}
	}

	return true;
}

static void GUIProc_InitializeSysMenu(HWND hwnd)
{
	MENUITEMINFO info = {sizeof(MENUITEMINFO)}, separator = {sizeof(MENUITEMINFO)};

	separator.fMask = MIIM_ID | MIIM_TYPE;
	separator.fType = MFT_SEPARATOR;
	separator.wID = 8000;

	info.fMask = MIIM_ID | MIIM_TYPE;
	info.fType = MFT_STRING;

	HMENU hSysMenu = ::GetSystemMenu(hwnd, FALSE);

	info.wID = IDC_CONFIG;
	info.dwTypeData = "設定(&O)...";
	::InsertMenuItem(hSysMenu, SC_CLOSE, false, &info);

	::InsertMenuItem(hSysMenu, SC_CLOSE, false, &separator);
}

BOOL GUIProc_OnCreate(HWND hwnd, LPCREATESTRUCT lpCreateStruct)
{
	guiWnd = hwnd;

	{
		WSingleLock lock(&selchanID_lock);
		selchanID.clear();
	}

	{
		INITCOMMONCONTROLSEX icce = {sizeof(INITCOMMONCONTROLSEX)};
		icce.dwICC = ICC_BAR_CLASSES | ICC_COOL_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_UPDOWN_CLASS;
		if(!::InitCommonControlsEx(&icce))
		{
			LOG_ERROR("InitCommonControlsEx function failed.");
			return FALSE;
		}
	}

	GUIProc_OnThemeChanged(hwnd);

	if(!GUIProc_CreateControls(hwnd, lpCreateStruct->hInstance))
	{
		LOG_ERROR("GUIProc_CreateControls function failed.");
		return FALSE;
	}
	::SendDlgItemMessage(hwnd, logID, WM_SETFONT, (WPARAM)::GetStockObject(DEFAULT_GUI_FONT), 0);
	if(!GUIProc_InitializeBar(hwnd, lpCreateStruct->hInstance))
	{
		LOG_ERROR("GUIProc_IntializeBar function failed.");
		return FALSE;
	}

	peercastApp->updateSettings();

	if(servMgr->autoServe)
	{
		::SendDlgItemMessage(::GetDlgItem(hwnd, cid_cmdbar), 0, TB_CHECKBUTTON, IDC_CHECK1, MAKELONG(TRUE, 0));
		::SendMessage(hwnd, WM_COMMAND, IDC_CHECK1, 0);
	}
	if(servMgr->autoConnect)
	{
		setButtonStateEx(hwnd, IDC_CHECK2, true);
		::SendMessage(hwnd, WM_COMMAND, IDC_CHECK2, 0);
	}

	guiThread.func = showConnections;
	guiThread.data = hwnd;
	if(!sys->startThread(&guiThread))
	{
		LOG_ERROR("Worker thread creation failed.");
		::MessageBox(hwnd,"GUIが開始できません","PeerCast",MB_OK|MB_ICONERROR);
		return FALSE;
	}

	if(guiFlg)
		::SetWindowPlacement(hwnd, &winPlace);

	GUIProc_InitializeSysMenu(hwnd);

	{
		HWND chanWnd = ::GetDlgItem(hwnd, chanID);
		HWND connWnd = ::GetDlgItem(hwnd, statusID);

		if(ThemeHelper::IsThemingSupported()) // for Vista (Explorer selection visuals)
		{
			::SetWindowTheme(chanWnd, L"Explorer", NULL);
			::SetWindowTheme(connWnd, L"Explorer", NULL);
		}

		// for PCRaw (connection list) start
		::SetWindowLongPtr(chanWnd, GWLP_USERDATA, ::GetWindowLongPtr(chanWnd, GWLP_WNDPROC));
		::SetWindowLongPtr(chanWnd, GWLP_WNDPROC, (LONG_PTR)ListBoxProc);
		::SetWindowLongPtr(connWnd, GWLP_USERDATA, ::GetWindowLongPtr(connWnd, GWLP_WNDPROC));
		::SetWindowLongPtr(connWnd, GWLP_WNDPROC, (LONG_PTR)ConnListBoxProc);
		// for PCRaw (connection list) end

		DWORD dwStyle = LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP | LVS_EX_SUBITEMIMAGES;
		ListView_SetExtendedListViewStyleEx(chanWnd, dwStyle, dwStyle);
		ListView_SetExtendedListViewStyleEx(connWnd, dwStyle, dwStyle);

		HIMAGELIST hImage = ImageList_LoadImage(lpCreateStruct->hInstance, MAKEINTRESOURCE(IDB_CHANLIST), 0, 10, CLR_DEFAULT, IMAGE_BITMAP, LR_CREATEDIBSECTION | LR_DEFAULTSIZE);
		if(hImage == NULL)
		{
			LOG_ERROR("Channel and connection list's listview creation failed.");
			return FALSE;
		}
		ImageList_SetOverlayImage(hImage, 9, 1);
		ListView_SetImageList(chanWnd, hImage, LVSIL_SMALL);
		ListView_SetImageList(connWnd, hImage, LVSIL_SMALL);
		ListView_SetCallbackMask(connWnd, LVIS_OVERLAYMASK);

		GUIProc_InitChannelListColumn(chanWnd);
		GUIProc_InitConnectionListColumn(connWnd);
	}

	return TRUE;
}

static void GUIProc_OnStartServer(HWND hwnd)
{
	HWND hwndCmdbar = ::GetDlgItem(hwnd, cid_cmdbar);
	if(!::IsWindow(hwndCmdbar))
		return;

	if(::SendDlgItemMessage(hwndCmdbar, 0, TB_ISBUTTONCHECKED, IDC_CHECK1, 0))
	{
		//writeSettings();
		servMgr->autoServe = true;
	}
	else
		servMgr->autoServe = false;

	setControls(hwnd, true);
}

static void GUIProc_OnDJMessage(HWND hwnd)
{
	HWND hwndTaskbar = ::GetDlgItem(hwnd, cid_taskbar);
	if(!::IsWindow(hwndTaskbar))
		return;

	String djMsgUNI;
	::GetDlgItemText(hwndTaskbar, IDC_EDIT9, djMsgUNI.cstr(), String::MAX_LEN);
	djMsgUNI.convertTo(String::T_UNICODE);
	chanMgr->setBroadcastMsg(djMsgUNI);
}

static void GUIProc_OnStartOutgoing(HWND hwnd)
{
	if(getButtonState(hwnd, IDC_CHECK2))
	{
		::SendDlgItemMessage(hwnd, IDC_COMBO1,WM_GETTEXT, 128, (LPARAM)servMgr->connectHost);
		servMgr->autoConnect = true;
		::EnableWindow(::GetDlgItem(hwnd, IDC_COMBO1), FALSE);
	}
	else
	{
		servMgr->autoConnect = false;
		::EnableWindow(::GetDlgItem(hwnd, IDC_COMBO1), TRUE);
	}
}

static void GUIProc_OnBroadcast(HWND hwnd)
{
	Host sh = servMgr->serverHost;
	if (sh.isValid())
	{
		char cmd[256];
		sprintf(cmd,"http://localhost:%d/admin?page=broadcast",sh.port);
		::ShellExecute(hwnd, NULL, cmd, NULL, NULL, SW_SHOWNORMAL);

	}
	else
		::MessageBox(hwnd,"Server is not currently connected.\nPlease wait until you have a connection.","PeerCast",MB_OK);
}

static void GUIProc_OnPlaySelected(HWND hwnd)
{
	class Local : public EnumSelectedChannels
	{
		bool discovered(Channel *c)
		{
			chanMgr->playChannel(c->info);
			return true;
		}
	} my;
	my.enumerate(hwnd);
}

static void GUIProc_OnServentDisconnect(HWND hwnd)
{
	WSingleLock lock(&servMgr->lock);
	Servent *s = getListBoxServent(hwnd);

	if(s != NULL)
	{
		s->thread.active = false;
		s->thread.finish = true;
	}

	sleep_skip = true;
}

static void GUIProc_OnChanDisconnect(HWND hwnd)
{
	class Local : public EnumSelectedChannels
	{
		bool discovered(Channel *c)
		{
			c->thread.active = false;
			c->thread.finish = true;
			return true;
		}
	} my;
	my.enumerate(hwnd);

	sleep_skip = true;
}

static void GUIProc_OnChanBump(HWND hwnd)
{
	class Local : public EnumSelectedChannels
	{
		bool discovered(Channel *c)
		{
			c->bump = true;
			return true;
		}
	} my;
	my.enumerate(hwnd);
}

static void GUIProc_OnGetChannel(HWND hwnd)
{
	WSingleLock lock(&chanMgr->hitlistlock);
	ChanHitList *chl = (ChanHitList *)getListBoxSelData(hwnd, hitID);

	if(chl != NULL)
	{
		if(!chanMgr->findChannelByID(chl->info.id))
		{
			WSingleLock lock(&chanMgr->channellock);
			Channel *channel = chanMgr->createChannel(chl->info,NULL);
			if(channel != NULL)
				channel->startGet();
		}
	}
	else
		::MessageBox(hwnd,"Please select a channel","PeerCast",MB_OK);
}

static void GUIProc_OnChanKeep(HWND hwnd)
{
	class Local : public EnumSelectedChannels
	{
		bool discovered(Channel *c)
		{
			c->stayConnected = !c->stayConnected;
			return true;
		}
	} my;
	my.enumerate(hwnd);
}

static void GUIProc_OnStealth(HWND hwnd)
{
	class Local : public EnumSelectedChannels
	{
		bool discovered(Channel *c)
		{
			c->stealth = !c->stealth;
			return true;
		}
	} my;
	my.enumerate(hwnd);
}

static void GUIProc_OnClap(HWND hwnd)
{
	class Local : public EnumSelectedChannels
	{
		bool discovered(Channel *c)
		{
			c->bClap = true;
			return true;
		}
	} my;
	my.enumerate(hwnd);
}

static void GUIProc_OnClearLog(HWND hwnd)
{
	::SendDlgItemMessage(hwnd, logID, LB_RESETCONTENT, 0, 0);
	sys->logBuf->clear();	// for PCRaw (clear log)
}

static void GUIProc_OnFind(HWND hwnd)
{
	char str[64];
	::SendDlgItemMessage(hwnd, IDC_EDIT2,WM_GETTEXT, 64, (LPARAM)str);
	::SendDlgItemMessage(hwnd, hitID, LB_RESETCONTENT, 0, 0);
	ChanInfo info;
	info.init();
	info.name.set(str);
	chanMgr->startSearch(info);
}

void GUIProc_OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify)
{
	switch(id)
	{
	case IDC_CHECK1:	return GUIProc_OnStartServer(hwnd);			// start server
	case IDC_CHECK11:	return GUIProc_OnDJMessage(hwnd);			// DJ message
	case IDC_LOGDEBUG:		// log debug
		servMgr->showLog ^= 1<<LogBuffer::T_DEBUG;
		break;
	case IDC_LOGERRORS:		// log errors
		servMgr->showLog ^= 1<<LogBuffer::T_ERROR;
		break;
	case IDC_LOGNETWORK:	// log network
		servMgr->showLog ^= 1<<LogBuffer::T_NETWORK;
		break;
	case IDC_LOGCHANNELS:	// log channels
		servMgr->showLog ^= 1<<LogBuffer::T_CHANNEL;
		break;
	case IDC_CHECK9:		// pause log
		servMgr->pauseLog = !servMgr->pauseLog;
		break;
	case IDC_CHECK2:	return GUIProc_OnStartOutgoing(hwnd);		// start outgoing
	case IDC_BUTTON11:	return GUIProc_OnBroadcast(hwnd);			// broadcast
	case IDC_BUTTON8:	return GUIProc_OnPlaySelected(hwnd);		// play selected
	case IDC_BUTTON7:		// advanced
		sys->callLocalURL("admin?page=settings",servMgr->serverHost.port);
		break;
	case IDC_BUTTON6:	return GUIProc_OnServentDisconnect(hwnd);	// servent disconnect
	case IDC_BUTTON5:	return GUIProc_OnChanDisconnect(hwnd);		// chan disconnect
	case IDC_BUTTON3:	return GUIProc_OnChanBump(hwnd);			// chan bump
	case IDC_BUTTON4:	return GUIProc_OnGetChannel(hwnd);			// get channel
	case IDC_BUTTON9:	return GUIProc_OnChanKeep(hwnd);			//JP-EX chan keep
	case IDC_STEALTH:	return GUIProc_OnStealth(hwnd);				//JP-MOD
	case IDC_CLAP:		return GUIProc_OnClap(hwnd);				//JP-MOD
	case IDC_BUTTON1:	return GUIProc_OnClearLog(hwnd);			// clear log
	case IDC_BUTTON2:	return GUIProc_OnFind(hwnd);				// find
	}
}

void GUIProc_OnSysCommand(HWND hwnd, UINT cmd, int x, int y)
{
	if(cmd == IDC_CONFIG)
	{
		GUIConfig config;
		if(config.Show(hwnd) > 0)
		{
			setControls(hwnd, true);
			::PostMessage(hwnd, WM_REFRESH, 0, 0);

			if(servMgr->guiAntennaNotifyIcon)
				::PostMessage(mainWnd, WM_ANTENNA, 0, 0);
		}
	}

	FORWARD_WM_SYSCOMMAND(hwnd, cmd, x, y, DefWindowProc);
}

static void GUIProc_OnChanListClick(HWND hwnd, NMITEMACTIVATE &nmItem)
{
	if(nmItem.iItem == -1)
		return;

	switch(nmItem.iSubItem)
	{
	case 4:
		{
			int currentOverrideMaxRelaysPerChannel = chanMgr->maxRelaysPerChannel;
			{
				HWND chanWnd = GetDlgItem(hwnd, chanID);

				WSingleLock lock(&chanMgr->channellock);
				Channel *channel = (::IsWindow(chanWnd)) ? getListBoxChannel(chanWnd) : NULL;

				if(channel != NULL && channel->overrideMaxRelaysPerChannel >= 1)
					currentOverrideMaxRelaysPerChannel = channel->overrideMaxRelaysPerChannel;
			}

			TPMPARAMS tpmParams = {sizeof(TPMPARAMS)};
			if(ListView_GetSubItemRect(nmItem.hdr.hwndFrom, nmItem.iItem, nmItem.iSubItem, LVIR_BOUNDS, &tpmParams.rcExclude))
			{
				HMENU hMenu = ::CreatePopupMenu();
				if(hMenu == NULL)
					break;

				MENUITEMINFO menuInfo = {sizeof(MENUITEMINFO)};
				menuInfo.fMask = MIIM_ID | MIIM_STATE | MIIM_TYPE;
				menuInfo.fType = MFT_STRING;

				for(menuInfo.wID = 1;
					menuInfo.wID <= servMgr->maxRelays;
					menuInfo.wID++)
				{
					char szNumber[0xF];
					sprintf_s(szNumber, _countof(szNumber), "%d", menuInfo.wID);

					menuInfo.fState = (menuInfo.wID == currentOverrideMaxRelaysPerChannel) ? MFS_CHECKED : MFS_UNCHECKED;
					menuInfo.dwTypeData = szNumber;
					::InsertMenuItem(hMenu, -1, true, &menuInfo);
				}

				{
					MapWindowPoints(nmItem.hdr.hwndFrom, HWND_DESKTOP, (LPPOINT)&tpmParams.rcExclude, 2);

					DWORD dwResult = ::TrackPopupMenuEx(hMenu, TPM_LEFTALIGN | TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_VERTICAL,
						tpmParams.rcExclude.left, tpmParams.rcExclude.bottom, nmItem.hdr.hwndFrom, &tpmParams);
					::PostMessage(hwnd, WM_NULL, 0, 0);

					if(dwResult != 0)
					{
						class Local : public EnumSelectedChannels
						{
							int newOverrideMaxRelaysPerChannel;

							bool discovered(Channel *c)
							{
								c->overrideMaxRelaysPerChannel = newOverrideMaxRelaysPerChannel;
								return true;
							}
						public:
							Local(int value)
							{
								newOverrideMaxRelaysPerChannel = (value >= 1) ? value : -1;
							}
						} my(dwResult);
						my.enumerate(hwnd);
					}
				}

				::DestroyMenu(hMenu);
			}
		}
		break;
	case 7:
		::PostMessage(hwnd, WM_COMMAND, IDC_BUTTON9, NULL);
		break;
	}
}

static void GUIProc_OnChanListItemChanged(HWND hwnd, NMLISTVIEW &nmv)
{
	UNREFERENCED_PARAMETER(nmv);

	HWND hwndCmdbar = ::GetDlgItem(hwnd, cid_cmdbar);
	if(!::IsWindow(hwndCmdbar))
		return;

	class Local : public EnumSelectedChannels
	{
		bool discovered(Channel *c)
		{
			if(c->info.ppFlags & ServMgr::bcstClap)
				clap = true;
#ifdef PP_PUBLISH
			if(c->status == Channel::S_BROADCASTING)
				stealth = true;
#endif /* PP_PUBLISH */
			return true;
		}

	public:
		bool clap;
#ifdef PP_PUBLISH
		bool stealth;
#endif /* PP_PUBLISH */
		Local() : clap(false)
#ifdef PP_PUBLISH
			, stealth(false)
#endif /* PP_PUBLISH */
		{}
	} my;

	{
		WSingleLock lock(&chanMgr->channellock);

		Channel *channel = getListBoxChannel(nmv.hdr.hwndFrom);
		{
			WSingleLock lock(&selchanID_lock);
			if(channel != NULL)
				selchanID = channel->info.id;
			else
				selchanID.clear();
		}
	}

	if(my.enumerate(hwnd))
	{
		::SendDlgItemMessage(hwndCmdbar, 0, TB_ENABLEBUTTON, IDC_CLAP,	MAKELONG(my.clap, 0));
#ifdef PP_PUBLISH
		::SendDlgItemMessage(hwndCmdbar, 0, TB_ENABLEBUTTON, IDC_STEALTH,	MAKELONG(my.stealth, 0));
#else /* PP_PUBLISH */
		::SendDlgItemMessage(hwndCmdbar, 0, TB_ENABLEBUTTON, IDC_STEALTH,	MAKELONG(TRUE, 0));
#endif /* PP_PUBLISH */
	}
	else
	{
		::SendDlgItemMessage(hwndCmdbar, 0, TB_ENABLEBUTTON, IDC_CLAP,	MAKELONG(FALSE, 0));
		::SendDlgItemMessage(hwndCmdbar, 0, TB_ENABLEBUTTON, IDC_STEALTH,	MAKELONG(FALSE, 0));
	}
}

static void GUIProc_OnChanListBeginDrag(HWND hwnd, NMLISTVIEW &nmv)
{
	WSingleLock lock(&ld_lock);
	ListData *ld = getListData(nmv.iItem);

	if(ld)
	{
		//Not implemented.
	}
}

static void GUIProc_OnChanListGetDispInfo(HWND hwnd, NMLVDISPINFO &nmv)
{
	LVITEM &lvItem = nmv.item;

	WSingleLock lock(&ld_lock);
	ListData *ld = getListData(lvItem.iItem);
	if(ld == NULL)
		return;

	if(lvItem.mask & LVIF_TEXT)
	{
		switch(lvItem.iSubItem)
		{
		case 0:
			strncpy_s(lvItem.pszText, lvItem.cchTextMax, ld->name, _TRUNCATE);
			break;
		case 1:
			_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%dkbps", ld->bitRate);
			break;
		case 2:
			strncpy_s(lvItem.pszText, lvItem.cchTextMax, ld->statusStr, _TRUNCATE);
			break;
		case 3:
			_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%d / %d", ld->totalListeners, ld->totalRelays);
			break;
		case 4:
			_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%d / %d", ld->localListeners, ld->localRelays);
			break;
		case 5:
#ifdef PP_PUBLISH
			if(ld->status == Channel::S_BROADCASTING)
#endif /* PP_PUBLISH */
				_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%d", ld->totalClaps);
			break;
		case 6:
			_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%d", ld->skipCount);
			break;
		}
	}

	if(lvItem.mask & LVIF_IMAGE)
	{
		switch(lvItem.iSubItem)
		{
		case 0:
			lvItem.iImage =
				(ld->status == Channel::S_RECEIVING) ?
				(ld->chDisp.status == Channel::S_RECEIVING) ?
				(ld->chDisp.relay) ? 2
				: (ld->chDisp.numRelays) ? 3
				: 4
				: 1
				: 0;
			break;
		case 1:
			if(ld->linkQuality >= 0)
				lvItem.iImage = 5 + ld->linkQuality;
			break;
		}
	}
}

static LRESULT GUIProc_OnChanListCustomDraw(HWND hwnd, NMLVCUSTOMDRAW &NMCustomDraw)
{
	switch(NMCustomDraw.nmcd.dwDrawStage)
	{
	case CDDS_PREPAINT:
		return CDRF_NOTIFYITEMDRAW;
	case CDDS_ITEMPREPAINT:
		if(servMgr->getFirewall() == ServMgr::FW_ON)
		{
			NMCustomDraw.clrText = RGB(255, 0, 0);
			return CDRF_DODEFAULT;
		}
		return CDRF_NOTIFYSUBITEMDRAW;
	case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
		{
			NMCustomDraw.clrText = ::GetSysColor(COLOR_WINDOWTEXT);

			WSingleLock lock(&ld_lock);
			ListData *ld = getListData(NMCustomDraw.nmcd.dwItemSpec);
			if(ld)
			{
				switch(NMCustomDraw.iSubItem)
				{
				case 0:
					if(ld->bTracker && (ld->status == Channel::S_RECEIVING))
						NMCustomDraw.clrText = RGB(0, 128, 0);
					break;
				case 4:
					if(ld->stealth)
						NMCustomDraw.clrText = GetSysColor(COLOR_GRAYTEXT);
					if(NMCustomDraw.nmcd.uItemState & CDIS_FOCUS)
						return CDRF_NOTIFYPOSTPAINT;
					break;
				case 7:
					return CDRF_NOTIFYPOSTPAINT;
				}
			}
		}
		return CDRF_DODEFAULT;
	case CDDS_ITEMPOSTPAINT | CDDS_SUBITEM:
		{
			RECT rc = NMCustomDraw.nmcd.rc;
			ListView_GetSubItemRect(NMCustomDraw.nmcd.hdr.hwndFrom, NMCustomDraw.nmcd.dwItemSpec, NMCustomDraw.iSubItem, LVIR_LABEL, &rc);

			switch(NMCustomDraw.iSubItem)
			{
			case 4:
				{
					RECT rcClip = rc;
					rcClip.left = rcClip.right - ::GetSystemMetrics(SM_CXVSCROLL);
					if(rc.left <= rcClip.left)
					{
						HTHEME hTheme = GUIProc_GetWindowLongTheme(hwnd, tid_combobox);

						if(hTheme != NULL)
						{
							int iStateId = (NMCustomDraw.nmcd.uItemState & CDIS_HOT) ? CBXS_HOT : CBXS_NORMAL;

							if(::IsThemePartDefined(hTheme, CP_READONLY, 0)) // for Vista
							{
								::DrawThemeBackground(hTheme, NMCustomDraw.nmcd.hdc, CP_READONLY, iStateId, &rc, &rc);

								RECT rcExcludeClip;
								if(::SubtractRect(&rcExcludeClip, &rc, &rcClip))
								{
									RECT rcContent;
									if(SUCCEEDED(::GetThemeBackgroundContentRect(hTheme, NMCustomDraw.nmcd.hdc, CP_READONLY, iStateId, &rcExcludeClip, &rcContent)))
									{
										char szText[0x10] = {0};
										WCHAR szwText[0x10];
										ListView_GetItemText(NMCustomDraw.nmcd.hdr.hwndFrom, NMCustomDraw.nmcd.dwItemSpec, NMCustomDraw.iSubItem, szText, _countof(szText));
										if(::MultiByteToWideChar(CP_OEMCP, MB_PRECOMPOSED, szText, -1, szwText, _countof(szwText)))
											::DrawThemeText(hTheme, NMCustomDraw.nmcd.hdc, CP_READONLY, iStateId, szwText, -1, DT_NOPREFIX, 0, &rcContent);
									}
								}
								::DrawThemeBackground(hTheme, NMCustomDraw.nmcd.hdc, CP_DROPDOWNBUTTONRIGHT, CBXSR_NORMAL, &rcClip, &rcClip);
							}
							else
								::DrawThemeBackground(hTheme, NMCustomDraw.nmcd.hdc, CP_DROPDOWNBUTTON, iStateId, &rcClip, &rcClip);
						}
						else
							::DrawFrameControl(NMCustomDraw.nmcd.hdc, &rcClip, DFC_SCROLL, DFCS_SCROLLCOMBOBOX | DFCS_TRANSPARENT);
					}
				}
				break;
			case 7:
				{
					WSingleLock lock(&ld_lock);
					ListData *ld = getListData(NMCustomDraw.nmcd.dwItemSpec);
					if(ld)
					{
						HTHEME hTheme = GUIProc_GetWindowLongTheme(hwnd, tid_button);

						if(hTheme != NULL)
						{
							int iStateId = (ld->stayConnected) ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;

							if(NMCustomDraw.nmcd.uItemState & CDIS_HOT)
								++iStateId;

							::DrawThemeBackground(hTheme, NMCustomDraw.nmcd.hdc, BP_CHECKBOX, iStateId, &rc, &rc);
						}
						else
						{
							UINT uState = DFCS_BUTTONCHECK | DFCS_TRANSPARENT;
							if(ld->stayConnected)
								uState |= DFCS_CHECKED;

							::DrawFrameControl(NMCustomDraw.nmcd.hdc, &rc, DFC_BUTTON, uState);
						}
					}
				}
				break;
			}
		}
		return CDRF_DODEFAULT;
	}

	return CDRF_DODEFAULT;
}

static void GUIProc_OnConnListGetDispInfo(HWND hwnd, NMLVDISPINFO &nmv)
{
	LVITEM &lvItem = nmv.item;
	WSingleLock lock(&sd_lock);
	ServentData *sd = getServentData(lvItem.iItem);

	if(sd)
	{
		if(lvItem.mask & LVIF_TEXT)
		{
			switch(lvItem.iSubItem)
			{
			case 0:
				if(sd->type == Servent::T_RELAY && sd->status == Servent::S_CONNECTED)
				{
					int pos = 0;
					if(sd->lastSkipCount)
						pos = _snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "(%d)", sd->lastSkipCount);
					if(pos >= 0)
					{
						strncpy_s(lvItem.pszText + pos, lvItem.cchTextMax - pos, "RELAYING", _TRUNCATE);
					}
				}
				else
					_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%s-%s", sd->typeStr, sd->statusStr);
				break;
			case 1:
				_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%d秒", sd->tnum);
				break;
			case 2:
				if(sd->type == Servent::T_RELAY)
					_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%d/%d", sd->totalListeners, sd->totalRelays);
				break;
			case 3:
				strncpy_s(lvItem.pszText, lvItem.cchTextMax, sd->hostName.cstr(), _TRUNCATE);
				break;
			case 4:
				_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%d",
					/*(sd->type == Servent::T_SERVER) ?*/ sd->h.port /*: ntohs(sd->h.port)*/);
				break;
			case 5:
				if (sd->type == Servent::T_RELAY	||
					sd->type == Servent::T_DIRECT	||
					sd->status == Servent::S_CONNECTED
					)
					_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%d", sd->syncpos);
				break;
			case 6:
				if (sd->type != Servent::T_RELAY	&&
					sd->type != Servent::T_DIRECT	&&
					sd->status == Servent::S_CONNECTED
					)
				{
					_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "luid/%Ix", sd->agent.cstr()); //'Local Unique Identifier' generate from pointer.
				}
				else
				{
					strncpy_s(lvItem.pszText, lvItem.cchTextMax, sd->agent.cstr(), _TRUNCATE);
				}
				break;
			case 7:
				if(sd->ver_ex_number)
					_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "%c%c%02d", sd->ver_ex_prefix[0], sd->ver_ex_prefix[1], sd->ver_ex_number);
				else if(sd->vp_ver)
					_snprintf_s(lvItem.pszText, lvItem.cchTextMax, _TRUNCATE, "VP%02d", sd->vp_ver);
				break;
			}
		}

		if(lvItem.mask & LVIF_IMAGE)
		{
			switch(lvItem.iSubItem)
			{
			case 0:
				lvItem.iImage =
					(sd->type == Servent::T_RELAY) ?
					(sd->infoFlg) ?
					(sd->relay) ? 2
					: (sd->numRelays) ? 3
					: 4
					: 1
					: 0;
				break;
			}
		}

		if((lvItem.mask & LVIF_STATE) && sd->lastSkipTime + 120 > sys->getTime())
			lvItem.state = INDEXTOOVERLAYMASK(1);
	}
}

static LRESULT GUIProc_OnConnListCustomDraw(HWND hwnd, NMLVCUSTOMDRAW &NMCustomDraw)
{
	switch(NMCustomDraw.nmcd.dwDrawStage)
	{
	case CDDS_PREPAINT:
		return CDRF_NOTIFYITEMDRAW;
	case CDDS_ITEMPREPAINT:
		{
			WSingleLock lock(&sd_lock);
			ServentData *sd = getServentData(NMCustomDraw.nmcd.dwItemSpec);

			if(sd && sd->firewalled)
				NMCustomDraw.clrText = (!sd->numRelays) ? RGB(255, 0, 0) : RGB(255, 168, 0);
		}
		return CDRF_DODEFAULT;
	}

	return CDRF_DODEFAULT;
}

static BOOL GUIProc_OnCommandBarGetButtonInfo(HWND hwnd, NMTOOLBAR &nmToolbar)
{
	if(nmToolbar.iItem < ARRAYSIZE(tbButtons))
	{
		nmToolbar.tbButton = tbButtons[nmToolbar.iItem];
		return TRUE;
	}
	return FALSE;
}

static LRESULT GUIProc_OnCommandBarDropDown(HWND hwnd, NMTOOLBAR &nmToolbar)
{
	switch(nmToolbar.iItem)
	{
	case IDC_CHECK9:
		{
			HMENU logMenu = ::LoadMenu(GetWindowInstance(hwnd), MAKEINTRESOURCE(IDR_LOGMENU));

			if(logMenu)
			{
				HMENU subMenu = ::GetSubMenu(logMenu, 0);

				if(subMenu)
				{
					{
						MENUITEMINFO menuInfo;

						menuInfo.cbSize = sizeof(MENUITEMINFO);
						menuInfo.fMask = MIIM_STATE;

						menuInfo.fState = ((servMgr->showLog&(1<<LogBuffer::T_DEBUG))!=0) ? MFS_CHECKED : MFS_UNCHECKED;
						::SetMenuItemInfo(subMenu, IDC_LOGDEBUG, FALSE, &menuInfo);

						menuInfo.fState = ((servMgr->showLog&(1<<LogBuffer::T_ERROR))!=0) ? MFS_CHECKED : MFS_UNCHECKED;
						::SetMenuItemInfo(subMenu, IDC_LOGERRORS, FALSE, &menuInfo);

						menuInfo.fState = ((servMgr->showLog&(1<<LogBuffer::T_NETWORK))!=0) ? MFS_CHECKED : MFS_UNCHECKED;
						::SetMenuItemInfo(subMenu, IDC_LOGNETWORK, FALSE, &menuInfo);

						menuInfo.fState = ((servMgr->showLog&(1<<LogBuffer::T_CHANNEL))!=0) ? MFS_CHECKED : MFS_UNCHECKED;
						::SetMenuItemInfo(subMenu, IDC_LOGCHANNELS, FALSE, &menuInfo);

						::EnableMenuItem(subMenu, IDC_BUTTON1, (SendDlgItemMessage(hwnd, IDC_LIST1, LB_GETCOUNT, 0, 0) > 0) ? MF_ENABLED : MF_GRAYED);
					}

					{
						TPMPARAMS tpmParams;
						tpmParams.cbSize = sizeof(tpmParams);
						tpmParams.rcExclude = nmToolbar.rcButton;
						::MapWindowPoints(nmToolbar.hdr.hwndFrom, HWND_DESKTOP, (LPPOINT)&tpmParams.rcExclude, 2);

						::TrackPopupMenuEx(subMenu, TPM_LEFTALIGN | TPM_LEFTBUTTON | TPM_VERTICAL, tpmParams.rcExclude.left, tpmParams.rcExclude.bottom, hwnd, &tpmParams);
						::PostMessage(hwnd, WM_NULL, 0, 0);
					}
				}

				::DestroyMenu(logMenu);
			}
		}
		return TBDDRET_DEFAULT;
	}

	return TBDDRET_NODEFAULT;
}

LRESULT GUIProc_OnNotify(HWND hwnd, int idCtrl, LPNMHDR pnmh)
{
	HWND hwndCmdbar = ::GetDlgItem(hwnd, cid_cmdbar);
	if(!::IsWindow(hwndCmdbar))
		return 0;
	hwndCmdbar = ::GetDlgItem(hwndCmdbar, 0);
	if(!::IsWindow(hwndCmdbar))
		return 0;

	if(idCtrl == IDC_LIST3)
	{
		switch(pnmh->code)
		{
		case NM_CLICK:			return GUIProc_OnChanListClick(hwnd, *((LPNMITEMACTIVATE)pnmh)), 0L;
		case LVN_ITEMCHANGED:	return GUIProc_OnChanListItemChanged(hwnd, *((LPNMLISTVIEW)pnmh)), 0L;
		case LVN_BEGINDRAG:		return GUIProc_OnChanListBeginDrag(hwnd, *((LPNMLISTVIEW)pnmh)), 0L;
		case LVN_GETDISPINFO:	return GUIProc_OnChanListGetDispInfo(hwnd, *((NMLVDISPINFO*)pnmh)), 0L;
		case NM_CUSTOMDRAW:		return GUIProc_OnChanListCustomDraw(hwnd, *((LPNMLVCUSTOMDRAW)pnmh));
		}
	}
	else if(idCtrl == IDC_LIST2)
	{
		switch(pnmh->code)
		{
		case LVN_GETDISPINFO:	return GUIProc_OnConnListGetDispInfo(hwnd, *((NMLVDISPINFO*)pnmh)), 0L;
		case NM_CUSTOMDRAW:		return GUIProc_OnConnListCustomDraw(hwnd, *((LPNMLVCUSTOMDRAW)pnmh));
		}
	}
	else if(pnmh->hwndFrom == hwndCmdbar)
	{
		switch(pnmh->code)
		{
		case TBN_GETBUTTONINFO:	return GUIProc_OnCommandBarGetButtonInfo(hwnd, *((LPNMTOOLBAR)pnmh));
		case TBN_QUERYINSERT:	return TRUE;
		case TBN_QUERYDELETE:	return TRUE;
		case TBN_INITCUSTOMIZE:	return TBNRF_HIDEHELP;
		case TBN_RESET:			return GUIProc_ResetToolbar(pnmh->hwndFrom), 0L;
		case TBN_DROPDOWN:		return GUIProc_OnCommandBarDropDown(hwnd, *((LPNMTOOLBAR)pnmh));
		}
	}

	return 0;
}

void GUIProc_OnRefresh(HWND hwnd)
{
	RECT rc;
	if(GetClientRect(hwnd, &rc))
		GUIProc_OnSize(hwnd, SIZE_RESTORED, rc.right, rc.bottom);
}

void GUIProc_OnChanInfoChanged(HWND hwnd)
{
	class Local : public EnumSelectedChannels
	{
		bool discovered(Channel *c)
		{
			if(c->info.ppFlags & ServMgr::bcstClap)
				enable = true;
			return !enable;
		}
	public:
		bool enable;

		Local() : enable(false)	{}
	} my;
	my.enumerate(hwnd);
	::SendDlgItemMessage(::GetDlgItem(hwnd, cid_cmdbar), 0, TB_ENABLEBUTTON, IDC_CLAP, MAKELONG(my.enable, 0));
}

void GUIProc_OnClose(HWND hwnd)
{
	::GetWindowPlacement(hwnd, &winPlace);
	guiFlg = true;

	::DestroyWindow(hwnd);
}

void GUIProc_OnDestroy(HWND hwnd)
{
	{ //JP-MOD
		HIMAGELIST hImage = ListView_SetImageList(::GetDlgItem(hwnd, IDC_LIST3), NULL, LVSIL_SMALL);
		ListView_SetImageList(::GetDlgItem(hwnd, IDC_LIST2), NULL, LVSIL_SMALL);
		if(hImage)
			ImageList_Destroy(hImage);

		GUIProc_CloseThemes(hwnd);
	}

	::GetWindowPlacement(hwnd, &winPlace);
	guiFlg = true;

	guiThread.active = false;
	//guiThread.lock();
	guiWnd = NULL;
	//guiThread.unlock();
}

static void GUIProc_UpdateNavbarTheme(HWND hwnd, bool fDWMEnabled, bool fActivated)
{
	LPWSTR pwStr;
	if(fDWMEnabled)
		pwStr = (fActivated) ? L"NavbarComposited" : L"InactiveNavbarComposited";
	else
		pwStr = (fActivated) ? L"Navbar" : L"InactiveNavbar";

	::SendDlgItemMessage(hwnd, cid_taskbar, RB_SETWINDOWTHEME, 0, (LPARAM)pwStr);
}

void GUIProc_OnActivate(HWND hwnd, UINT state, HWND hwndActDeact, BOOL fMinimized)
{
	if(ThemeHelper::IsDwmApiSupported())
	{
		BOOL fEnabled;
		HRESULT hr = ::DwmIsCompositionEnabled(&fEnabled);
		if(SUCCEEDED(hr))
			GUIProc_UpdateNavbarTheme(hwnd, fEnabled != FALSE, state != WA_INACTIVE);
	}
}

bool GUIProc_GetTransparentMargins(HWND hwnd, MARGINS &margins)
{
	HWND hwndCmdbar = ::GetDlgItem(hwnd, cid_cmdbar);
	RECT rect;
	if(!::IsWindow(hwndCmdbar) || !::GetWindowRect(hwndCmdbar, &rect))
		return false;

	::MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rect, 2);

	margins.cyTopHeight = rect.top;
	return true;
}

void GUIProc_OnThemeChanged(HWND hwnd)
{
	if(ThemeHelper::IsThemingSupported())
	{
		GUIProc_UpdateTheme(hwnd, tid_button, VSCLASS_BUTTON);
		GUIProc_UpdateTheme(hwnd, tid_combobox, VSCLASS_COMBOBOX);
	}
}

void GUIProc_OnDwmCompositionChanged(HWND hwnd)
{
	if(ThemeHelper::IsDwmApiSupported())
	{
		BOOL fEnabled;
		HRESULT hr = ::DwmIsCompositionEnabled(&fEnabled);
		if(SUCCEEDED(hr))
		{
			MARGINS margins = {0};
			if(fEnabled)
				GUIProc_GetTransparentMargins(hwnd, margins);
			::DwmExtendFrameIntoClientArea(hwnd, &margins);

			WINDOWINFO windowInfo = {sizeof(WINDOWINFO)};
			::GetWindowInfo(hwnd, &windowInfo);

			GUIProc_UpdateNavbarTheme(hwnd, fEnabled != FALSE, windowInfo.dwWindowStatus == WS_ACTIVECAPTION);
		}
	}
}

LRESULT CALLBACK GUIConfig::PropGeneralProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) //JP-MOD
{
	UNREFERENCED_PARAMETER(wParam);
	switch(message)
	{
	case WM_INITDIALOG:
		setButtonStateEx(hwnd, IDC_SIMPLE_CHANNELLIST, servMgr->guiSimpleChannelList);
		setButtonStateEx(hwnd, IDC_SIMPLE_CONNECTIONLIST, servMgr->guiSimpleConnectionList);
		setButtonStateEx(hwnd, IDC_TOPMOST, servMgr->guiTopMost);
		break;
	case WM_NOTIFY:
		switch(((NMHDR*)lParam)->code)
		{
		case PSN_APPLY:
			servMgr->guiSimpleChannelList = getButtonState(hwnd, IDC_SIMPLE_CHANNELLIST);
			servMgr->guiSimpleConnectionList = getButtonState(hwnd, IDC_SIMPLE_CONNECTIONLIST);
			servMgr->guiTopMost = getButtonState(hwnd, IDC_TOPMOST);
			return TRUE;
		}
		break;
	}

	return FALSE;
}

LRESULT CALLBACK GUIConfig::PropTitleProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) //JP-MOD
{
	switch(message)
	{
	case WM_INITDIALOG:
		setButtonStateEx(hwnd, IDC_TITLEMODIFY, servMgr->guiTitleModify);
		::SendMessage(hwnd, WM_COMMAND, IDC_TITLEMODIFY, 0);

		::SetDlgItemText(hwnd, IDC_TITLENORMAL, servMgr->guiTitleModifyNormal.cstr());
		::SetDlgItemText(hwnd, IDC_TITLEMINIMIZED, servMgr->guiTitleModifyMinimized.cstr());
		break;
	case WM_NOTIFY:
		switch(((NMHDR*)lParam)->code)
		{
		case PSN_APPLY:
			servMgr->guiTitleModify = getButtonState(hwnd, IDC_TITLEMODIFY);
			::GetDlgItemText(hwnd, IDC_TITLENORMAL, servMgr->guiTitleModifyNormal.cstr(), String::MAX_LEN);
			::GetDlgItemText(hwnd, IDC_TITLEMINIMIZED, servMgr->guiTitleModifyMinimized.cstr(), String::MAX_LEN);
			return TRUE;
		}
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDC_TITLEMODIFY:
			{
				bool isChecked = getButtonState(hwnd, IDC_TITLEMODIFY);
				::EnableWindow(GetDlgItem(hwnd, IDC_TITLENORMAL), isChecked);
				::EnableWindow(GetDlgItem(hwnd, IDC_TITLEMINIMIZED), isChecked);
			}
			return TRUE;
		}
		break;
	}

	return FALSE;
}

LRESULT CALLBACK GUIConfig::PropLayoutProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) //JP-MOD
{
	UNREFERENCED_PARAMETER(wParam);
	switch(message)
	{
	case WM_INITDIALOG:
		::SetDlgItemInt(hwnd, IDC_CHANLIST_DISPLAYS, servMgr->guiChanListDisplays, TRUE);
		::SetDlgItemInt(hwnd, IDC_CONNLIST_DISPLAYS, servMgr->guiConnListDisplays, TRUE);

		::SendDlgItemMessage(hwnd, IDC_SPIN_CHANLIST_DISPLAYS, UDM_SETRANGE32, -1, 99);
		::SendDlgItemMessage(hwnd, IDC_SPIN_CONNLIST_DISPLAYS, UDM_SETRANGE32, -1, 99);
		break;
	case WM_NOTIFY:
		switch(((NMHDR*)lParam)->code)
		{
		case PSN_APPLY:
			servMgr->guiChanListDisplays = ::GetDlgItemInt(hwnd, IDC_CHANLIST_DISPLAYS, NULL, TRUE);
			servMgr->guiConnListDisplays = ::GetDlgItemInt(hwnd, IDC_CONNLIST_DISPLAYS, NULL, TRUE);
			return TRUE;
		}
		break;
	}

	return FALSE;
}

LRESULT CALLBACK GUIConfig::PropExpansionProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) //JP-MOD
{
	switch(message)
	{
	case WM_INITDIALOG:
		setButtonStateEx(hwnd, IDC_CLAP_ENABLESOUND, servMgr->ppClapSound);
		::SetDlgItemText(hwnd, IDC_CLAP_SOUNDPATH, servMgr->ppClapSoundPath.cstr());
		{
			HWND hwndPath = ::GetDlgItem(hwnd, IDC_CLAP_SOUNDPATH);
			if(::IsWindow(hwndPath))
				::SHAutoComplete(hwndPath, SHACF_FILESYSTEM);
		}

		setButtonStateEx(hwnd, IDC_ANTENNA_ICON, servMgr->guiAntennaNotifyIcon);
		break;
	case WM_NOTIFY:
		switch(((NMHDR*)lParam)->code)
		{
		case PSN_APPLY:
			servMgr->ppClapSound = getButtonState(hwnd, IDC_CLAP_ENABLESOUND);
			::GetDlgItemText(hwnd, IDC_CLAP_SOUNDPATH, servMgr->ppClapSoundPath.cstr(), String::MAX_LEN);

			servMgr->guiAntennaNotifyIcon = getButtonState(hwnd, IDC_ANTENNA_ICON);
			return TRUE;
		}
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDC_CLAP_SOUNDBROWSE:
			{
				OPENFILENAME dlgParam;
				String strPath;

				memset(&dlgParam, 0, sizeof(dlgParam));
				::GetDlgItemText(hwnd, IDC_CLAP_SOUNDPATH, strPath.cstr(), String::MAX_LEN);

				dlgParam.lStructSize = sizeof(dlgParam);
				dlgParam.hwndOwner = hwnd;
				dlgParam.lpstrFilter = "Waveファイル(*.wav)\0*.wav\0";
				dlgParam.lpstrFile = strPath.cstr();
				dlgParam.nMaxFile = String::MAX_LEN;
				dlgParam.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
				dlgParam.lpstrDefExt = "wav";

				if(::GetOpenFileName(&dlgParam))
					::SetDlgItemText(hwnd, IDC_CLAP_SOUNDPATH, strPath.cstr());
			}
			return TRUE;
		}
		break;
	}

	return FALSE;
}

INT_PTR GUIConfig::Show(HWND hWnd) //JP-MOD
{
	PROPSHEETPAGE page;
	PROPSHEETHEADER header;
	HPROPSHEETPAGE hPage[4];

	memset(&page, 0, sizeof(page));
	memset(&header, 0, sizeof(header));

	page.dwSize = sizeof(page);
	page.dwFlags = PSP_DEFAULT;
	page.hInstance = GetWindowInstance(hWnd);

	page.pszTemplate = MAKEINTRESOURCE(IDD_PROPPAGE_GENERAL);
	page.pfnDlgProc = (DLGPROC)PropGeneralProc;
	hPage[0] = ::CreatePropertySheetPage(&page);

	page.pszTemplate = MAKEINTRESOURCE(IDD_PROPPAGE_TITLE);
	page.pfnDlgProc = (DLGPROC)PropTitleProc;
	hPage[1] = ::CreatePropertySheetPage(&page);

	page.pszTemplate = MAKEINTRESOURCE(IDD_PROPPAGE_LAYOUT);
	page.pfnDlgProc = (DLGPROC)PropLayoutProc;
	hPage[2] = ::CreatePropertySheetPage(&page);

	page.pszTemplate = MAKEINTRESOURCE(IDD_PROPPAGE_EXPANSION);
	page.pfnDlgProc = (DLGPROC)PropExpansionProc;
	hPage[3] = ::CreatePropertySheetPage(&page);

	header.dwSize = sizeof(header);
	header.dwFlags = PSH_NOAPPLYNOW;
	header.hwndParent = hWnd;
	header.hInstance = page.hInstance;
	header.pszCaption = "GUI 設定";
	header.nPages = 4;
	header.phpage = hPage;

	return ::PropertySheet(&header);
}
