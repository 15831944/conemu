﻿
/*
Copyright (c) 2009-2014 Maximus5
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#undef HIDE_USE_EXCEPTION_INFO
#include "Header.h"
#include <Tlhelp32.h>
#include "ConEmu.h"
#include "Attach.h"
#include "Inside.h"
#include "OptionsClass.h"
#include "VConGroup.h"
#include "../common/Monitors.h"
#include "../common/WinObjects.h"
#include "../common/ProcList.h"
#include "../ConEmuCD/ExitCodes.h"
#include "../common/ConEmuCheck.h"

//#undef ALLOW_GUI_ATTACH
#define ALLOW_GUI_ATTACH

/*

IDD_ATTACHDLG DIALOGEX 0, 0, 317, 186
STYLE DS_SETFONT | DS_MODALFRAME | DS_FIXEDSYS | WS_POPUP | WS_CAPTION | WS_SYSMENU
EXSTYLE WS_EX_TOPMOST
CAPTION "Choose window or console application for attach"
FONT 8, "MS Shell Dlg", 0, 0, 0x1
BEGIN
    DEFPUSHBUTTON   "Attach",IDOK,205,165,50,14
    //PUSHBUTTON      "Cancel",IDCANCEL,260,165,50,14
    CONTROL         "",IDC_ATTACHLIST,"SysListView32",LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_ALIGNLEFT | WS_BORDER | WS_TABSTOP,7,7,303,152
    PUSHBUTTON      "&New",IDC_NEWCONSOLE,7,165,50,14
    PUSHBUTTON      "&Refresh",IDC_REFRESH,60,165,50,14
END

*/

enum AttachListColumns
{
	alc_PID = 0,
	alc_Type,
	alc_Title,
	alc_File, // executable name
	alc_Path, // executable full path
	alc_HWND,
	alc_Class,
};

const wchar_t szTypeCon[] = L"CON";
const wchar_t szTypeGui[] = L"GUI";

CAttachDlg::CAttachDlg()
	: mh_Dlg(NULL)
	, mh_List(NULL)
	, mn_AttachType(0)
	, mn_AttachPID(0)
	, mh_AttachHWND(NULL)
	, mp_ProcessData(NULL)
{
	mb_IsWin64 = IsWindows64();
}

CAttachDlg::~CAttachDlg()
{
	_ASSERTE(!mh_Dlg || !IsWindow(mh_Dlg));
	Close();
}

HWND CAttachDlg::GetHWND()
{
	if (!this)
	{
		_ASSERTE(this);
		return NULL;
	}
	return mh_Dlg;
}

// Открыть диалог со списком окон/процессов, к которым мы можем подцепиться
void CAttachDlg::AttachDlg()
{
	if (!this)
	{
		_ASSERTE(this);
		return;
	}

	if (mh_Dlg && IsWindow(mh_Dlg))
	{
		ShowWindow(mh_Dlg, SW_SHOWNORMAL);
		apiSetForegroundWindow(mh_Dlg);
		return;
	}
	
	bool bPrev = gpConEmu->SetSkipOnFocus(true);
	mh_Dlg = CreateDialogParam(g_hInstance, MAKEINTRESOURCE(IDD_ATTACHDLG), NULL, AttachDlgProc, (LPARAM)this);
	gpConEmu->SetSkipOnFocus(bPrev);

	//DWORD_PTR nAttachParm[3] = {};
	//int nRc = DialogBoxParam(nAttachParm);
	//switch (nRc)
	//{
	//case IDC_NEWCONSOLE:
	//	Recreate(FALSE, gpSet->isMultiNewConfirm);
	//	break;
	//case IDOK:
	//	// [0] Type: 1 - console, 2 - GUI
	//	// [1] PID
	//	// [2] HWND (для GUI)
	//	_ASSERTE((nAttachParm[0] == 1 || nAttachParm[0] == 2) && (nAttachParm[1]) && (nAttachParm[2] || nAttachParm[0] == 1));
	//	TODO("Ну и сам аттач, собственно");
	//	break;
	//}
}

void CAttachDlg::Close()
{
	if (mh_Dlg)
	{
		if (IsWindow(mh_Dlg))
			DestroyWindow(mh_Dlg);
		mh_Dlg = NULL;
	}
	
	if (mp_ProcessData)
	{
		_ASSERTE(mp_ProcessData==NULL);
		delete mp_ProcessData;
		mp_ProcessData = NULL;
	}

	gpConEmu->OnOurDialogClosed();
}

bool CAttachDlg::OnStartAttach()
{
	bool lbRc = false;
	// Тут нужно получить инфу из списка и дернуть собственно аттач
	wchar_t szItem[128] = {};
	//DWORD nPID = 0, nBits = WIN3264TEST(32,64);
	//AttachProcessType nType = apt_Unknown;
	wchar_t *psz;
	int iSel, iCur;
	DWORD nTID;
	HANDLE hThread = NULL;
	AttachParm *pParm = NULL;
	MArray<AttachParm> Parms;
	//HWND hAttachWnd = NULL;

	ShowWindow(mh_Dlg, SW_HIDE);

	BOOL bAlternativeMode = (IsDlgButtonChecked(mh_Dlg, IDC_ATTACH_ALT) != 0);

	iSel = ListView_GetNextItem(mh_List, -1, LVNI_SELECTED);
	while (iSel >= 0)
	{
		iCur = iSel;
		iSel = ListView_GetNextItem(mh_List, iCur, LVNI_SELECTED);

		AttachParm L = {NULL, 0, WIN3264TEST(32,64), apt_Unknown, bAlternativeMode};

		ListView_GetItemText(mh_List, iCur, alc_PID, szItem, countof(szItem)-1);
		L.nPID = wcstoul(szItem, &psz, 10);
		if (L.nPID)
		{
			psz = wcschr(szItem, L'*');
			if (psz)
				L.nBits = wcstoul(psz+1, &psz, 10);
		}
		ListView_GetItemText(mh_List, iCur, alc_Type, szItem, countof(szItem));
		if (lstrcmp(szItem, szTypeCon) == 0)
			L.nType = apt_Console;
		else if (lstrcmp(szItem, szTypeGui) == 0)
			L.nType = apt_Gui;

		ListView_GetItemText(mh_List, iCur, alc_HWND, szItem, countof(szItem));
		L.hAttachWnd = (szItem[0]==L'0' && szItem[1]==L'x') ? (HWND)wcstoul(szItem+2, &psz, 16) : NULL;

		if (!L.nPID || !L.nBits || !L.nType || !L.hAttachWnd)
		{
			MBoxAssert(L.nPID && L.nBits && L.nType && L.hAttachWnd);
			goto wrap;
		}

		Parms.push_back(L);
	}

	if (Parms.empty())
	{
		goto wrap;
	}
	else
	{
		AttachParm N = {NULL};
		Parms.push_back(N);
	}

	//// Чтобы клик от мышки в консоль не провалился
	//WARNING("Клик от мышки в консоль проваливается");
	//gpConEmu->mouse.nSkipEvents[0] = WM_LBUTTONUP;
	//gpConEmu->mouse.nSkipEvents[1] = 0;
	//gpConEmu->mouse.nReplaceDblClk = 0;

	// Все, диалог закрываем, чтобы не мешался
	Close();

	// Работу делаем в фоновом потоке, чтобы не блокировать главный
	// (к окну ConEmu должна подцепиться новая вкладка)
	pParm = Parms.detach();
	if (!pParm)
	{
		_wsprintf(szItem, SKIPLEN(countof(szItem)) L"ConEmu Attach, PID=%u, TID=%u", GetCurrentProcessId(), GetCurrentThreadId());
		DisplayLastError(L"Parms.detach() failed", -1, 0, szItem);
		goto wrap;
	}
	else
	{
		hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)StartAttachThread, pParm, 0, &nTID);
		if (!hThread)
		{
			DWORD dwErr = GetLastError();
			_wsprintf(szItem, SKIPLEN(countof(szItem)) L"ConEmu Attach, PID=%u, TID=%u", GetCurrentProcessId(), GetCurrentThreadId());
			DisplayLastError(L"Can't start attach thread", dwErr, 0, szItem);
		}
		else
			lbRc = true;
	}
wrap:
	// We don't need this handle
	if (hThread)
		CloseHandle(hThread);

	return lbRc;
}

BOOL CAttachDlg::AttachDlgEnumWin(HWND hFind, LPARAM lParam)
{
	if (IsWindowVisible(hFind))
	{
		CAttachDlg* pDlg = (CAttachDlg*)lParam;

		// Условия?
		DWORD_PTR nStyle = GetWindowLongPtr(hFind, GWL_STYLE);
		DWORD_PTR nStyleEx = GetWindowLongPtr(hFind, GWL_EXSTYLE);
		DWORD nPID;
		if (!GetWindowThreadProcessId(hFind, &nPID))
			nPID = 0;

		bool lbCan = ((nStyle & (WS_VISIBLE/*|WS_CAPTION|WS_MAXIMIZEBOX*/)) == (WS_VISIBLE/*|WS_CAPTION|WS_MAXIMIZEBOX*/));
		if (lbCan)
		{
			// Более тщательно стили проверить
			lbCan = ((nStyle & WS_MAXIMIZEBOX) == WS_MAXIMIZEBOX) || ((nStyle & WS_THICKFRAME) == WS_THICKFRAME);
		}
		if (lbCan && nPID == GetCurrentProcessId())
			lbCan = false;
		if (lbCan && nPID == pDlg->mn_ExplorerPID)
			lbCan = false;
		if (lbCan && (nStyle & WS_CHILD))
			lbCan = false;
		if (lbCan && (nStyleEx & WS_EX_TOOLWINDOW))
			lbCan = false;
		if (lbCan && gpConEmu->isOurConsoleWindow(hFind))
			lbCan = false;
		if (lbCan && gpConEmu->mp_Inside && (hFind == gpConEmu->mp_Inside->mh_InsideParentRoot))
			lbCan = false;

		wchar_t szClass[MAX_PATH], szTitle[MAX_PATH];
		GetClassName(hFind, szClass, countof(szClass));
		GetWindowText(hFind, szTitle, countof(szTitle));

		if (gpSetCls->isAdvLogging)
		{
			wchar_t szLogInfo[MAX_PATH*3];
			_wsprintf(szLogInfo, SKIPLEN(countof(szLogInfo)) L"Attach:%s x%08X/x%08X/x%08X {%s} \"%s\"", (DWORD)hFind, nStyle, nStyleEx, szClass, szTitle);
			CVConGroup::LogString(szLogInfo);
		}

		if (lbCan)
		{
			wchar_t szPid[32], szHwnd[32], szType[16]; szPid[0] = szHwnd[0] = szType[0] = 0;
			wchar_t szExeName[MAX_PATH], szExePathName[MAX_PATH*4]; szExeName[0] = szExePathName[0] = 0;

			#ifndef ALLOW_GUI_ATTACH
			if (!isConsoleClass(szClass))
				return TRUE;
			#endif

			HWND hList = pDlg->mh_List;
			if (nPID)
			{
				_wsprintf(szPid, SKIPLEN(countof(szPid)) L"%u", nPID);
			}
			else
			{
				#ifdef _DEBUG
					wcscpy_c(szPid, L"????");
				#else
					return TRUE;
				#endif
			}

			HANDLE h;
			bool lbExeFound = false;
			int nImageBits = 32;

			#if 0
			// Так можно получить только имя файла процесса, а интересен еще и путь
			PROCESSENTRY32 pi = {sizeof(pi)};
			h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (h && h != INVALID_HANDLE_VALUE)
			{
				if (Process32First(h, &pi))
				{
					while (pi.th32ProcessID != nPID)
					{
						if (!Process32Next(h, &pi))
							pi.th32ProcessID = 0;
					}
				}
			}
			#endif

			if (pDlg->mp_ProcessData)
			{
				lbExeFound = pDlg->mp_ProcessData->GetProcessName(nPID, szExeName, countof(szExeName), szExePathName, countof(szExePathName), &nImageBits);
				if (lbExeFound)
				{
					//ListView_SetItemText(hList, nItem, alc_File, szExeName);
					//ListView_SetItemText(hList, nItem, alc_Path, szExePathName);
					if (pDlg->mb_IsWin64)
					{
						wcscat_c(szPid, (nImageBits == 64) ? L" *64" : L" *32");
					}
				}
			}

			if (!lbExeFound)
			{
				h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, nPID);
				if (h && h != INVALID_HANDLE_VALUE)
				{
					MODULEENTRY32 mi = {sizeof(mi)};
					if (Module32First(h, &mi))
					{
						//ListView_SetItemText(hList, nItem, alc_File, *mi.szModule ? mi.szModule : (wchar_t*)PointToName(mi.szExePath));
						lstrcpyn(szExeName, *mi.szModule ? mi.szModule : (wchar_t*)PointToName(mi.szExePath), countof(szExeName));
						//ListView_SetItemText(hList, nItem, alc_Path, mi.szExePath);
						lstrcpyn(szExePathName, mi.szExePath, countof(szExePathName));
						if (pDlg->mb_IsWin64)
						{
							wcscat_c(szPid, WIN3264TEST(L" *32",L" *64"));
						}
						lbExeFound = true;
					}
					else
					{
						if (pDlg->mb_IsWin64)
						{
							wcscat_c(szPid, L" *64");
						}
					}
					CloseHandle(h);
				}
				
				#ifdef _WIN64
				if (!lbExeFound)
				{
					h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32, nPID);
					if (h && h != INVALID_HANDLE_VALUE)
					{
						MODULEENTRY32 mi = {sizeof(mi)};
						if (Module32First(h, &mi))
						{
							//ListView_SetItemText(hList, nItem, alc_File, *mi.szModule ? mi.szModule : (wchar_t*)PointToName(mi.szExePath));
							lstrcpyn(szExeName, *mi.szModule ? mi.szModule : (wchar_t*)PointToName(mi.szExePath), countof(szExeName));
							//ListView_SetItemText(hList, nItem, alc_Path, mi.szExePath);
							lstrcpyn(szExePathName, mi.szExePath, countof(szExePathName));
							wcscat_c(szPid, L" *32");
						}
						CloseHandle(h);
					}
				}
				#endif
			}

			LVITEM lvi = {LVIF_TEXT|LVIF_STATE};
			lvi.state = 0;
			lvi.stateMask = LVIS_SELECTED|LVIS_FOCUSED;
			lvi.pszText = szPid;
			lvi.iItem = ListView_GetItemCount(hList); // в конец
			int nItem = ListView_InsertItem(hList, &lvi);
			ListView_SetItemState(hList, nItem, 0, LVIS_SELECTED|LVIS_FOCUSED);

			ListView_SetItemText(hList, nItem, alc_Title, szTitle);
			ListView_SetItemText(hList, nItem, alc_Class, szClass);
			
			_wsprintf(szHwnd, SKIPLEN(countof(szHwnd)) L"0x%08X", (DWORD)(((DWORD_PTR)hFind) & (DWORD)-1));
			ListView_SetItemText(hList, nItem, alc_HWND, szHwnd);

			wcscpy_c(szType, isConsoleClass(szClass) ? szTypeCon : szTypeGui);
			ListView_SetItemText(hList, nItem, alc_File, szExeName);
			ListView_SetItemText(hList, nItem, alc_Path, szExePathName);
			ListView_SetItemText(hList, nItem, alc_Type, szType);
		}
	}
	return TRUE; // Продолжить
}

//RECT CAttachDlg::CenterInParent(RECT rcDlg, HWND hParent)
//{
//	RECT rcParent; GetWindowRect(ghWnd, &rcParent);
//
//	int nWidth  = (rcDlg.right - rcDlg.left);
//	int nHeight = (rcDlg.bottom - rcDlg.top);
//
//	MONITORINFO mi = {sizeof(mi)};
//	GetNearestMonitorInfo(&mi, NULL, &rcParent);
//
//	RECT rcCenter = {
//		max(mi.rcWork.left,(rcParent.left+rcParent.right-rcDlg.right+rcDlg.left)/2),
//		max(mi.rcWork.top,(rcParent.top+rcParent.bottom-rcDlg.bottom+rcDlg.top)/2)
//	};
//
//	if (((rcCenter.left + nWidth) > mi.rcWork.right)
//		&& (rcCenter.left > mi.rcWork.left))
//	{
//		rcCenter.left = max(mi.rcWork.left, (mi.rcWork.right - nWidth));
//	}
//
//	if (((rcCenter.top + nWidth) > mi.rcWork.bottom)
//		&& (rcCenter.top > mi.rcWork.top))
//	{
//		rcCenter.top = max(mi.rcWork.top, (mi.rcWork.bottom - nHeight));
//	}
//
//	rcCenter.right = rcCenter.left + nWidth;
//	rcCenter.bottom = rcCenter.top + nHeight;
//
//	return rcCenter;
//}

INT_PTR CAttachDlg::AttachDlgProc(HWND hDlg, UINT messg, WPARAM wParam, LPARAM lParam)
{
	CAttachDlg* pDlg = NULL;
	if (messg == WM_INITDIALOG)
	{
		pDlg = (CAttachDlg*)lParam;
		pDlg->mh_Dlg = hDlg;
		SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)lParam);
	}
	else
	{
		pDlg = (CAttachDlg*)GetWindowLongPtr(hDlg, DWLP_USER);
	}
	
	if (!pDlg)
	{
		//_ASSERTE(pDlg!=NULL);
		return 0;
	}
	_ASSERTE(pDlg->mh_Dlg!=NULL);

	PatchMsgBoxIcon(hDlg, messg, wParam, lParam);

	switch (messg)
	{
		case WM_INITDIALOG:
		{
			gpConEmu->OnOurDialogOpened();

			// Если ConEmu - AlwaysOnTop, то и диалогу нужно выставит этот флаг
			if (GetWindowLongPtr(ghWnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
				SetWindowPos(hDlg, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);

			SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hClassIcon);
			SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hClassIconSm);
			SetClassLongPtr(hDlg, GCLP_HICON, (LONG_PTR)hClassIcon);
			
			// В Windows 2000 отсуствует процедура AttachConsole необходимая для этого режима
			EnableWindow(GetDlgItem(hDlg, IDC_ATTACH_ALT), (gnOsVer >= 0x501));

			HWND hList = GetDlgItem(hDlg, IDC_ATTACHLIST);
			pDlg->mh_List = hList;
			{
				LVCOLUMN col ={LVCF_WIDTH|LVCF_TEXT|LVCF_FMT, LVCFMT_LEFT, 60};
				wchar_t szTitle[64]; col.pszText = szTitle;

				ListView_SetExtendedListViewStyleEx(hList,LVS_EX_FULLROWSELECT,LVS_EX_FULLROWSELECT);
				ListView_SetExtendedListViewStyleEx(hList,LVS_EX_LABELTIP|LVS_EX_INFOTIP,LVS_EX_LABELTIP|LVS_EX_INFOTIP);
				
				col.cx = pDlg->mb_IsWin64 ? 75 : 60;
				wcscpy_c(szTitle, L"PID");			ListView_InsertColumn(hList, alc_PID, &col);
				col.cx = 45; col.fmt = LVCFMT_CENTER;
				wcscpy_c(szTitle, L"Type");			ListView_InsertColumn(hList, alc_Type, &col);
				col.cx = 200; col.fmt = LVCFMT_LEFT;
				wcscpy_c(szTitle, L"Title");		ListView_InsertColumn(hList, alc_Title, &col);
				col.cx = 120;
				wcscpy_c(szTitle, L"Image name");	ListView_InsertColumn(hList, alc_File, &col);
				col.cx = 200;
				wcscpy_c(szTitle, L"Image path");	ListView_InsertColumn(hList, alc_Path, &col);
				col.cx = 90;
				wcscpy_c(szTitle, L"HWND");			ListView_InsertColumn(hList, alc_HWND, &col);
				col.cx = 200;
				wcscpy_c(szTitle, L"Class");		ListView_InsertColumn(hList, alc_Class, &col);
			}

			pDlg->mp_ProcessData = new CProcessData;

			CVConGroup::LogString(L"CAttachDlg::AttachDlgEnumWin::Begin", TRUE);

			HWND hTaskBar = FindWindowEx(NULL, NULL, L"Shell_TrayWnd", NULL);
			if (hTaskBar)
				GetWindowThreadProcessId(hTaskBar, &pDlg->mn_ExplorerPID);
			else
				pDlg->mn_ExplorerPID = 0;

			EnumWindows(AttachDlgEnumWin, (LPARAM)pDlg);

			CVConGroup::LogString(L"CAttachDlg::AttachDlgEnumWin::End", TRUE);

			delete pDlg->mp_ProcessData;
			pDlg->mp_ProcessData = NULL;

			AttachDlgProc(hDlg, WM_SIZE, 0, 0);

			RECT rect; GetWindowRect(hDlg, &rect);
			RECT rcCenter = CenterInParent(rect, ghWnd);
			MoveWindow(hDlg, rcCenter.left, rcCenter.top,
			           rect.right - rect.left, rect.bottom - rect.top, false);
			
			ShowWindow(hDlg, SW_SHOWNORMAL);

			SetFocus(hList);

			return FALSE;
		}

		case WM_SIZE:
		{
			RECT rcDlg, rcBtn, rcList, rcMode;
			::GetClientRect(hDlg, &rcDlg);
			HWND h = GetDlgItem(hDlg, IDC_ATTACHLIST);
			GetWindowRect(h, &rcList); MapWindowPoints(NULL, hDlg, (LPPOINT)&rcList, 2);
			HWND hb = GetDlgItem(hDlg, IDOK);
			GetWindowRect(hb, &rcBtn); MapWindowPoints(NULL, hb, (LPPOINT)&rcBtn, 2);
			HWND hc = GetDlgItem(hDlg, IDC_ATTACH_ALT);
			GetWindowRect(hc, &rcMode); MapWindowPoints(NULL, hc, (LPPOINT)&rcMode, 2);
			BOOL lbRedraw = FALSE;
			MoveWindow(h,
				rcList.left, rcList.top, rcDlg.right - 2*rcList.left, rcDlg.bottom - 3*rcList.top - rcBtn.bottom, lbRedraw);
			MoveWindow(GetDlgItem(hDlg, IDC_NEWCONSOLE),
				rcList.left, rcDlg.bottom - rcList.top - rcBtn.bottom, rcBtn.right, rcBtn.bottom, lbRedraw);
			MoveWindow(GetDlgItem(hDlg, IDC_REFRESH),
				rcList.left*2 + rcBtn.right, rcDlg.bottom - rcList.top - rcBtn.bottom, rcBtn.right, rcBtn.bottom, lbRedraw);
			MoveWindow(GetDlgItem(hDlg, IDCANCEL),
				rcDlg.right - rcList.left - rcBtn.right, rcDlg.bottom - rcList.top - rcBtn.bottom, rcBtn.right, rcBtn.bottom, lbRedraw);
			MoveWindow(GetDlgItem(hDlg, IDOK),
				rcDlg.right - 2*rcList.left - 2*rcBtn.right, rcDlg.bottom - rcList.top - rcBtn.bottom, rcBtn.right, rcBtn.bottom, lbRedraw);
			MoveWindow(hc,
				rcDlg.right - 3*rcList.left - 2*rcBtn.right - rcMode.right, rcDlg.bottom - rcList.top - rcBtn.bottom + ((rcBtn.bottom - rcMode.bottom) / 2), rcMode.right, rcMode.bottom, lbRedraw);
			RedrawWindow(hDlg, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
			break;
		}

		case WM_GETMINMAXINFO:
		{
			MINMAXINFO* p = (MINMAXINFO*)lParam;
			HWND h;
			RECT rcBtn = {}, rcList = {}, rcMode = {};
			GetWindowRect((h = GetDlgItem(hDlg, IDOK)), &rcBtn); MapWindowPoints(NULL, h, (LPPOINT)&rcBtn, 2);
			GetWindowRect((h = GetDlgItem(hDlg, IDC_ATTACH_ALT)), &rcMode); MapWindowPoints(NULL, h, (LPPOINT)&rcMode, 2);
			GetWindowRect((h = GetDlgItem(hDlg, IDC_ATTACHLIST)), &rcList); MapWindowPoints(NULL, hDlg, (LPPOINT)&rcList, 2);
			RECT rcWnd = {}, rcClient = {};
			GetWindowRect(hDlg, &rcWnd);
			::GetClientRect(hDlg, &rcClient);
			p->ptMinTrackSize.x = (rcWnd.right - rcWnd.left - rcClient.right) + 4*rcBtn.right + 8*rcList.left + rcMode.right;
			p->ptMinTrackSize.y = 6*rcBtn.bottom + 3*rcList.top;
			return 0;
		}

		case WM_NOTIFY:
		{
			LPNMITEMACTIVATE lpnmitem = (LPNMITEMACTIVATE)lParam;
			// По LMouseDblClick - выполнить аттач
			if (lpnmitem && (lpnmitem->hdr.idFrom == IDC_ATTACHLIST))
			{
				if (lpnmitem->hdr.code == (UINT)NM_DBLCLK)
				{
					PostMessage(hDlg, WM_COMMAND, IDOK, 0);

					// Если мышка над консолью - то клик (LBtnUp) может в нее провалиться, а не должен
					POINT ptCur; GetCursorPos(&ptCur);
					RECT rcWnd; GetWindowRect(ghWnd, &rcWnd);
					if (PtInRect(&rcWnd, ptCur))
					{
						gpConEmu->SetSkipMouseEvent(WM_MOUSEMOVE, WM_LBUTTONUP, 0);
					}
				}
			}
			break;
		}

		//case WM_TIMER:
		//	// Автообновление?
		//	break;

		case WM_CLOSE:
			pDlg->Close();
			break;
		case WM_DESTROY:
			break;
		
		case WM_COMMAND:
			if (HIWORD(wParam) == BN_CLICKED)
			{
				switch (LOWORD(wParam))
				{
					case IDOK:
						// Тут нужно получить инфу из списка и дернуть собственно аттач
						pDlg->OnStartAttach();
						return 1;
					case IDCANCEL:
					case IDCLOSE:
						// Просто закрыть
						pDlg->Close();
						return 1;
					case IDC_NEWCONSOLE:
						// Запрос на создание новой консоли
						PostMessage(ghWnd, WM_SYSCOMMAND, ID_NEWCONSOLE, 0);
						pDlg->Close();
						return 1;
					case IDC_REFRESH:
						// Перечитать список доступных окон/процессов
						{
							ListView_DeleteAllItems(pDlg->mh_List);
							pDlg->mp_ProcessData = new CProcessData;
							EnumWindows(AttachDlgEnumWin, (LPARAM)pDlg);
							delete pDlg->mp_ProcessData;
							pDlg->mp_ProcessData = NULL;
						}
						return 1;
				}
			}

			break;
		default:
			return 0;
	}

	return 0;
}

bool CAttachDlg::StartAttach(HWND ahAttachWnd, DWORD anPID, DWORD anBits, AttachProcessType anType, BOOL abAltMode)
{
	bool lbRc = false;
	// Тут нужно получить инфу из списка и дернуть собственно аттач
	wchar_t szPipe[MAX_PATH];
	PROCESS_INFORMATION pi = {};
	STARTUPINFO si = {sizeof(si)};
	SHELLEXECUTEINFO sei = {sizeof(sei)};
	CESERVER_REQ *pIn = NULL, *pOut = NULL;
	HANDLE hPipeTest = NULL, hProcTest = NULL;
	DWORD nErrCode = 0;
	bool lbCreate;
	CESERVER_CONSOLE_MAPPING_HDR srv;
	DWORD nWrapperWait = -1;
	DWORD nWrapperResult = -1;

	if (!ahAttachWnd || !anPID || !anBits || !anType)
	{
		MBoxAssert(ahAttachWnd && anPID && anBits && anType);
		goto wrap;
	}

	if (gpSetCls->isAdvLogging)
	{
		wchar_t szInfo[128];
		_wsprintf(szInfo, SKIPLEN(countof(szInfo))
			L"CAttachDlg::StartAttach HWND=x%08X, PID=%u, Bits%u, Type=%u, AltMode=%u",
			(DWORD)(DWORD_PTR)ahAttachWnd, anPID, anBits, (UINT)anType, abAltMode);
		gpConEmu->LogString(szInfo);
	}

	if (LoadSrvMapping(ahAttachWnd, srv))
	{
		pIn = ExecuteNewCmd(CECMD_ATTACH2GUI, sizeof(CESERVER_REQ_HDR));
		pOut = ExecuteSrvCmd(srv.nServerPID, pIn, ghWnd);
		if (pOut && (pOut->hdr.cbSize >= (sizeof(CESERVER_REQ_HDR)+sizeof(DWORD))) && (pOut->dwData[0] != 0))
		{
			lbRc = true; // Успешно подцепились
			goto wrap;
		}
		ExecuteFreeResult(pIn);
		ExecuteFreeResult(pOut);
	}


	// Может быть в процессе уже есть ConEmuHk.dll? Или этот процесс вообще уже во вкладке другого ConEmu?
	_wsprintf(szPipe, SKIPLEN(countof(szPipe)) CEHOOKSPIPENAME, L".", anPID);
	hPipeTest = CreateFile(szPipe, GENERIC_READ|GENERIC_WRITE, 0, LocalSecurity(), OPEN_EXISTING, 0, NULL);
	if (hPipeTest && hPipeTest != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hPipeTest);
		goto DoExecute;
	}


	wchar_t szSrv[MAX_PATH+64], szArgs[128];
	wcscpy_c(szSrv, gpConEmu->ms_ConEmuBaseDir);
	wcscat_c(szSrv, (anBits==64) ? L"\\ConEmuC64.exe" : L"\\ConEmuC.exe");

	if (abAltMode && (anType == apt_Console))
	{
		_wsprintf(szArgs, SKIPLEN(countof(szArgs)) L" /ATTACH /CONPID=%u /GID=%u /GHWND=%08X", anPID, GetCurrentProcessId(), (DWORD)ghWnd);
	}
	else
	{
		_wsprintf(szArgs, SKIPLEN(countof(szArgs)) L" /INJECT=%u", anPID);
		abAltMode = FALSE;
	}

	TODO("Определить, может он уже под админом? Тогда и ConEmuC.exe под админом запускать нужно");
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;

	if (anType == apt_Gui)
	{
		gpConEmu->CreateGuiAttachMapping(anPID);
	}

	hProcTest = OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ, FALSE, anPID);

	if (hProcTest == NULL)
	{
		nErrCode = GetLastError();
		MBoxAssert(hProcTest!=NULL || nErrCode==ERROR_ACCESS_DENIED);

		sei.hwnd = ghWnd;
		sei.fMask = (abAltMode ? 0 : SEE_MASK_NO_CONSOLE)|SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC;
		sei.lpVerb = L"runas";
		sei.lpFile = szSrv;
		sei.lpParameters = szArgs;
		sei.lpDirectory = gpConEmu->ms_ConEmuBaseDir;
		sei.nShow = SW_SHOWMINIMIZED;

		lbCreate = ShellExecuteEx(&sei);
		if (lbCreate)
		{
			MBoxAssert(sei.hProcess!=NULL);
			pi.hProcess = sei.hProcess;
		}
	}
	else
	{
		DWORD dwFlags = 0
			| (abAltMode ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE)
			| CREATE_DEFAULT_ERROR_MODE
			| NORMAL_PRIORITY_CLASS;
		lbCreate = CreateProcess(szSrv, szArgs, NULL, NULL, FALSE, dwFlags, NULL, NULL, &si, &pi);
	}

	if (!lbCreate)
	{
		wchar_t szErrMsg[MAX_PATH+255], szTitle[128];
		DWORD dwErr = GetLastError();
		_wsprintf(szErrMsg, SKIPLEN(countof(szErrMsg)) L"Can't start %s server\n%s %s", abAltMode ? L"injection" : L"console", szSrv, szArgs);
		_wsprintf(szTitle, SKIPLEN(countof(szTitle)) L"ConEmu Attach, PID=%u, TID=%u", GetCurrentProcessId(), GetCurrentThreadId());
		DisplayLastError(szErrMsg, dwErr, 0, szTitle);
		goto wrap;
	}

	if (abAltMode)
	{
		TODO("Подождать бы завершения процесса, или пока он подцепится к GUI");
		lbRc = true;
		goto wrap;
	}

	nWrapperWait = WaitForSingleObject(pi.hProcess, INFINITE);
	nWrapperResult = -1;
	GetExitCodeProcess(pi.hProcess, &nWrapperResult);
	CloseHandle(pi.hProcess);
	if (pi.hThread) CloseHandle(pi.hThread);
	if (((int)nWrapperResult != CERR_HOOKS_WAS_SET) && ((int)nWrapperResult != CERR_HOOKS_WAS_ALREADY_SET))
	{
		goto wrap;
	}


DoExecute:
	// Теперь можно дернуть созданный в удаленном процессе пайп для запуска в той консоли сервера.
	pIn = ExecuteNewCmd(CECMD_STARTSERVER, sizeof(CESERVER_REQ_HDR)+sizeof(CESERVER_REQ_START));
	pIn->NewServer.nGuiPID = GetCurrentProcessId();
	pIn->NewServer.hGuiWnd = ghWnd;
	if (anType == apt_Gui)
	{
		_ASSERTE(ahAttachWnd && IsWindow(ahAttachWnd));
		pIn->NewServer.hAppWnd = ahAttachWnd;
	}
	pOut = ExecuteCmd(szPipe, pIn, 500, ghWnd);
	if (!pOut || (pOut->hdr.cbSize < pIn->hdr.cbSize) || (pOut->dwData[0] == 0))
	{
		_ASSERTE(pOut && pOut->hdr.cbSize == (sizeof(CESERVER_REQ_HDR)+sizeof(CESERVER_REQ_START)));

		wchar_t szMsg[255], szTitle[128];
		wcscpy_c(szMsg, L"Failed to start console server in the remote process");
		if (hPipeTest && hPipeTest != INVALID_HANDLE_VALUE)
			wcscat_c(szMsg, L"\nHooks already was set");
		_wsprintf(szTitle, SKIPLEN(countof(szTitle)) L"ConEmu Attach, PID=%u, TID=%u", GetCurrentProcessId(), GetCurrentThreadId());
		DisplayLastError(szMsg, (pOut && (pOut->hdr.cbSize >= pIn->hdr.cbSize)) ? pOut->dwData[1] : -1, 0, szTitle);
		goto wrap;
	}


	lbRc = true;
wrap:
	UNREFERENCED_PARAMETER(nErrCode);
	UNREFERENCED_PARAMETER(nWrapperWait);
	ExecuteFreeResult(pIn);
	ExecuteFreeResult(pOut);
	return lbRc;
}

// Работу делаем в фоновом потоке, чтобы не блокировать главный
// (к окну ConEmu должна подцепиться новая вкладка)
DWORD CAttachDlg::StartAttachThread(AttachParm* lpParam)
{
	if (!lpParam)
	{
		_ASSERTE(lpParam!=NULL);
		return 100;
	}

	bool lbRc = true;
	
	for (AttachParm* p = lpParam; p->hAttachWnd; p++)
	{
		if (!StartAttach(p->hAttachWnd, p->nPID, p->nBits, p->nType, p->bAlternativeMode))
			lbRc = false;
	}

	free(lpParam);

	if (!lbRc)
		return 10;

	return 0;
}
