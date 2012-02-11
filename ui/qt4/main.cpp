// --------------------------------------------------------------------------
// File : main.cpp
// Author: ◆e5bW6vDOJ.
// --------------------------------------------------------------------------
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// --------------------------------------------------------------------------

#include <stdio.h>

#ifdef _APPLE
#include <Carbon/Carbon.h>
#endif

#include <qapplication.h>

#include "peercast.h" 
#include "servmgr.h"
#include "channel.h"
#include "version2.h"

#ifdef WIN32
#include "win32/wsys.h"
#else //WIN32
#include "unix/usys.h" 
#endif //WIN32

#include "main.h"
#include "gui.h"
#include "listitem.h"

bool g_bChangeSettings = false;
std::queue<QString> g_qLog;
std::queue<tNotifyInfo> g_qNotify;
String g_iniFilename;

QMainForm *g_mainform = NULL;

class MyPeercastInst : public PeercastInstance
{
public:
	virtual Sys * APICALL createSys()
	{
#ifdef WIN32
		return new WSys(NULL);
#else //WIN32
		return new USys();
#endif //WIN32
	}
};

class MyPeercastApp : public PeercastApplication
{
public:
	virtual const char * APICALL getIniFilename()
	{
		return g_iniFilename;
	}
	
	virtual const char * APICALL getPath() 
	{
		static char path[512];
		QString str = qApp->applicationDirPath();
		str += "/";
		strcpy(path, str.toLocal8Bit().data());
		return path;
	}
	
	virtual const char *APICALL getClientTypeOS() 
	{
#ifdef WIN32
		return PCX_OS_WIN32;
#else //WIN32
 #ifdef __APPLE__
		return PCX_OS_MACOSX;
 #else //__APPLE__
 		return PCX_OS_LINUX;
 #endif //__APPLE__
#endif //WIN32
	}
	
	virtual void APICALL printLog(LogBuffer::TYPE t, const char *str)
	{
		QString temp;
		temp = "[";
		temp += LogBuffer::logTypes[t];
		temp += "] ";
		temp += QString::fromUtf8(str);
		
		printf("%s\n", temp.toLocal8Bit().data());
		
		g_qLog.push(temp);
	}
	
	virtual void APICALL getDirectory()
	{
		strcpy(servMgr->modulePath, qApp->applicationDirPath().toLocal8Bit().data());
	}
	
	virtual bool clearTemp()
	{
		return false;
	}

	virtual void APICALL channelUpdate(ChanInfo *info)
	{
		QString msg = "";
		
		if(!info || info->status != ChanInfo::S_PLAY)
			return;
		
		if(ServMgr::NT_TRACKINFO & peercastInst->getNotifyMask())
		{
			msg += QString::fromUtf8(info->track.artist.data);
			
			if(!info->track.artist.isSame("") && !info->track.title.isSame(""))
				msg += " - ";
			
			msg += QString::fromUtf8(info->track.title.data);
		}
			
		if(ServMgr::NT_BROADCASTERS & peercastInst->getNotifyMask())
		{
			if(!info->comment.isSame(""))
			{
				if(msg != "")
					msg += "\r\n";
					
				msg += "\"";
				msg += QString::fromUtf8(info->comment.data);
				msg += "\"";
			}
		}
		
		if(msg != "" && b_msg != msg)
		{
			tNotifyInfo ninfo;
			
			b_msg = msg;
			
			ninfo.type = 0;
			ninfo.name = QString::fromUtf8(info->name.data);
			ninfo.msg = msg;
			
			g_qNotify.push(ninfo);
		}
	}
	
	virtual void APICALL notifyMessage(ServMgr::NOTIFY_TYPE type, const char *msg)
	{
		if(ServMgr::NT_PEERCAST & peercastInst->getNotifyMask())
		{
			tNotifyInfo ninfo;
			
			ninfo.type = 0;
			ninfo.name = "Message from PeerCast";
			ninfo.msg = QString::fromUtf8(msg);
			
			g_qNotify.push(ninfo);
		}
	}

	virtual void APICALL updateSettings()
	{
		g_bChangeSettings = true;
	}
	
	QString b_msg;
	
//	virtual void APICALL channelStart(ChanInfo *info);
//	virtual void APICALL channelStop(ChanInfo *info);
//	virtual void APICALL openLogFile();	
};

#ifdef _APPLE
AEEventHandlerUPP sRAppHandler = NULL;

OSErr AEHandleRApp(const AppleEvent *event, AppleEvent *reply, long refcon)
{
	if(g_mainform)
		g_mainform->show();
	
    return 0;
}
#endif

#include <qplastiquestyle.h>

int main(int argc, char **argv)
{
	int ret;
	QString str;
	QApplication app(argc, argv);

#ifdef _APPLE
	QStyle *style = new QPlastiqueStyle;
	QApplication::setStyle(style);
	QApplication::setPalette(style->standardPalette());
#endif

	str = app.applicationDirPath();
	str += "/peercast.ini";
	g_iniFilename.set(str.toLocal8Bit().data());
	
	for (int i = 1; i < argc; i++)
	{
		if(!strcmp(argv[i],"--inifile") || !strcmp(argv[i],"-i"))
		{
			if(++i < argc)
				g_iniFilename.setFromString(argv[i]);
		}
		else if(!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h"))
		{
			printf("peercast - P2P Streaming Server, version %s\n\n", PCX_VERSTRING);
			printf("Copyright (c) 2002-2006 PeerCast.org <code@peercast.org>\n");
			printf("This is free software; see the source for copying conditions.\n\n");
			printf("Usage: peercast [options]\n");
			printf("  -i, --inifile <inifile>  specify ini file\n");
			printf("  -h, --help               show this help\n");
			
			return 0;
		}
	}
	
	peercastInst = new MyPeercastInst();
	peercastApp = new MyPeercastApp();
	
	peercastInst->init();
	
	servMgr->getModulePath = false;
	
	{
		QMainForm mainform;
		
		g_mainform = &mainform;
		
#ifdef _APPLE
		sRAppHandler = NewAEEventHandlerUPP(AEHandleRApp);
		AEInstallEventHandler(kCoreEventClass, kAEReopenApplication, sRAppHandler, 0, FALSE);
#endif
		
		app.setQuitOnLastWindowClosed(false);
		
		mainform.show();
		ret = app.exec();

#ifdef _APPLE		
		AERemoveEventHandler(kCoreEventClass, kAEReopenApplication, sRAppHandler, FALSE);
		DisposeAEEventHandlerUPP(sRAppHandler);
#endif
		
		g_mainform = NULL;
	}
	
	peercastInst->saveSettings();
	peercastInst->quit();

	sys->sleep(500);
	
	delete peercastApp;
	delete peercastInst;
	
	return ret;
}
