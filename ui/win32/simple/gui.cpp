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
#ifdef _DEBUG
#include "chkMemoryLeak.h"
#define DEBUG_NEW new(__FILE__, __LINE__)
#define new DEBUG_NEW
#endif

ThreadInfo guiThread;
bool shownChannels=false;

class ListData{
public:
	int channel_id;
	char name[21];
	int bitRate;
	int status;
	const char *statusStr;
	int totalListeners;
	int totalRelays;
	int localListeners;
	int localRelays;
	bool stayConnected;
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

ListData *list_top = NULL;
ServentData *servent_top = NULL;

// --------------------------------------------------
// for PCRaw (connection list) start
WNDPROC wndOldListBox = NULL, wndOldConnListBox = NULL;
bool sleep_skip = false;

LRESULT CALLBACK ListBoxProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch(message)
	{
		case WM_LBUTTONDOWN:
		{
			int index = SendMessage(hwnd, LB_ITEMFROMPOINT, 0, lParam);
			if(index >= 0x10000)
			{
				SendMessage(hwnd, LB_SETCURSEL, (DWORD)-1, 0L);
			}
			sleep_skip = true;
		}
			break;

		case WM_LBUTTONDBLCLK:
		{
			int index = SendMessage(hwnd, LB_ITEMFROMPOINT, 0, lParam);
			if(index < 0x10000)
			{
				SendMessage(guiWnd, WM_COMMAND, IDC_BUTTON8, NULL);
			}
		}
			break;

		case WM_RBUTTONDOWN:
		{
			POINT pos;
			MENUITEMINFO info, separator;
			HMENU hMenu;
			DWORD dwID;

			int index = SendMessage(hwnd, LB_ITEMFROMPOINT, 0, lParam);
			if(index < 0x10000)
			{
				SendMessage(hwnd, LB_SETCURSEL, (DWORD)index, 1L);
			}
			else
			{
				SendMessage(hwnd, LB_SETCURSEL, (DWORD)-1, 0L);
				sleep_skip = true;
				break;
			}
			
			hMenu = CreatePopupMenu();

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
			InsertMenuItem(hMenu, -1, true, &info);

			InsertMenuItem(hMenu, -1, true, &separator);

			info.wID = 1000;
			info.dwTypeData = "再生(&P)";
			InsertMenuItem(hMenu, -1, true, &info);

			InsertMenuItem(hMenu, -1, true, &separator);

			info.wID = 1002;
			info.dwTypeData = "再接続(&R)";
			InsertMenuItem(hMenu, -1, true, &info);

			info.wID = 1003;
			info.dwTypeData = "キープ(&K)";
			InsertMenuItem(hMenu, -1, true, &info);

			InsertMenuItem(hMenu, -1, true, &separator);

			info.wID = 2000;
			info.dwTypeData = "選択解除(&D)";
			InsertMenuItem(hMenu, -1, true, &info);

			GetCursorPos(&pos);
			dwID = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RETURNCMD, pos.x, pos.y, 0, hwnd, NULL);

			DestroyMenu(hMenu);

			switch(dwID)
			{
			case 1000:
				SendMessage(guiWnd, WM_COMMAND, IDC_BUTTON8, NULL);
				break;

			case 1001:
				SendMessage(guiWnd, WM_COMMAND, IDC_BUTTON5, NULL);
				break;

			case 1002:
				SendMessage(guiWnd, WM_COMMAND, IDC_BUTTON3, NULL);
				break;

			case 1003:
				SendMessage(guiWnd, WM_COMMAND, IDC_BUTTON9, NULL);
				break;

			case 2000:
				SendMessage(hwnd, LB_SETCURSEL, (DWORD)-1, 0L);
				sleep_skip = true;
				break;
			}

		}
			break;

		case WM_KEYDOWN:
			sleep_skip = true;
			break;
	}

	return CallWindowProc(wndOldListBox, hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ConnListBoxProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch(message)
	{
		case WM_LBUTTONDOWN:
		{
			int index = SendMessage(hwnd, LB_ITEMFROMPOINT, 0, lParam);
			if(index >= 0x10000)
			{
				SendMessage(hwnd, LB_SETCURSEL, (DWORD)-1, 0L);
			}
		}
			break;

		case WM_RBUTTONDOWN:
		{
			POINT pos;
			MENUITEMINFO info;
			HMENU hMenu;
			DWORD dwID;

			int index = SendMessage(hwnd, LB_ITEMFROMPOINT, 0, lParam);
			if(index < 0x10000)
			{
				SendMessage(hwnd, LB_SETCURSEL, (DWORD)index, 1L);
			}
			else
			{
				SendMessage(hwnd, LB_SETCURSEL, (DWORD)-1, 0L);
				break;
			}
			
			hMenu = CreatePopupMenu();

			memset(&info, 0, sizeof(MENUITEMINFO));
			info.cbSize = sizeof(MENUITEMINFO);
			info.fMask = MIIM_ID | MIIM_TYPE;
			info.fType = MFT_STRING;

			info.wID = 1001;
			info.dwTypeData = "切断(&X)";
			InsertMenuItem(hMenu, -1, true, &info);

			GetCursorPos(&pos);
			dwID = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RETURNCMD, pos.x, pos.y, 0, hwnd, NULL);

			DestroyMenu(hMenu);

			switch(dwID)
			{
			case 1001:
				SendMessage(guiWnd, WM_COMMAND, IDC_BUTTON6, NULL);
				break;
			}

		}
			break;
	}

	return CallWindowProc(wndOldConnListBox, hwnd, message, wParam, lParam);
}
// for PCRaw (connection list) end
// --------------------------------------------------
int logID,statusID,hitID,chanID;

// --------------------------------------------------
bool getButtonState(int id)
{
	return SendDlgItemMessage(guiWnd, id,BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// --------------------------------------------------
void enableControl(int id, bool on)
{
	EnableWindow(GetDlgItem(guiWnd,id),on);
}

// --------------------------------------------------
void enableEdit(int id, bool on)
{
	SendDlgItemMessage(guiWnd, id,WM_ENABLE, on, 0);
	SendDlgItemMessage(guiWnd, id,EM_SETREADONLY, !on, 0);
}
// --------------------------------------------------
int getEditInt(int id)
{
	char str[128];
	SendDlgItemMessage(guiWnd, id,WM_GETTEXT, 128, (LONG)str);
	return atoi(str);
}
// --------------------------------------------------
char * getEditStr(int id)
{
	static char str[128];
	SendDlgItemMessage(guiWnd, id,WM_GETTEXT, 128, (LONG)str);
	return str;
}
// --------------------------------------------------
void setEditStr(int id, char *str)
{
	SendDlgItemMessage(guiWnd, id,WM_SETTEXT, 0, (LONG)str);
}
// --------------------------------------------------
void setEditInt(int id, int v)
{
	char str[128];
	sprintf(str,"%d",v);
	SendDlgItemMessage(guiWnd, id,WM_SETTEXT, 0, (LONG)str);
}

// --------------------------------------------------
void *getListBoxSelData(int id)
{
	int sel = SendDlgItemMessage(guiWnd, id,LB_GETCURSEL, 0, 0);
	if (sel >= 0)
		return (void *)SendDlgItemMessage(guiWnd, id,LB_GETITEMDATA, sel, 0);
	return NULL;
}

Channel *getListBoxChannel(){
	int sel = SendDlgItemMessage(guiWnd, chanID ,LB_GETCURSEL, 0, 0);
	if (sel >= 0){
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

Servent *getListBoxServent(){
	int sel = SendDlgItemMessage(guiWnd, statusID ,LB_GETCURSEL, 0, 0);
	if (sel >= 0){
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
void setButtonState(int id, bool on)
{
	SendDlgItemMessage(guiWnd, id,BM_SETCHECK, on, 0);
	SendMessage(guiWnd,WM_COMMAND,id,0);
}
// --------------------------------------------------
void ADDLOG(const char *str,int id,bool sel,void *data, LogBuffer::TYPE type)
{
	if (guiWnd)
	{

		String sjis; //JP-EX
		int num = SendDlgItemMessage(guiWnd, id,LB_GETCOUNT, 0, 0);
		if (num > 100)
		{
			SendDlgItemMessage(guiWnd, id, LB_DELETESTRING, 0, 0);
			num--;
		}
		sjis = str; //JP-Patch
		sjis.convertTo(String::T_SJIS); //JP-Patch
		//int idx = SendDlgItemMessage(guiWnd, id, LB_ADDSTRING, 0, (LONG)(LPSTR)str);
		int idx = SendDlgItemMessage(guiWnd, id, LB_ADDSTRING, 0, (LONG)(LPSTR)sjis.cstr());
		SendDlgItemMessage(guiWnd, id, LB_SETITEMDATA, idx, (LONG)data);

		if (sel)
			SendDlgItemMessage(guiWnd, id, LB_SETCURSEL, num, 0);

	}

	if (type != LogBuffer::T_NONE)
	{
#if _DEBUG
		OutputDebugString(str);
		OutputDebugString("\n");
#endif
	}

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

WLock sd_lock;
WLock ld_lock;

// --------------------------------------------------
THREAD_PROC showConnections(ThreadInfo *thread)
{
//	thread->lock();
	while (thread->active)
	{
		int sel,top,i;
/*		sel = SendDlgItemMessage(guiWnd, statusID,LB_GETCURSEL, 0, 0);
		top = SendDlgItemMessage(guiWnd, statusID,LB_GETTOPINDEX, 0, 0);

		SendDlgItemMessage(guiWnd, statusID, LB_RESETCONTENT, 0, 0);
		Servent *s = servMgr->servents;
		while (s)
		{
			if (s->type != Servent::T_NONE)
			{
				Host h = s->getHost();
				{
					unsigned int ip = h.ip;
					unsigned short port = h.port;

					Host h(ip,port);
					char hostName[64];
					h.toStr(hostName);

					unsigned int tnum = 0;
					char tdef = 's';
					if (s->lastConnect)
						tnum = sys->getTime()-s->lastConnect;

					if ((s->type == Servent::T_RELAY) || (s->type == Servent::T_DIRECT))
					{
						ADDCONN(s,"%s-%s-%d%c  -  %s  -  %d - %s ",
							s->getTypeStr(),s->getStatusStr(),tnum,tdef,
							hostName,
							s->syncPos,s->agent.cstr()
							); //JP-Patch
					}else{
						if (s->status == Servent::S_CONNECTED)
						{
							ADDCONN(s,"%s-%s-%d%c  -  %s  -  %d/%d",
								s->getTypeStr(),s->getStatusStr(),tnum,tdef,
								hostName,
								s->gnuStream.packetsIn,s->gnuStream.packetsOut);
						}else{
							ADDCONN(s,"%s-%s-%d%c  -  %s",
								s->getTypeStr(),s->getStatusStr(),tnum,tdef,
								hostName
								);
						}

					}

				}
			}
			s=s->next;
		}
		if (sel >= 0)
			SendDlgItemMessage(guiWnd, statusID,LB_SETCURSEL, sel, 0);
		if (top >= 0)
			SendDlgItemMessage(guiWnd, statusID,LB_SETTOPINDEX, top, 0);*/

		int sd_count = 0;
		int diff = 0;

		sel = SendDlgItemMessage(guiWnd, statusID,LB_GETCURSEL, 0, 0);
		top = SendDlgItemMessage(guiWnd, statusID,LB_GETTOPINDEX, 0, 0);

		ServentData *sd = servent_top;
		while(sd){
			sd->flg = false;
			sd = sd->next;
			sd_count++;
		}

		servMgr->lock.on();
		Servent *s = servMgr->servents;

		Channel *sel_ch = getListBoxChannel();		// for PCRaw (connection list)

		while(s){
			Servent *next;
			bool foundFlg = false;
			ChanHitList *chl;
			bool infoFlg = false;
			bool relay = true;
			bool firewalled = false;
			unsigned int numRelays = 0;
			unsigned int totalRelays = 0;
			unsigned int totalListeners = 0;
			int vp_ver = 0;
			char ver_ex_prefix[2] = {' ', ' '};
			int ver_ex_number = 0;

			next = s->next;

			// for PCRaw (connection list) start
			if(sel_ch && !sel_ch->info.id.isSame(s->chanID))
			{
				s = next;
				continue;
			}
			// for PCRaw (connection list) end

			if (s->type != Servent::T_NONE){

				chanMgr->hitlistlock.on();
				
				chl = chanMgr->findHitListByID(s->chanID);
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
				chanMgr->hitlistlock.off();
			}

			sd = servent_top;
			while(sd){
				if (sd->servent_id == s->servent_id){
					foundFlg = true;
					if (s->thread.finish) break;
					sd->flg = true;
					sd->type = s->type;
					sd->status = s->status;
					sd->agent = s->agent;
					sd->h = s->getHost();
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

				int idx = SendDlgItemMessage(guiWnd, statusID, LB_ADDSTRING, 0, (LONG)"");
				SendDlgItemMessage(guiWnd, statusID, LB_SETITEMDATA, idx, (LONG)(newData->servent_id));
				diff++;
			}
			s = next;
		}
		servMgr->lock.off();

		sd_lock.on();
		sd = servent_top;
		int idx = 0;
		ServentData *prev = NULL;
		//int *idxs;
		//if (sd_count){
		//	idxs = new int[sd_count];
		//}
		while(sd){
			if (!sd->flg || (sd->type == Servent::T_NONE)){
				ServentData *next = sd->next;
				if (!prev){
					servent_top = next;
				} else {
					prev->next = next;
				}
				delete sd;

				PostMessage(GetDlgItem(guiWnd, statusID), LB_DELETESTRING, idx, 0);
//				SendDlgItemMessage(guiWnd, statusID, LB_DELETESTRING, idx, 0);
				sd = next;
//				diff--;
			} else {
				idx++;
				prev = sd;
				sd = sd->next;
			}
		}
		sd_lock.off();

		if ((sel >= 0) && (diff != 0)){
			PostMessage(GetDlgItem(guiWnd, statusID), LB_SETCURSEL, sel+diff, 0);
		}
		if (top >= 0){
			PostMessage(GetDlgItem(guiWnd, statusID), LB_SETTOPINDEX, top, 0);
		}
		InvalidateRect(GetDlgItem(guiWnd, statusID), NULL, FALSE);

		char cname[34];

		{
//			sel = SendDlgItemMessage(guiWnd, chanID,LB_GETCURSEL, 0, 0);
//			top = SendDlgItemMessage(guiWnd, chanID,LB_GETTOPINDEX, 0, 0);
//			SendDlgItemMessage(guiWnd, chanID, LB_RESETCONTENT, 0, 0);

			ListData *ld = list_top;
			while(ld){
				ld->flg = false;
				ld = ld->next;
			}

			Channel *c = chanMgr->channel;

			while (c)
			{
				Channel *next;
				bool foundFlg = false;
				String sjis;
				sjis = c->getName();
				sjis.convertTo(String::T_SJIS);
		
				next = c->next;

				ld = list_top;
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
						ld->localListeners = c->localListeners();
						ld->localRelays = c->localRelays();
						ld->stayConnected = c->stayConnected;
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
					newData->localListeners = c->localListeners();
					newData->localRelays = c->localRelays();
					newData->stayConnected = c->stayConnected;
					newData->chDisp = c->chDisp;
					newData->bTracker = c->sourceHost.tracker;

					int idx = SendDlgItemMessage(guiWnd, chanID, LB_ADDSTRING, 0, (LONG)"");
					SendDlgItemMessage(guiWnd, chanID, LB_SETITEMDATA, idx, (LONG)(newData->channel_id));
				}
				c = next;
			}

			ld = list_top;
			int idx = 0;
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

					SendDlgItemMessage(guiWnd, chanID, LB_DELETESTRING, idx, 0);
					ld = next;
				} else {
					idx++;
					prev = ld;
					ld = ld->next;
				}
			}

			InvalidateRect(GetDlgItem(guiWnd, chanID), NULL, FALSE);

/*					String sjis; //JP-Patch
					sjis = c->getName(); //JP-Patch
					sjis.convertTo(String::T_SJIS); //JP-Patch
					strncpy(cname,sjis.cstr(),16); //JP-Patch
					//strncpy(cname,c->getName(),16);
					cname[16] = 0;
					//int sec = ((c->currSPacket*c->bitrate*SPacket::DATA_LEN)/8)/(c->bitrate*1024);
					//int k = ((c->currSPacket*SPacket::DATA_LEN))/(1024);
					//ADDCHAN(c,"%d. %s - %d KB/s - %du - %dk",num,cname,c->bitrate,c->listeners,k);
					//ADDCHAN(c,"%d. %s - %d kb/s - %s",c->index,cname,c->getBitrate(),c->getStatusStr());
					ADDCHAN(c,"%s - %d kb/s - %s - %d/%d-[%d/%d] - %s",cname,c->getBitrate(),c->getStatusStr(),
							c->totalListeners(),c->totalRelays(),c->localListeners(),c->localRelays(),c->stayConnected?"Yes":"No"); //JP-Patch
				}
				c=c->next;
			}*/
//			if (sel >= 0)
//				SendDlgItemMessage(guiWnd, chanID,LB_SETCURSEL, sel, 0);
//			if (top >= 0)
//				SendDlgItemMessage(guiWnd, chanID,LB_SETTOPINDEX, top, 0);
		}



		bool update = ((sys->getTime() - chanMgr->lastHit) < 3)||(!shownChannels);

		if (update)
		{
			shownChannels = true;
			{
				sel = SendDlgItemMessage(guiWnd, hitID,LB_GETCURSEL, 0, 0);
				top = SendDlgItemMessage(guiWnd, hitID,LB_GETTOPINDEX, 0, 0);
				SendDlgItemMessage(guiWnd, hitID, LB_RESETCONTENT, 0, 0);

				chanMgr->hitlistlock.on();

				ChanHitList *chl = chanMgr->hitlist;

				while (chl)
				{
					if (chl->isUsed())
					{
						if (chl->info.match(chanMgr->searchInfo))
						{
							strncpy(cname,chl->info.name.cstr(),16);
							cname[16] = 0;
							ADDHIT(chl,"%s - %d kb/s - %d/%d",cname,chl->info.bitrate,chl->numListeners(),chl->numHits());
						}
					}
					chl = chl->next;
				}
				chanMgr->hitlistlock.off();
			}

			if (sel >= 0)
				SendDlgItemMessage(guiWnd, hitID,LB_SETCURSEL, sel, 0);
			if (top >= 0)
				SendDlgItemMessage(guiWnd, hitID,LB_SETTOPINDEX, top, 0);
		}




		{
			switch (servMgr->getFirewall())
			{
				case ServMgr::FW_ON:
					SendDlgItemMessage(guiWnd, IDC_EDIT4,WM_SETTEXT, 0, (LONG)"Firewalled");
					break;
				case ServMgr::FW_UNKNOWN:
					SendDlgItemMessage(guiWnd, IDC_EDIT4,WM_SETTEXT, 0, (LONG)"Unknown");
					break;
				case ServMgr::FW_OFF:
					SendDlgItemMessage(guiWnd, IDC_EDIT4,WM_SETTEXT, 0, (LONG)"Normal");
					break;
			}
		}

		// sleep for 1 second .. check every 1/10th for shutdown
		for(i=0; i<10; i++)
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
static void setControls(bool fromGUI)
{
	if (!guiWnd)
		return;
	setEditInt(IDC_EDIT1,servMgr->serverHost.port);
	setEditStr(IDC_EDIT3,servMgr->password);
	setEditStr(IDC_EDIT9,chanMgr->broadcastMsg.cstr());
	setEditInt(IDC_MAXRELAYS,servMgr->maxRelays);

	setButtonState(IDC_CHECK11,chanMgr->broadcastMsg[0]!=0);

	setButtonState(IDC_LOGDEBUG,(servMgr->showLog&(1<<LogBuffer::T_DEBUG))!=0);
	setButtonState(IDC_LOGERRORS,(servMgr->showLog&(1<<LogBuffer::T_ERROR))!=0);
	setButtonState(IDC_LOGNETWORK,(servMgr->showLog&(1<<LogBuffer::T_NETWORK))!=0);
	setButtonState(IDC_LOGCHANNELS,(servMgr->showLog&(1<<LogBuffer::T_CHANNEL))!=0);

	setButtonState(IDC_CHECK9,servMgr->pauseLog);


	if (!fromGUI)
		setButtonState(IDC_CHECK1,servMgr->autoServe);


}
// --------------------------------------------------
void APICALL MyPeercastApp::updateSettings()
{
	setControls(true);
}

void MoveControl(HWND hWnd, int cx, int cy, int y, HDWP& hDwp){
	RECT rc2;
	POINT pos;

	GetWindowRect(hWnd, &rc2);
	pos.x = rc2.left;
	pos.y = rc2.top;
	ScreenToClient(guiWnd, &pos);
	hDwp = DeferWindowPos(hDwp, hWnd, HWND_TOP,
		pos.x,cy-y-(rc2.bottom-rc2.top),rc2.right-rc2.left,rc2.bottom-rc2.top,SWP_SHOWWINDOW);
}

void MoveControl2(HWND hWnd, int cx, int cy, int y, HDWP& hDwp){
	RECT rc;
	POINT pos;
	
	GetWindowRect(hWnd, &rc);
	pos.x = rc.left;
	pos.y = rc.top;
	ScreenToClient(guiWnd, &pos);
	hDwp = DeferWindowPos(hDwp, hWnd, HWND_TOP,
		pos.x,pos.y,rc.right-rc.left,cy-y,SWP_SHOWWINDOW);
}

// --------------------------------------------------
void MoveControls(LPARAM lParam){
    HDWP hDwp;
			//IDC_LIST1 3,291,291,43
/*        CONTROL         "有効",IDC_CHECK1,"Button",BS_AUTOCHECKBOX | BS_PUSHLIKE | 
                    WS_TABSTOP,9,29,60,20,WS_EX_TRANSPARENT
    EDITTEXT        IDC_EDIT1,127,18,47,12,ES_AUTOHSCROLL
    RTEXT           "ポート :",IDC_STATIC,107,20,18,8
    GROUPBOX        "",IDC_STATIC,3,4,291,49
    PUSHBUTTON      "切断",IDC_BUTTON5,67,65,43,13
    EDITTEXT        IDC_EDIT3,127,34,47,12,ES_PASSWORD | ES_AUTOHSCROLL
    RTEXT           "パスワード :",IDC_STATIC,89,36,36,8
    PUSHBUTTON      "再生",IDC_BUTTON8,10,65,22,13
    PUSHBUTTON      "再接続",IDC_BUTTON3,41,65,24,13
    RTEXT           "最大リレー数 :",IDC_STATIC,203,20,40,8
    EDITTEXT        IDC_MAXRELAYS,248,18,40,14,ES_AUTOHSCROLL | ES_NUMBER
    PUSHBUTTON      "キープ",IDC_BUTTON9,112,65,24,13
    LTEXT           "Peercast-VP",IDC_STATIC,21,14,39,8*/

/*
    GROUPBOX        "リレー",IDC_GROUPBOX_RELAY,3,54,291,132
    LISTBOX         IDC_LIST3,3,81,291,102,LBS_OWNERDRAWFIXED | 
                    LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP
    CONTROL         "DJ",IDC_CHECK11,"Button",BS_AUTOCHECKBOX | BS_PUSHLIKE | 
                    WS_TABSTOP,5,190,23,12
    EDITTEXT        IDC_EDIT9,33,189,261,14,ES_AUTOHSCROLL
    LTEXT           "コネクション",IDC_STATIC_CONNECTION,3,214,40,8
    PUSHBUTTON      "切断",IDC_BUTTON6,47,209,43,13
    LISTBOX         IDC_LIST2,3,224,291,53,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | 
                    WS_TABSTOP
    LTEXT           "ログ",IDC_STATIC_LOG,3,282,13,8
    PUSHBUTTON      "クリア",IDC_BUTTON1,35,279,25,11
    CONTROL         "停止",IDC_CHECK9,"Button",BS_AUTOCHECKBOX | BS_PUSHLIKE | 
                    WS_TABSTOP,60,279,30,11
    CONTROL         "デバッグ",IDC_LOGDEBUG,"Button",BS_AUTOCHECKBOX | 
                    BS_PUSHLIKE | WS_TABSTOP,127,279,32,11
    CONTROL         "エラー",IDC_LOGERRORS,"Button",BS_AUTOCHECKBOX | 
                    BS_PUSHLIKE | WS_TABSTOP,159,279,25,11
    CONTROL         "ネットワーク",IDC_LOGNETWORK,"Button",BS_AUTOCHECKBOX | 
                    BS_PUSHLIKE | WS_TABSTOP,185,279,35,11
    CONTROL         "チャンネル",IDC_LOGCHANNELS,"Button",BS_AUTOCHECKBOX | 
                    BS_PUSHLIKE | WS_TABSTOP,221,279,35,11
	LISTBOX         IDC_LIST1,3,291,291,43,LBS_NOINTEGRALHEIGHT | WS_VSCROLL | 
                    WS_TABSTOP*/
	
	hDwp = BeginDeferWindowPos(15);
	int cx = LOWORD(lParam);
	int cy = HIWORD(lParam);
	
	MoveControl2(GetDlgItem(guiWnd, IDC_GROUPBOX_RELAY), cx, cy, 343, hDwp);
	MoveControl2(GetDlgItem(guiWnd, IDC_LIST3), cx, cy, 385, hDwp);
	
	MoveControl(GetDlgItem(guiWnd, IDC_EDIT9), cx, cy, 233, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_CHECK11), cx, cy, 232, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_STATIC_CONNECTION), cx, cy, 209, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_BUTTON6), cx, cy, 208, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_LIST2), cx, cy, 94, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_STATIC_LOG), cx, cy, 72, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_CHECK9), cx, cy, 71, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_BUTTON1), cx, cy, 71, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_LOGDEBUG), cx, cy, 71, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_LOGNETWORK), cx, cy, 71, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_LOGERRORS), cx, cy, 71, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_LOGCHANNELS), cx, cy, 71, hDwp);
	MoveControl(GetDlgItem(guiWnd, IDC_LIST1), cx, cy, 3, hDwp);

	EndDeferWindowPos(hDwp);
}

WINDOWPLACEMENT winPlace;
bool guiFlg = false;

// --------------------------------------------------
LRESULT CALLBACK GUIProc (HWND hwnd, UINT message,
                                 WPARAM wParam, LPARAM lParam)
{

	static const struct 
		{
			LRESULT from; // 変換元
			LRESULT to;   // 変換先
		}
	trans[] = 
		{
			{ HTLEFT,        HTBORDER }, // 左端の左右矢印 → 矢印なし
			{ HTRIGHT,       HTBORDER }, // 右端の左右矢印 → 矢印なし
			{ HTTOPLEFT,     HTTOP    }, // 左上隅の斜め矢印 → 縦矢印
			{ HTTOPRIGHT,    HTTOP    }, // 右上隅の斜め矢印 → 縦矢印
			{ HTBOTTOMLEFT,  HTBOTTOM }, // 左下隅の斜め矢印 → 縦矢印
			{ HTBOTTOMRIGHT, HTBOTTOM }, // 右下隅の斜め矢印 → 縦矢印
			{ HTTOP,         HTTOP    },
			{ HTBOTTOM,      HTBOTTOM }
		};


	bool rectflg = false;

   switch (message)
   {
       case WM_INITDIALOG:
			guiWnd = hwnd;

			shownChannels = false;
			logID = IDC_LIST1;		// log
			statusID = IDC_LIST2;	// status
			hitID = IDC_LIST4;		// hit
			chanID = IDC_LIST3;		// channels

			enableControl(IDC_BUTTON8,false);
			enableControl(IDC_BUTTON11,false);
			enableControl(IDC_BUTTON10,false);
			
			peercastApp->updateSettings();

			if (servMgr->autoServe)
				setButtonState(IDC_CHECK1,true);
			if (servMgr->autoConnect)
				setButtonState(IDC_CHECK2,true);


			guiThread.func = showConnections;
			if (!sys->startThread(&guiThread))
			{
				MessageBox(hwnd,"Unable to start GUI","PeerCast",MB_OK|MB_ICONERROR);
				PostMessage(hwnd,WM_DESTROY,0,0);
			}

			if (guiFlg){
				SetWindowPlacement(hwnd, &winPlace);
			}

			{	// for PCRaw (connection list)
				HWND hwndList;
				
				hwndList = GetDlgItem(guiWnd, chanID);
				wndOldListBox = (WNDPROC)GetWindowLong(hwndList, GWL_WNDPROC);
				SetWindowLong(hwndList, GWL_WNDPROC, (DWORD)ListBoxProc);

				hwndList = GetDlgItem(guiWnd, statusID);
				wndOldConnListBox = (WNDPROC)GetWindowLong(hwndList, GWL_WNDPROC);
				SetWindowLong(hwndList, GWL_WNDPROC, (DWORD)ConnListBoxProc);
			}

			break;

	  case WM_COMMAND:
			switch( wParam )
			{
				case IDC_CHECK1:		// start server
						if (getButtonState(IDC_CHECK1))
						{
							//SendDlgItemMessage(hwnd, IDC_CHECK1,WM_SETTEXT, 0, (LPARAM)"Deactivate");

							SendDlgItemMessage(hwnd, IDC_EDIT3,WM_GETTEXT, 64, (LONG)servMgr->password);

							servMgr->serverHost.port = (unsigned short)getEditInt(IDC_EDIT1);
							servMgr->setMaxRelays(getEditInt(IDC_MAXRELAYS));


							enableControl(IDC_EDIT1,false);
							enableControl(IDC_EDIT3,false);
							enableControl(IDC_MAXRELAYS,false);
							enableControl(IDC_BUTTON8,true);
							enableControl(IDC_BUTTON11,true);
							enableControl(IDC_BUTTON10,true);

							//writeSettings();
							servMgr->autoServe = true;

							setEditStr(IDC_CHECK1,"Enabled");


						}else{
							//SendDlgItemMessage(hwnd, IDC_CHECK1,WM_SETTEXT, 0, (LPARAM)"Activate");

							servMgr->autoServe = false;

							enableControl(IDC_EDIT1,true);
							enableControl(IDC_EDIT3,true);
							enableControl(IDC_MAXRELAYS,true);
							enableControl(IDC_BUTTON8,false);
							enableControl(IDC_BUTTON11,false);
							enableControl(IDC_BUTTON10,false);

							setEditStr(IDC_CHECK1,"Disabled");

						}
						setControls(true);

					break;
				case IDC_CHECK11:		// DJ message
					if (getButtonState(IDC_CHECK11))
					{
						enableControl(IDC_EDIT9,false);
						SendDlgItemMessage(hwnd, IDC_EDIT9,WM_GETTEXT, 128, (LONG)chanMgr->broadcastMsg.cstr());
					}else{
						enableControl(IDC_EDIT9,true);
						chanMgr->broadcastMsg.clear();
					}
					break;
				case IDC_LOGDEBUG:		// log debug
					servMgr->showLog = getButtonState(wParam) ? servMgr->showLog|(1<<LogBuffer::T_DEBUG) : servMgr->showLog&~(1<<LogBuffer::T_DEBUG);
					break;
				case IDC_LOGERRORS:		// log errors
					servMgr->showLog = getButtonState(wParam) ? servMgr->showLog|(1<<LogBuffer::T_ERROR) : servMgr->showLog&~(1<<LogBuffer::T_ERROR);
					break;
				case IDC_LOGNETWORK:		// log network
					servMgr->showLog = getButtonState(wParam) ? servMgr->showLog|(1<<LogBuffer::T_NETWORK) : servMgr->showLog&~(1<<LogBuffer::T_NETWORK);
					break;
				case IDC_LOGCHANNELS:		// log channels
					servMgr->showLog = getButtonState(wParam) ? servMgr->showLog|(1<<LogBuffer::T_CHANNEL) : servMgr->showLog&~(1<<LogBuffer::T_CHANNEL);
					break;
				case IDC_CHECK9:		// pause log
					servMgr->pauseLog = getButtonState(wParam);
					break;
				case IDC_CHECK2:		// start outgoing

					if (getButtonState(IDC_CHECK2))
					{

						SendDlgItemMessage(hwnd, IDC_COMBO1,WM_GETTEXT, 128, (LONG)servMgr->connectHost);
						servMgr->autoConnect = true;
						//SendDlgItemMessage(hwnd, IDC_CHECK2,WM_SETTEXT, 0, (LPARAM)"Disconnect");
						enableControl(IDC_COMBO1,false);
					}else{
						servMgr->autoConnect = false;
						//SendDlgItemMessage(hwnd, IDC_CHECK2,WM_SETTEXT, 0, (LPARAM)"Connect");
						enableControl(IDC_COMBO1,true);
					}
					break;
				case IDC_BUTTON11:		// broadcast
					{
						Host sh = servMgr->serverHost;
						if (sh.isValid())
						{
							char cmd[256];
							sprintf(cmd,"http://localhost:%d/admin?page=broadcast",sh.port);
							ShellExecute(hwnd, NULL, cmd, NULL, NULL, SW_SHOWNORMAL);
		
						}else{
							MessageBox(hwnd,"Server is not currently connected.\nPlease wait until you have a connection.","PeerCast",MB_OK);
						}
					}
					break;
				case IDC_BUTTON8:		// play selected
					{
						Channel *c = getListBoxChannel();
						if (c){
							chanMgr->playChannel(c->info);
						}
					}
					break;
				case IDC_BUTTON7:		// advanced
					sys->callLocalURL("admin?page=settings",servMgr->serverHost.port);
					break;
				case IDC_BUTTON6:		// servent disconnect
					{
/*						Servent *s = (Servent *)getListBoxSelData(statusID);
						if (s)
							s->thread.active = false;*/
						Servent *s = getListBoxServent();
						if (s){
							s->thread.active = false;
							s->thread.finish = true;
						}
						sleep_skip = true;
					}
					break;
				case IDC_BUTTON5:		// chan disconnect
					{
/*						Channel *c = (Channel *)getListBoxSelData(chanID);
						if (c)
							c->thread.active = false;*/

//						Channel *c = chanMgr->findChannelByChannelID((int)getListBoxSelData(chanID));
						Channel *c = getListBoxChannel();
						if (c){
							c->thread.active = false;
							c->thread.finish = true;
						}
						sleep_skip = true;
					}
					break;
				case IDC_BUTTON3:		// chan bump
					{
/*						Channel *c = (Channel *)getListBoxSelData(chanID);
						if (c)
							c->bump = true;*/

//						Channel *c = chanMgr->findChannelByChannelID((int)getListBoxSelData(chanID));
						Channel *c = getListBoxChannel();
						if (c){
							c->bump = true;
						}
					}

					break;
				case IDC_BUTTON4:		// get channel 
					{
						ChanHitList *chl = (ChanHitList *)getListBoxSelData(hitID);
						if (chl)
						{
							if (!chanMgr->findChannelByID(chl->info.id))
							{
								Channel *c = chanMgr->createChannel(chl->info,NULL);
								if (c)
									c->startGet();
							}
						}else{
							MessageBox(hwnd,"Please select a channel","PeerCast",MB_OK);
						}
					}
					break;

					case IDC_BUTTON9:		//JP-EX chan keep
					{
/*						Channel *c = (Channel *)getListBoxSelData(chanID);
						if (c)
						{
							if (!c->stayConnected)
							{
								//if (servMgr->getFirewall() == ServMgr::FW_OFF)
								c->stayConnected = true;
							}	
							else
								c->stayConnected = false;
						}*/

//						Channel *c = chanMgr->findChannelByChannelID((int)getListBoxSelData(chanID));
						Channel *c = getListBoxChannel();
						if (c){
							if (!c->stayConnected){
								c->stayConnected = true;
							} else {
								c->stayConnected = false;
							}
						}
					}
					break;

				case IDC_BUTTON1:		// clear log
					SendDlgItemMessage(guiWnd, logID, LB_RESETCONTENT, 0, 0);
					sys->logBuf->clear();	// for PCRaw (clear log)
					break;

				case IDC_BUTTON2:		// find
					{
						char str[64];
						SendDlgItemMessage(hwnd, IDC_EDIT2,WM_GETTEXT, 64, (LONG)str);
						SendDlgItemMessage(hwnd, hitID, LB_RESETCONTENT, 0, 0);
						ChanInfo info;
						info.init();
						info.name.set(str);
						chanMgr->startSearch(info);
					}
					break;

			}
			break;

		case WM_MEASUREITEM:
			if ((UINT) wParam==IDC_LIST3){
				LPMEASUREITEMSTRUCT lpMI = (LPMEASUREITEMSTRUCT)lParam;
				lpMI->itemHeight = 12;
			} else if ((UINT) wParam==IDC_LIST2){
				LPMEASUREITEMSTRUCT lpMI = (LPMEASUREITEMSTRUCT)lParam;
				lpMI->itemHeight = 12;
			}
			break;
		case WM_DRAWITEM:
			if  ((UINT) wParam==IDC_LIST3)
			{
				LPDRAWITEMSTRUCT _DrawItem=(LPDRAWITEMSTRUCT)lParam;
				HBRUSH   hBrush;
				ListData *ld;
				unsigned int idx = 0;
				bool flg = false;

				ld = list_top;
				while(ld){
					if (_DrawItem->itemID == idx){
						flg = true;
						break;
					}
					ld = ld->next;
					idx++;
				}
				if ((_DrawItem->itemState) & (ODS_SELECTED))
				{
					hBrush=CreateSolidBrush(RGB(49,106,197));
				}
				else  
				{
					hBrush=CreateSolidBrush(RGB(255,255,255));
				}
				FillRect(_DrawItem->hDC,&_DrawItem->rcItem,hBrush);                   
				DeleteObject(hBrush);
				if (flg){
					char buf[256];
					if (ld->status == Channel::S_RECEIVING){
						if (ld->chDisp.status == Channel::S_RECEIVING){
							if (ld->chDisp.relay){
								/* relay ok */
								SetTextColor(_DrawItem->hDC,RGB(0,255,0));
								SetBkColor(_DrawItem->hDC,RGB(255,255,255)) ;
							} else {
								/* no more relay */
								if (ld->chDisp.numRelays){
									/* relay full */
									SetTextColor(_DrawItem->hDC,RGB(0,0,255));
									SetBkColor(_DrawItem->hDC,RGB(255,255,255)) ;
								} else {
									/* relay ng */
									SetTextColor(_DrawItem->hDC,RGB(255,0,255));
									SetBkColor(_DrawItem->hDC,RGB(255,255,255)) ;
								}
							}
						} else {
							/* status unmatch */
							SetTextColor(_DrawItem->hDC,RGB(0,0,0));
							SetBkColor(_DrawItem->hDC,RGB(255,255,255)) ;
						}
						TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left,
							_DrawItem->rcItem.top,
							"■",
							2);
					} else {
						SetTextColor(_DrawItem->hDC,RGB(0,0,0));
						SetBkColor(_DrawItem->hDC,RGB(255,255,255)) ;
						TextOut(_DrawItem->hDC,
								_DrawItem->rcItem.left,
								_DrawItem->rcItem.top,
								"□",
								2);
					}
					if ((_DrawItem->itemState) & (ODS_SELECTED))
					{
						SetTextColor(_DrawItem->hDC,RGB(255,255,255));
						SetBkColor(_DrawItem->hDC,RGB(49,106,197)) ;
					} else {
						SetTextColor(_DrawItem->hDC,RGB(0,0,0));
						SetBkColor(_DrawItem->hDC,RGB(255,255,255)) ;
					}
					if (servMgr->getFirewall() == ServMgr::FW_ON){
						SetTextColor(_DrawItem->hDC,RGB(255,0,0));
					} else if (ld->bTracker && (ld->status == Channel::S_RECEIVING)){
						if ((_DrawItem->itemState) & (ODS_SELECTED))
						{
							SetTextColor(_DrawItem->hDC,RGB(0,255,0));
							SetBkColor(_DrawItem->hDC,RGB(49,106,197)) ;
						} else {
							SetTextColor(_DrawItem->hDC,RGB(0,128,0));
						}
					}

					TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left + 12,
							_DrawItem->rcItem.top,
							ld->name,
							strlen(ld->name));
/*					sprintf(buf, "- %4dkbps -", ld->bitRate);
					TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left + 12 + 118,
							_DrawItem->rcItem.top,
							buf,
							strlen(buf));
					TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left + 12 + 118 + 80,
							_DrawItem->rcItem.top,
							ld->statusStr,
							strlen(ld->statusStr));
					sprintf(buf, "- %3d/%3d - [%3d/%3d] -", ld->totalListeners, ld->totalRelays, ld->localListeners, ld->localRelays);
					TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left + 12 + 118 + 80 + 80,
							_DrawItem->rcItem.top,
							buf,
							strlen(buf));
					strcpy(buf, ld->stayConnected?"YES":"NO");
					TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left + 12 + 118 + 80 + 80 + 130,
							_DrawItem->rcItem.top,
							buf,
							strlen(buf));*/
					sprintf(buf, "- %4dkbps - %s - %3d/%3d - [%3d/%3d] - %s",
						ld->bitRate,
						ld->statusStr,
						ld->totalListeners,
						ld->totalRelays,
						ld->localListeners,
						ld->localRelays,
						ld->stayConnected?"YES":"NO");
					TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left + 12 + 118,
							_DrawItem->rcItem.top,
							buf,
							strlen(buf));
				}
			} else if  ((UINT) wParam==IDC_LIST2) {
				LPDRAWITEMSTRUCT _DrawItem=(LPDRAWITEMSTRUCT)lParam;
				HBRUSH   hBrush;
				ServentData *sd;
				unsigned int idx = 0;
				bool flg = false;

				sd_lock.on();

				sd = servent_top;
				while(sd){
					if (_DrawItem->itemID == idx){
						flg = true;
						break;
					}
					sd = sd->next;
					idx++;
				}
				if (flg){
					char buf[256];
					char hostName[64];
					sd->h.toStr(hostName);

					if ((_DrawItem->itemState) & (ODS_SELECTED))
					{
						hBrush=CreateSolidBrush(RGB(49,106,197));
					}
					else  
					{
						hBrush=CreateSolidBrush(RGB(255,255,255));
					}
					FillRect(_DrawItem->hDC,&_DrawItem->rcItem,hBrush);                   
					DeleteObject(hBrush);

					if (sd->infoFlg){
						if (sd->relay){
							/* relay ok */
							SetTextColor(_DrawItem->hDC,RGB(0,255,0));
//							SetBkColor(_DrawItem->hDC,RGB(255,255,255));
						} else {
							/* no more relay */
							if (sd->numRelays){
								/* relay full */
								SetTextColor(_DrawItem->hDC,RGB(0,0,255));
//								SetBkColor(_DrawItem->hDC,RGB(255,255,255));
							} else {
								/* relay ng */
								SetTextColor(_DrawItem->hDC,RGB(255,0,255));
//								SetBkColor(_DrawItem->hDC,RGB(255,255,255));
							}
						}
					} else {
						/* no info */
						SetTextColor(_DrawItem->hDC,RGB(0,0,0));
//						SetBkColor(_DrawItem->hDC,RGB(255,255,255));
					}

					if (sd->lastSkipTime + 120 > sys->getTime()){
						SetBkColor(_DrawItem->hDC,RGB(128,128,128));
						if (sd->type == Servent::T_RELAY){
							sprintf(buf, "▼(%d)",sd->lastSkipCount);
							TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left,
							_DrawItem->rcItem.top,
							buf,
							strlen(buf));
						} else {
							SetTextColor(_DrawItem->hDC,RGB(0,0,0));
							sprintf(buf, "▽(%d)",sd->lastSkipCount);
							TextOut(_DrawItem->hDC,
									_DrawItem->rcItem.left,
									_DrawItem->rcItem.top,
									buf,
									strlen(buf));
						}
					} else {
						SetBkColor(_DrawItem->hDC,RGB(255,255,255));
						if (sd->type == Servent::T_RELAY){
							TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left,
							_DrawItem->rcItem.top,
							"■",
							2);
						} else {
							SetTextColor(_DrawItem->hDC,RGB(0,0,0));
							SetBkColor(_DrawItem->hDC,RGB(255,255,255)) ;
							TextOut(_DrawItem->hDC,
									_DrawItem->rcItem.left,
									_DrawItem->rcItem.top,
									"□",
									2);
						}
					}

					if ((_DrawItem->itemState) & (ODS_SELECTED))
					{
						SetTextColor(_DrawItem->hDC,RGB(255,255,255));
						SetBkColor(_DrawItem->hDC,RGB(49,106,197)) ;
					} else {
						SetTextColor(_DrawItem->hDC,RGB(0,0,0));
						SetBkColor(_DrawItem->hDC,RGB(255,255,255)) ;
					}
					if (sd->firewalled){
						if (!sd->numRelays){
							SetTextColor(_DrawItem->hDC,RGB(255,0,0));
						} else {
							SetTextColor(_DrawItem->hDC,RGB(255,168,0));
						}
					}

					char buf2[16];
					if (sd->ver_ex_number){
						sprintf(buf2, "(%c%c%04d)", sd->ver_ex_prefix[0], sd->ver_ex_prefix[1], sd->ver_ex_number);
					} else if (sd->vp_ver){
						sprintf(buf2, "(VP%04d)", sd->vp_ver);
					} else {
						buf2[0] = '\0';
					}
					if (sd->type == Servent::T_RELAY){
						if (sd->status == Servent::S_CONNECTED){
							sprintf(buf, "(%d)RELAYING-%ds  - %d/%d -  %s  -  %d - %s%s",
								sd->lastSkipCount,
								sd->tnum,
								sd->totalListeners, sd->totalRelays,
								hostName,
								sd->syncpos, sd->agent.cstr(), buf2
								);
						} else {
							sprintf(buf, "%s-%s-%ds  - %d/%d -  %s  -  %d - %s%s",
								sd->typeStr, sd->statusStr, sd->tnum,
								sd->totalListeners, sd->totalRelays,
								hostName,
								sd->syncpos, sd->agent.cstr(), buf2
								);
						}
					} else if (sd->type == Servent::T_DIRECT){
						sprintf(buf, "%s-%s-%ds  -  %s  -  %d - %s ",
							sd->typeStr, sd->statusStr, sd->tnum,
							hostName,
							sd->syncpos, sd->agent.cstr()
							);
					} else {
						if (sd->status == Servent::S_CONNECTED){
							sprintf(buf, "%s-%s-%ds  -  %s  -  %d/%d",
								sd->typeStr, sd->statusStr, sd->tnum,
								hostName,
								sd->syncpos, sd->agent.cstr()
								);
						} else {
							sprintf(buf, "%s-%s-%ds  -  %s",
								sd->typeStr, sd->statusStr, sd->tnum,
								hostName
								);
						}
					}
					TextOut(_DrawItem->hDC,
							_DrawItem->rcItem.left + 12,
							_DrawItem->rcItem.top,
							buf,
							strlen(buf));
				}
				sd_lock.off();
			}
			break;

		case WM_SIZE:
			MoveControls(lParam);
			break;

		case WM_GETMINMAXINFO:
		{
			MINMAXINFO *pmmi = (MINMAXINFO *)lParam;
			if ( pmmi )
			{
				pmmi->ptMinTrackSize.x = 530;  // 最小幅
				pmmi->ptMinTrackSize.y = 435;  // 最小高
				pmmi->ptMaxTrackSize.x = 530; // 最大幅
				pmmi->ptMaxTrackSize.y = 1200;  // 最大高
			}
			return 0;
		}

/*		case WM_NCHITTEST:
		{
			LRESULT lResult; // カーソル位置判定結果

			// 変換一覧にあれば変換した値を返す。
			lResult = DefWindowProc( guiWnd, message, wParam, lParam );
			for( int i = 0; i < 8; i++ )
			{
				if ( lResult == trans[ i ].from )
				{
					return trans[ i ].to;
				}
			}
//			return lResult;
//			return HTCAPTION;
//			return 0;
		}*/
		
		case WM_CLOSE:
			GetWindowPlacement(hwnd, &winPlace);
			guiFlg = true;
			DestroyWindow( hwnd );
			break;

		case WM_DESTROY:
			GetWindowPlacement(hwnd, &winPlace);
			guiFlg = true;
			guiThread.active = false;
//			guiThread.lock();
			guiWnd = NULL;
//			guiThread.unlock();
			EndDialog(hwnd, LOWORD(wParam));
			break;

		//default:
			// do nothing
			//return DefDlgProc(hwnd, message, wParam, lParam);		// this recurses for ever
			//return DefWindowProc(hwnd, message, wParam, lParam);	// this stops window messages
   }
   return 0;
}
