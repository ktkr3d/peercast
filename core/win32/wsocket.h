// ------------------------------------------------
// File : wsocket.h
// Date: 4-apr-2002
// Author: giles
// Desc: 
//		see .cpp for details
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

#ifndef _WSOCKET_H
#define _WSOCKET_H

#include <windows.h>
#include "..\common\socket.h"


// --------------------------------------------------
class WSAClientSocket : public ClientSocket
{
public:
	static void	init();

    WSAClientSocket()
	:sockNum(0)
	,writeCnt(0)
	,rbPos(0)
	,rbDataSize(0)
    {
    }

	~WSAClientSocket(){
		bufList.clear();
	}

	virtual void	open(Host &);
	virtual int		read(void *, int);
	virtual int		readUpto(void *, int);
	virtual void	write(const void *, int);
	virtual void	bind(Host &);
	virtual void	connect();
	virtual void	close();
	virtual ClientSocket * accept();
	virtual bool	active() {return sockNum != 0;}
	virtual bool	readReady();
	virtual Host 	getLocalHost();
	virtual void	setBlocking(bool);
	void	setReuse(bool);
	void	setNagle(bool);
	void	setLinger(int);
	void	setBufSize(int size);

	static	HOSTENT		*resolveHost(const char *);

	void	checkTimeout(bool,bool);
	void	checkTimeout2(bool,bool);

	virtual void	bufferingWrite(const void*, int);
	void	checkBuffering(bool, bool);

	unsigned int writeCnt;
	SOCKET sockNum;
	struct sockaddr_in remoteAddr;

	enum {RBSIZE = 8192};
	char apReadBuf[RBSIZE];
	int rbPos;
	int rbDataSize;

	WLock sockLock;
};

#endif
 