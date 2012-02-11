// ------------------------------------------------
// File : socket.h
// Date: 4-apr-2002
// Author: giles
// Desc: 
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

#ifndef _SOCKET_H
#define _SOCKET_H


#include "common.h"
#include "stream.h"

//#define DISABLE_NAGLE 1

class SocketBuffer {

public:
	SocketBuffer(const void *p, int l){
		buf = new char[l];
		len = l;
		pos = 0;
		next = NULL;
		ctime = sys->getTime();
		memcpy((void*)buf, p, l);
	}

	~SocketBuffer(){
		if (buf){
			delete [] buf;
		}
	}
	char *buf;
	int len;
	int pos;
	unsigned int ctime;
	SocketBuffer *next;
};

class SocketBufferList {

public:
	SocketBufferList(){
		top = NULL;
		last = NULL;
		skipCount = 0;
		lastSkipTime = 0;
	}

	bool isNull(){ return (top == NULL); }
	void add(const void *p, int l){
		SocketBuffer *tmp = new SocketBuffer(p,l);

		if (!last){
			top = tmp;
			last = tmp;
		} else {
			last->next = tmp;
			last = tmp;
		}

//		LOG_DEBUG("tmp = %d, top = %d, last = %d", tmp, top, last);
	}

	SocketBuffer *getTop(){
		unsigned int ctime = sys->getTime();

		while(top){
			if (top && (top->ctime + 10 >= ctime)){
				break;
			} else {
//				LOG_DEBUG("over 10sec(data skip)");
				skipCount++;
				lastSkipTime = sys->getTime();
				deleteTop();
			}
		}
		return top;
	}

	void deleteTop(){
//		LOG_DEBUG("oldtop = %d", top);
		SocketBuffer *tmp = top;
		top = tmp->next;
		delete tmp;
		if (!top){
			last = NULL;
		}

//		LOG_DEBUG("newtop = %d",top);
	}

	void clear(){
		while(top){
			SocketBuffer *tmp = top;
			top = tmp->next;
			delete tmp;
		}
		top = NULL;
		last = NULL;
	}

	SocketBuffer *top;
	SocketBuffer *last;
	unsigned int skipCount;
	unsigned int lastSkipTime;

};

// --------------------------------------------------
class ClientSocket : public Stream
{
public:

	ClientSocket()
	{
		readTimeout = 30000;
		writeTimeout = 30000;
#ifdef NULL//WIN32
		skipCount = 0;
		lastSkipTime = 0;
#endif
	}

	~ClientSocket(){
#ifdef NULL//WIN32
		bufList.clear();
#endif
	}

    // required interface
	virtual void	open(Host &) = 0;
	virtual void	bind(Host &) = 0;
	virtual void	connect() = 0;
	virtual bool	active() = 0;
	virtual ClientSocket	*accept() = 0;
	virtual Host	getLocalHost() = 0;

	virtual void	setReadTimeout(unsigned int t)
	{
		readTimeout = t;
	}
	virtual void	setWriteTimeout(unsigned int t)
	{
		writeTimeout = t;
	}
	virtual void	setBlocking(bool) {}


    static unsigned int    getIP(char *);
	static bool			getHostname(char *,size_t,unsigned int); //JP-MOD

    virtual bool eof()
    {
        return active()==false;
    }

    Host    host;

#ifdef NULL//WIN32
	SocketBufferList	bufList;
	virtual void bufferingWrite(const void *, int) = 0;
	unsigned int skipCount;
	unsigned int lastSkipTime;
#endif

	unsigned int readTimeout,writeTimeout;

};


#endif
