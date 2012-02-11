// ------------------------------------------------
// File : wsocket.cpp
// Date: 4-apr-2002
// Author: giles
// Desc: 
//		Windows version of ClientSocket. Handles the nitty gritty of actually
//		reading and writing TCP
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

// TODO: fix socket closing



#include "winsock2.h"
#include <windows.h>
#include <stdio.h>
#include "wsocket.h"
#include "..\common\stats.h"
#ifdef _DEBUG
#include "chkMemoryLeak.h"
#define DEBUG_NEW new(__FILE__, __LINE__)
#define new DEBUG_NEW
#endif


// --------------------------------------------------
void WSAClientSocket::init()
{
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;
    
	wVersionRequested = MAKEWORD( 2, 0 );
	err = WSAStartup( wVersionRequested, &wsaData );
	if ( err != 0 )
		throw SockException("Unable to init sockets");

    //LOG4("WSAStartup:  OK");

}
// --------------------------------------------------
bool ClientSocket::getHostname(char *str,size_t size,unsigned int ip) //JP-MOD
{
	if(size == 0)
		return false;

	HOSTENT *he;

	ip = htonl(ip);

	he = gethostbyaddr((char *)&ip,sizeof(ip),AF_INET);

	if (he)
	{
		LOG_DEBUG("getHostname: %d.%d.%d.%d -> %s", ((BYTE*)&ip)[0], ((BYTE*)&ip)[1], ((BYTE*)&ip)[2], ((BYTE*)&ip)[3], he->h_name);
		strncpy(str,he->h_name,size-1);
		str[size-1] = '\0';
		return true;
	}else
		return false;
}

unsigned int cache_ip = 0;
unsigned int cache_time = 0;

// --------------------------------------------------
unsigned int ClientSocket::getIP(char *name)
{
	unsigned int ctime = sys->getTime();
	bool null_flg = (name == NULL);

	if (null_flg){
		if ((cache_time != 0) && (cache_time + 60 > ctime)){
			return cache_ip;
		} else {
			cache_time = 0;
			cache_ip = 0;
		}
	}

	char szHostName[256];

	if (!name)
	{
		if (gethostname(szHostName, sizeof(szHostName))==0)
			name = szHostName;
		else
			return 0;
	}

	HOSTENT *he = WSAClientSocket::resolveHost(name);

	if (!he)
		return 0;


	LPSTR lpAddr = he->h_addr_list[0];
	if (lpAddr)
	{
		unsigned int ret;
		struct in_addr  inAddr;
		memmove (&inAddr, lpAddr, 4);

		ret =  inAddr.S_un.S_un_b.s_b1<<24 |
			   inAddr.S_un.S_un_b.s_b2<<16 |
			   inAddr.S_un.S_un_b.s_b3<<8 |
			   inAddr.S_un.S_un_b.s_b4;

		if (null_flg){
			cache_ip = ret;
			cache_time = ctime;
		}
		return ret;
	}
	return 0;
}
// --------------------------------------------------
void WSAClientSocket::setLinger(int sec)
{
	linger linger;
	linger.l_onoff = (sec>0)?1:0;
    linger.l_linger = sec;

	if (setsockopt(sockNum, SOL_SOCKET, SO_LINGER, (const char *)&linger, sizeof (linger)) == -1) 
		throw SockException("Unable to set LINGER");
}

// --------------------------------------------------
void WSAClientSocket::setBlocking(bool yes)
{
	unsigned long op = yes ? 0 : 1;
	if (ioctlsocket(sockNum, FIONBIO, &op) == SOCKET_ERROR)
		throw SockException("Can`t set blocking");
}
// --------------------------------------------------
void WSAClientSocket::setNagle(bool on)
{
    int nodelay = (on==false);
	if (setsockopt(sockNum, SOL_SOCKET, TCP_NODELAY, (char *)&nodelay, sizeof nodelay) == -1) 
		throw SockException("Unable to set NODELAY");

}

// --------------------------------------------------
void WSAClientSocket::setReuse(bool yes)
{
	unsigned long op = yes ? 1 : 0;
	if (setsockopt(sockNum,SOL_SOCKET,SO_REUSEADDR,(char *)&op,sizeof(unsigned long)) == -1) 
		throw SockException("Unable to set REUSE");
}

// --------------------------------------------------
void WSAClientSocket::setBufSize(int size)
{
	int oldop;
	int op = size;
	int len = sizeof(op);
	if (getsockopt(sockNum,SOL_SOCKET,SO_RCVBUF,(char *)&oldop,&len) == -1) {
		LOG_DEBUG("Unable to get RCVBUF");
	} else if (oldop < size) {
		if (setsockopt(sockNum,SOL_SOCKET,SO_RCVBUF,(char *)&op,len) == -1) 
			LOG_DEBUG("Unable to set RCVBUF");
		//else
		//	LOG_DEBUG("*** recvbufsize:%d -> %d", oldop, op);
	}

	if (getsockopt(sockNum,SOL_SOCKET,SO_SNDBUF,(char *)&oldop,&len) == -1) {
		LOG_DEBUG("Unable to get SNDBUF");
	} else if (oldop < size) {
		if (setsockopt(sockNum,SOL_SOCKET,SO_SNDBUF,(char *)&op,len) == -1) 
			LOG_DEBUG("Unable to set SNDBUF");
		//else
		//	LOG_DEBUG("*** sendbufsize: %d -> %d", oldop, op);
	}
}

// --------------------------------------------------
HOSTENT *WSAClientSocket::resolveHost(const char *hostName)
{
	HOSTENT *he;

	if ((he = gethostbyname(hostName)) == NULL)
	{
		// if failed, try using gethostbyaddr instead

		unsigned long ip = inet_addr(hostName);
		
		if (ip == INADDR_NONE)
			return NULL;

		if ((he = gethostbyaddr((char *)&ip,sizeof(ip),AF_INET)) == NULL)
			return NULL;	
	}
	return he;
}

// --------------------------------------------------
void WSAClientSocket::open(Host &rh)
{
	sockNum = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (sockNum == INVALID_SOCKET)
		throw SockException("Can`t open socket");

	setBlocking(false);
#ifdef DISABLE_NAGLE
	setNagle(false);
#endif
	setBufSize(65535);

	host = rh;

	memset(&remoteAddr,0,sizeof(remoteAddr));

	remoteAddr.sin_family = AF_INET;
	remoteAddr.sin_port = htons(host.port);
	remoteAddr.sin_addr.S_un.S_addr = htonl(host.ip);

}
// --------------------------------------------------
void WSAClientSocket::checkTimeout(bool r, bool w)
{
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK)
    {

		timeval timeout;
		fd_set read_fds;
		fd_set write_fds;

		timeout.tv_sec = 0;
		timeout.tv_usec = 0;

        FD_ZERO (&write_fds);
		if (w)
		{
			timeout.tv_sec = (int)this->writeTimeout/1000;
			FD_SET (sockNum, &write_fds);
		}

        FD_ZERO (&read_fds);
		if (r)
		{
			timeout.tv_sec = (int)this->readTimeout/1000;
	        FD_SET (sockNum, &read_fds);
		}

		timeval *tp;
		if (timeout.tv_sec)
			tp = &timeout;
		else
			tp = NULL;


		int r=select (NULL, &read_fds, &write_fds, NULL, tp);

        if (r == 0)
			throw TimeoutException();
		else if (r == SOCKET_ERROR)
			throw SockException("select failed.");

	}else{
		char str[32];
		sprintf(str,"%d",err);
		throw SockException(str);
	}
}

// --------------------------------------------------
void WSAClientSocket::checkTimeout2(bool r, bool w)
{
    {

		timeval timeout;
		fd_set read_fds;
		fd_set write_fds;

		timeout.tv_sec = 0;
		timeout.tv_usec = 0;

        FD_ZERO (&write_fds);
		if (w)
		{
			timeout.tv_sec = (int)this->writeTimeout/1000;
			FD_SET (sockNum, &write_fds);
		}

        FD_ZERO (&read_fds);
		if (r)
		{
			timeout.tv_sec = (int)this->readTimeout/1000;
	        FD_SET (sockNum, &read_fds);
		}

		timeval *tp;
		if (timeout.tv_sec)
			tp = &timeout;
		else
			tp = NULL;


		int r=select (NULL, &read_fds, &write_fds, NULL, tp);

        if (r == 0)
			throw TimeoutException();
		else if (r == SOCKET_ERROR)
			throw SockException("select failed.");
	}
}

// --------------------------------------------------
Host WSAClientSocket::getLocalHost()
{
	struct sockaddr_in localAddr;

	int len = sizeof(localAddr);
    if (getsockname(sockNum, (sockaddr *)&localAddr, &len) == 0)
		return Host(SWAP4(localAddr.sin_addr.s_addr),0);
	else
		return Host(0,0);
}

// --------------------------------------------------
void WSAClientSocket::connect()
{
	if (::connect(sockNum,(struct sockaddr *)&remoteAddr,sizeof(remoteAddr)) == SOCKET_ERROR)
		checkTimeout(false,true);

}
// --------------------------------------------------
int WSAClientSocket::read(void *p, int l)
{
	int bytesRead=0;

	while (l)
	{
		if (rbDataSize >= l) {
			memcpy(p, &apReadBuf[rbPos], l);
			rbPos += l;
			rbDataSize -= l;
			return l;
		} else if (rbDataSize > 0) {
			memcpy(p, &apReadBuf[rbPos], rbDataSize);
			p = (char *) p + rbDataSize;
			l -= rbDataSize;
			bytesRead += rbDataSize;
		}

		rbPos = 0;
		rbDataSize = 0;
		//int r = recv(sockNum, (char *)p, l, 0);
		int r = recv(sockNum, apReadBuf, RBSIZE, 0);
		if (r == SOCKET_ERROR)
		{
			// non-blocking sockets always fall through to here
			checkTimeout(true,false);

		}else if (r == 0)
		{
			throw EOFException("Closed on read");

		}else
		{
			stats.add(Stats::BYTESIN,r);
			if (host.localIP())
				stats.add(Stats::LOCALBYTESIN,r);
			updateTotals(r,0);
			//bytesRead += r;
			//l -= r;
			//p = (char *)p+r;

			rbDataSize += r;
		}
	}
	return bytesRead;
}
// --------------------------------------------------
int WSAClientSocket::readUpto(void *p, int l)
{
	int bytesRead=0;
	while (l)
	{
		int r = recv(sockNum, (char *)p, l, 0);
		if (r == SOCKET_ERROR)
		{
			// non-blocking sockets always fall through to here
			//checkTimeout(true,false);
			return r;

		}else if (r == 0)
		{
			break;
		}else
		{
			stats.add(Stats::BYTESIN,r);
			if (host.localIP())
				stats.add(Stats::LOCALBYTESIN,r);
			updateTotals(r,0);
			bytesRead += r;
			l -= r;
			p = (char *)p+r;
		}
		if (bytesRead) break;
	}
	return bytesRead;
}


// --------------------------------------------------
void WSAClientSocket::write(const void *p, int l)
{
	while (l)
	{
		int r = send(sockNum, (char *)p, l, 0);
		if (r == SOCKET_ERROR)
		{
			checkTimeout(false,true);	
		}
		else if (r == 0)
		{
			throw SockException("Closed on write");
		}
		else
		if (r > 0)
		{
			stats.add(Stats::BYTESOUT,r);
			if (host.localIP())
				stats.add(Stats::LOCALBYTESOUT,r);

			updateTotals(0,r);
			l -= r;
			p = (char *)p+r;
		}
	}
}

// --------------------------------------------------
void WSAClientSocket::bufferingWrite(const void *p, int l)
{
	if (bufList.isNull() && p != NULL){
		while(l){
			int r = send(sockNum, (char *)p, l, 0);
			if (r == SOCKET_ERROR){
				int err = WSAGetLastError();
				if (err == WSAEWOULDBLOCK){
					bufList.add(p, l);
//					LOG_DEBUG("normal add");
					break;
				} else {
					char str[32];
					sprintf(str,"%d",err);
					throw SockException(str);
				}
			} else if (r == 0) {
				throw SockException("Closed on write");
			} else if (r > 0){
				stats.add(Stats::BYTESOUT,r);
				if (host.localIP())
					stats.add(Stats::LOCALBYTESOUT,r);

				updateTotals(0,r);
				l -= r;
				p = (char *)p+r;
			}
		}
	} else {
//		LOG_DEBUG("***************BufferingWrite");
		if (p)
			bufList.add(p,l);

		bool flg = true;

		while(flg){
			SocketBuffer *tmp;
			tmp = bufList.getTop();

			if (tmp){
//				LOG_DEBUG("tmp->pos = %d, tmp->len = %d, %d", tmp->pos, tmp->len, tmp);
				while(tmp->pos < tmp->len){
					int r = send(sockNum, (char*)(tmp->buf + tmp->pos), tmp->len - tmp->pos, 0);
//					LOG_DEBUG("send = %d", r);
					if (r == SOCKET_ERROR){
						int err = WSAGetLastError();
						if (err == WSAEWOULDBLOCK){
							flg = false;
							break;
						} else {
							bufList.clear();
							char str[32];
							sprintf(str,"%d",err);
							throw SockException(str);
						}
					} else if (r == 0){
						bufList.clear();
						throw SockException("Closed on write");
					} else if (r > 0){
						stats.add(Stats::BYTESOUT,r);
						if (host.localIP())
							stats.add(Stats::LOCALBYTESOUT,r);

						updateTotals(0,r);

						tmp->pos += r;
						if (tmp->pos >= tmp->len){
//							LOG_DEBUG("deleteTop");
							bufList.deleteTop();
							break;
						}
					}
				}
			} else {
				flg = false;
			}
		}
//		LOG_DEBUG("bufferingWrite end");
	}
}

// --------------------------------------------------
void WSAClientSocket::checkBuffering(bool r, bool w)
{
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK)
    {

		timeval timeout;
		fd_set read_fds;
		fd_set write_fds;

		timeout.tv_sec = 0;
		timeout.tv_usec = 0;

        FD_ZERO (&write_fds);
		if (w)
		{
			timeout.tv_sec = (int)this->writeTimeout/1000;
			FD_SET (sockNum, &write_fds);
		}

        FD_ZERO (&read_fds);
		if (r)
		{
			timeout.tv_sec = (int)this->readTimeout/1000;
	        FD_SET (sockNum, &read_fds);
		}

		timeval *tp;
		if (timeout.tv_sec)
			tp = &timeout;
		else
			tp = NULL;


		int r=select (NULL, &read_fds, &write_fds, NULL, tp);

        if (r == 0)
			throw TimeoutException();
		else if (r == SOCKET_ERROR)
			throw SockException("select failed.");

	}else{
		char str[32];
		sprintf(str,"%d",err);
		throw SockException(str);
	}
}

// --------------------------------------------------
void WSAClientSocket::bind(Host &h)
{
	struct sockaddr_in localAddr;

	if ((sockNum = socket (PF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
		throw SockException("Can`t open socket");

	setBlocking(false);
	setReuse(true);

	memset(&localAddr,0,sizeof(localAddr));
	localAddr.sin_family = AF_INET;
	localAddr.sin_port = htons(h.port);
	localAddr.sin_addr.s_addr = INADDR_ANY;

	if( ::bind (sockNum, (sockaddr *)&localAddr, sizeof(localAddr)) == -1)
		throw SockException("Can`t bind socket");

	if (::listen(sockNum,SOMAXCONN))
		throw SockException("Can`t listen",WSAGetLastError());

	host = h;
}
// --------------------------------------------------
ClientSocket *WSAClientSocket::accept()
{

	int fromSize = sizeof(sockaddr_in);
	sockaddr_in from;

	SOCKET conSock = ::accept(sockNum,(sockaddr *)&from,&fromSize);


	if (conSock ==  INVALID_SOCKET)
		return NULL;

	
    WSAClientSocket *cs = new WSAClientSocket();
	cs->sockNum = conSock;

	cs->host.port = (from.sin_port & 0xff) << 8 | ((from.sin_port >> 8) & 0xff);
	cs->host.ip = from.sin_addr.S_un.S_un_b.s_b1<<24 |
				  from.sin_addr.S_un.S_un_b.s_b2<<16 |
				  from.sin_addr.S_un.S_un_b.s_b3<<8 |
				  from.sin_addr.S_un.S_un_b.s_b4;


	cs->setBlocking(false);
#ifdef DISABLE_NAGLE
	cs->setNagle(false);
#endif
	cs->setBufSize(65535);

	return cs;
}

// --------------------------------------------------
void WSAClientSocket::close()
{
	sockLock.on();
	if (sockNum)
	{
		shutdown(sockNum,SD_SEND);

		setReadTimeout(2000);
		unsigned int stime = sys->getTime();
		try
		{
			char c[1024];
			while (read(&c, sizeof(c)) > 0)
				if (sys->getTime() - stime > 5)
					break;
		}catch(StreamException &) {}

		if (closesocket (sockNum))
			LOG_ERROR("closesocket() error");


		sockNum=0;
	}
	sockLock.off();
}

// --------------------------------------------------
bool	WSAClientSocket::readReady()
{
	if (rbDataSize) return true;

	timeval timeout;
	fd_set read_fds;

	timeout.tv_sec = 0;
	timeout.tv_usec = 0;

    FD_ZERO (&read_fds);
    FD_SET (sockNum, &read_fds);

	return select (sockNum+1, &read_fds, NULL, NULL, &timeout) == 1;
}

