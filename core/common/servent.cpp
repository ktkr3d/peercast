// ------------------------------------------------
// File : servent.cpp
// Date: 4-apr-2002
// Author: giles
// Desc: 
//		Servents are the actual connections between clients. They do the handshaking,
//		transfering of data and processing of GnuPackets. Each servent has one socket allocated
//		to it on connect, it uses this to transfer all of its data.
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
// todo: make lan->yp not check firewall

#include <stdlib.h>
#include "servent.h"
#include "sys.h"
#include "gnutella.h"
#include "xml.h"
#include "html.h"
#include "http.h"
#include "stats.h"
#include "servmgr.h"
#include "peercast.h"
#include "atom.h"
#include "pcp.h"
#include "version2.h"
#ifdef _DEBUG
#include "chkMemoryLeak.h"
#define DEBUG_NEW new(__FILE__, __LINE__)
#define new DEBUG_NEW
#endif


const int DIRECT_WRITE_TIMEOUT = 60;

// -----------------------------------
char *Servent::statusMsgs[]=
{
        "NONE",
		"CONNECTING",
        "PROTOCOL",
        "HANDSHAKE",
        "CONNECTED",
        "CLOSING",
		"LISTENING",
		"TIMEOUT",
		"REFUSED",
		"VERIFIED",
		"ERROR",
		"WAIT",
		"FREE"
};

// -----------------------------------
char *Servent::typeMsgs[]=
{
		"NONE",
        "INCOMING",
        "SERVER",
		"RELAY",
		"DIRECT",
		"COUT",
		"CIN",
		"PGNU"
};
// -----------------------------------
bool	Servent::isPrivate() 
{
	Host h = getHost();
	return servMgr->isFiltered(ServFilter::F_PRIVATE,h) || h.isLocalhost();
}
// -----------------------------------
bool	Servent::isAllowed(int a) 
{
	Host h = getHost();

	if (servMgr->isFiltered(ServFilter::F_BAN,h))
		return false;

	return (allow&a)!=0;
}

// -----------------------------------
bool	Servent::isFiltered(int f) 
{
	Host h = getHost();
	return servMgr->isFiltered(f,h);
}

int servent_count = 1;
// -----------------------------------
Servent::Servent(int index)
:outPacketsPri(MAX_OUTPACKETS)
,outPacketsNorm(MAX_OUTPACKETS)
,seenIDs(MAX_HASH)
,serventIndex(index)
,sock(NULL)
,next(NULL)
{
	reset();
	servent_id = servent_count++;
	lastSkipTime = 0;
	lastSkipCount = 0;
	waitPort = 0;
}

// -----------------------------------
Servent::~Servent()
{	
	
}
// -----------------------------------
void	Servent::kill() 
{
	thread.active = false;
		
	setStatus(S_CLOSING);

	if (pcpStream)
	{
		PCPStream *pcp = pcpStream;
		pcpStream = NULL;
		pcp->kill();
		delete pcp;
	}

	chanMgr->hitlistlock.on();
	ChanHitList *chl = chanMgr->findHitListByID(chanID);
	if (chl) {
		ChanHit *chh = chl->hit;
		ChanHit *prev = NULL;
		while (chh) {
			if (chh->servent_id == this->servent_id){
				if ((servMgr->kickKeepTime != 0) && (chh->firewalled == 1)){
					chh->numHops = 0;
					chh->numListeners = 0;
					chh->numRelays = 0;
					prev = chh;
					chh = chh->next;
				} else {
					ChanHit *next = chh->next;
					if (prev)
						prev->next = next;
					else
						chl->hit = next;

					delete chh;
					chh = next;
				}
			} else {
				prev = chh;
				chh = chh->next;
			}
		}
	}
	chanMgr->hitlistlock.off();

	if (sock)
	{
		sock->close();
		delete sock;
		sock = NULL;
	}

	if (pushSock)
	{
		pushSock->close();
		delete pushSock;
		pushSock = NULL;
	}

//	thread.unlock();

	if (type != T_SERVER)
	{
		reset();
		setStatus(S_FREE);
	}

}
// -----------------------------------
void	Servent::abort() 
{
	thread.active = false;
	if (sock)
	{
		sock->close();
	}

}

// -----------------------------------
void Servent::reset()
{

	remoteID.clear();

	servPort = 0;

	pcpStream = NULL;

	flowControl = false;
	networkID.clear();

	chanID.clear();

	outputProtocol = ChanInfo::SP_UNKNOWN;

	agent.clear();
	sock = NULL;
	allow = ALLOW_ALL;
	syncPos = 0;
	addMetadata = false;
	nsSwitchNum = 0;
	pack.func = 255;
	lastConnect = lastPing = lastPacket = 0;
	loginPassword.clear();
	loginMount.clear();
	bytesPerSecond = 0;
	priorityConnect = false;
	pushSock = NULL;
	sendHeader = true;

	outPacketsNorm.reset();
	outPacketsPri.reset();

	seenIDs.clear();

	status = S_NONE;
	type = T_NONE;

	serventHit.init();
}
// -----------------------------------
bool Servent::sendPacket(ChanPacket &pack,GnuID &cid,GnuID &sid,GnuID &did,Servent::TYPE t)
{

	if  (	   (type == t) 
			&& (isConnected())
			&& (!cid.isSet() || chanID.isSame(cid))
			&& (!sid.isSet() || !sid.isSame(remoteID))
			&& (pcpStream != NULL)
		)
	{
		return pcpStream->sendPacket(pack,did);
	}
	return false;
}


// -----------------------------------
bool Servent::acceptGIV(ClientSocket *givSock)
{
	if (!pushSock)
	{
		pushSock = givSock;
		return true;
	}else
		return false;
}

// -----------------------------------
Host Servent::getHost()
{
	Host h(0,0);

	if (sock)
		h = sock->host;

	return h;
}

// -----------------------------------
bool Servent::outputPacket(GnuPacket &p, bool pri)
{
	lock.on();

	bool r=false;
	if (pri)
		r = outPacketsPri.write(p);
	else
	{
		if (servMgr->useFlowControl)
		{
			int per = outPacketsNorm.percentFull();
			if (per > 50)
				flowControl = true;
			else if (per < 25)
				flowControl = false;
		}


		bool send=true;
		if (flowControl)
		{
			// if in flowcontrol, only allow packets with less of a hop count than already in queue
			if (p.hops >= outPacketsNorm.findMinHop())
				send = false;
		}

		if (send)
			r = outPacketsNorm.write(p);
	}

	lock.off();
	return r;
}

// -----------------------------------
bool Servent::initServer(Host &h)
{
	try
	{
		checkFree();

		status = S_WAIT;

		createSocket();

		sock->bind(h);

		thread.data = this;

		thread.func = serverProc;

		type = T_SERVER;

		if (!sys->startThread(&thread))
			throw StreamException("Can`t start thread");

	}catch(StreamException &e)
	{
		LOG_ERROR("Bad server: %s",e.msg);
		kill();
		return false;
	}

	return true;
}
// -----------------------------------
void Servent::checkFree()
{
	if (sock)
		throw StreamException("Socket already set");
	if (thread.active)
		throw StreamException("Thread already active");
}
// -----------------------------------
void Servent::initIncoming(ClientSocket *s, unsigned int a)
{

	try{

		checkFree();

		type = T_INCOMING;
		sock = s;
		allow = a;
		thread.data = this;
		thread.func = incomingProc;
		thread.finish = false;

		setStatus(S_PROTOCOL);

		char ipStr[64];
		sock->host.toStr(ipStr);
		LOG_DEBUG("Incoming from %s",ipStr);


		if (!sys->startThread(&thread))
			throw StreamException("Can`t start thread");
	}catch(StreamException &e)
	{
		//LOG_ERROR("!!FATAL!! Incoming error: %s",e.msg);
		//servMgr->shutdownTimer = 1;  	
		kill();

		LOG_ERROR("INCOMING FAILED: %s",e.msg);

	}
}

// -----------------------------------
void Servent::initOutgoing(TYPE ty)
{
	try 
	{
		checkFree();


		type = ty;

		thread.data = this;
		thread.func = outgoingProc;

		if (!sys->startThread(&thread))
			throw StreamException("Can`t start thread");

	}catch(StreamException &e)
	{
		LOG_ERROR("Unable to start outgoing: %s",e.msg);
		kill();
	}
}

// -----------------------------------
void Servent::initPCP(Host &rh)
{
	char ipStr[64];
	rh.toStr(ipStr);
	try 
	{
		checkFree();

	    createSocket();

		type = T_COUT;

		sock->open(rh);

		if (!isAllowed(ALLOW_NETWORK))
			throw StreamException("Servent not allowed");

		thread.data = this;
		thread.func = outgoingProc;

		LOG_DEBUG("Outgoing to %s",ipStr);

		if (!sys->startThread(&thread))
			throw StreamException("Can`t start thread");

	}catch(StreamException &e)
	{
		LOG_ERROR("Unable to open connection to %s - %s",ipStr,e.msg);
		kill();
	}
}

#if 0
// -----------------------------------
void	Servent::initChannelFetch(Host &host)
{
	type = T_STREAM;

	char ipStr[64];
	host.toStr(ipStr);

	checkFree();
	 
	createSocket();
		
	sock->open(host);

		
	if (!isAllowed(ALLOW_DATA))	
		throw StreamException("Servent not allowed");
		
	sock->connect();
}
#endif

// -----------------------------------
void Servent::initGIV(Host &h, GnuID &id)
{
	char ipStr[64];
	h.toStr(ipStr);
	try 
	{
		checkFree();

		givID = id;

	    createSocket();

		sock->open(h);

		if (!isAllowed(ALLOW_NETWORK))
			throw StreamException("Servent not allowed");
		
		sock->connect();

		thread.data = this;
		thread.func = givProc;

		type = T_RELAY;

		if (!sys->startThread(&thread))
			throw StreamException("Can`t start thread");

	}catch(StreamException &e)
	{
		LOG_ERROR("GIV error to %s: %s",ipStr,e.msg);
		kill();
	}
}
// -----------------------------------
void Servent::createSocket()
{
	if (sock)
		LOG_ERROR("Servent::createSocket attempt made while active");

	sock = sys->createSocket();
}
// -----------------------------------
void Servent::setStatus(STATUS s)
{
	if (s != status)
	{
		status = s;

		if ((s == S_HANDSHAKE) || (s == S_CONNECTED) || (s == S_LISTENING))
			lastConnect = sys->getTime();
	}

}

// -----------------------------------
void Servent::handshakeOut()
{
    sock->writeLine(GNU_PEERCONN);

	char str[64];
    
	sock->writeLineF("%s %s",HTTP_HS_AGENT,PCX_AGENT);
    sock->writeLineF("%s %d",PCX_HS_PCP,1);

	if (priorityConnect)
	    sock->writeLineF("%s %d",PCX_HS_PRIORITY,1);
	
	if (networkID.isSet())
	{
		networkID.toStr(str);
		sock->writeLineF("%s %s",PCX_HS_NETWORKID,str);
	}

	servMgr->sessionID.toStr(str);
	sock->writeLineF("%s %s",PCX_HS_ID,str);

	
    sock->writeLineF("%s %s",PCX_HS_OS,peercastApp->getClientTypeOS());
	
	sock->writeLine("");

	HTTP http(*sock);

	int r = http.readResponse();

	if (r != 200)
	{
		LOG_ERROR("Expected 200, got %d",r);
		throw StreamException("Unexpected HTTP response");
	}


	bool versionValid = false;

	GnuID clientID;
	clientID.clear();

    while (http.nextHeader())
    {
		LOG_DEBUG(http.cmdLine);

		char *arg = http.getArgStr();
		if (!arg)
			continue;

		if (http.isHeader(HTTP_HS_AGENT))
		{
			agent.set(arg);

			if (strnicmp(arg,"PeerCast/",9)==0)
				versionValid = (stricmp(arg+9,MIN_CONNECTVER)>=0);
		}else if (http.isHeader(PCX_HS_NETWORKID))
			clientID.fromStr(arg);
    }

	if (!clientID.isSame(networkID))
		throw HTTPException(HTTP_SC_UNAVAILABLE,503);

	if (!versionValid)
		throw HTTPException(HTTP_SC_UNAUTHORIZED,401);


    sock->writeLine(GNU_OK);
    sock->writeLine("");

}


// -----------------------------------
void Servent::processOutChannel()
{
}


// -----------------------------------
void Servent::handshakeIn()
{

	int osType=0;

	HTTP http(*sock);


	bool versionValid = false;
	bool diffRootVer = false;

	GnuID clientID;
	clientID.clear();

    while (http.nextHeader())
    {
		LOG_DEBUG("%s",http.cmdLine);

		char *arg = http.getArgStr();
		if (!arg)
			continue;

		if (http.isHeader(HTTP_HS_AGENT))
		{
			agent.set(arg);

			if (strnicmp(arg,"PeerCast/",9)==0)
			{
				versionValid = (stricmp(arg+9,MIN_CONNECTVER)>=0);
				diffRootVer = stricmp(arg+9,MIN_ROOTVER)<0;
			}
		}else if (http.isHeader(PCX_HS_NETWORKID))
		{
			clientID.fromStr(arg);

		}else if (http.isHeader(PCX_HS_PRIORITY))
		{
			priorityConnect = atoi(arg)!=0;

		}else if (http.isHeader(PCX_HS_ID))
		{
			GnuID id;
			id.fromStr(arg);
			if (id.isSame(servMgr->sessionID))
				throw StreamException("Servent loopback");

		}else if (http.isHeader(PCX_HS_OS))
		{
			if (stricmp(arg,PCX_OS_LINUX)==0)
				osType = 1;
			else if (stricmp(arg,PCX_OS_WIN32)==0)
				osType = 2;
			else if (stricmp(arg,PCX_OS_MACOSX)==0)
				osType = 3;
			else if (stricmp(arg,PCX_OS_WINAMP2)==0)
				osType = 4;
		}

    }

	if (!clientID.isSame(networkID))
		throw HTTPException(HTTP_SC_UNAVAILABLE,503);

	// if this is a priority connection and all incoming connections 
	// are full then kill an old connection to make room. Otherwise reject connection.
	//if (!priorityConnect)
	{
		if (!isPrivate())
			if (servMgr->pubInFull())
				throw HTTPException(HTTP_SC_UNAVAILABLE,503);
	}

	if (!versionValid)
		throw HTTPException(HTTP_SC_FORBIDDEN,403);

    sock->writeLine(GNU_OK);

    sock->writeLineF("%s %s",HTTP_HS_AGENT,PCX_OLDAGENT);

	if (networkID.isSet())
	{
		char idStr[64];
		networkID.toStr(idStr);
		sock->writeLineF("%s %s",PCX_HS_NETWORKID,idStr);
	}

	if (servMgr->isRoot)
	{
		sock->writeLineF("%s %d",PCX_HS_FLOWCTL,servMgr->useFlowControl?1:0);
		sock->writeLineF("%s %d",PCX_HS_MINBCTTL,chanMgr->minBroadcastTTL);
		sock->writeLineF("%s %d",PCX_HS_MAXBCTTL,chanMgr->maxBroadcastTTL);
		sock->writeLineF("%s %d",PCX_HS_RELAYBC,servMgr->relayBroadcast);
		//sock->writeLine("%s %d",PCX_HS_FULLHIT,2);


		if (diffRootVer)
		{
			sock->writeString(PCX_HS_DL);
			sock->writeLine(PCX_DL_URL);
		}

		sock->writeLineF("%s %s",PCX_HS_MSG,servMgr->rootMsg.cstr());
	}


	char hostIP[64];
	Host h = sock->host;
	h.IPtoStr(hostIP);
    sock->writeLineF("%s %s",PCX_HS_REMOTEIP,hostIP);


    sock->writeLine("");


	while (http.nextHeader());
}

// -----------------------------------
bool	Servent::pingHost(Host &rhost,GnuID &rsid)
{
	char ipstr[64];
	rhost.toStr(ipstr);
	LOG_DEBUG("Ping host %s: trying..",ipstr);
	ClientSocket *s=NULL;
	bool hostOK=false;
	try
	{
		s = sys->createSocket();
		if (!s)
			return false;
		else
		{

			s->setReadTimeout(15000);
			s->setWriteTimeout(15000);
			s->open(rhost);
			s->connect();

			AtomStream atom(*s);

			atom.writeInt(PCP_CONNECT,1);
			atom.writeParent(PCP_HELO,1);
				atom.writeBytes(PCP_HELO_SESSIONID,servMgr->sessionID.id,16);

			GnuID sid;
			sid.clear();

			int numc,numd;
			ID4 id = atom.read(numc,numd);
			if (id == PCP_OLEH)
			{
				for(int i=0; i<numc; i++)
				{
					int c,d;
					ID4 pid = atom.read(c,d);
					if (pid == PCP_SESSIONID)
						atom.readBytes(sid.id,16,d);
					else
						atom.skip(c,d);
				}
			}else
			{
				LOG_DEBUG("Ping response: %s",id.getString().str());
				throw StreamException("Bad ping response");
			}

			if (!sid.isSame(rsid))
				throw StreamException("SIDs don`t match");

			hostOK = true;
			LOG_DEBUG("Ping host %s: OK",ipstr);
			atom.writeInt(PCP_QUIT,PCP_ERROR_QUIT);


		}
	}catch(StreamException &e)
	{
		LOG_DEBUG("Ping host %s: %s",ipstr,e.msg);
	}
	if (s)
	{
		s->close();
		delete s;
	}

	if (!hostOK)
		rhost.port = 0;

	return true;
}

WLock canStreamLock;

// -----------------------------------
bool Servent::handshakeStream(ChanInfo &chanInfo)
{

	HTTP http(*sock);


	bool gotPCP=false;
	unsigned int reqPos=0;
	unsigned short listenPort = 0;

	nsSwitchNum=0;

	while (http.nextHeader())
	{
		char *arg = http.getArgStr();
		if (!arg)
			continue;

		if (http.isHeader(PCX_HS_PCP))
			gotPCP = atoi(arg)!=0;
		else if (http.isHeader(PCX_HS_POS))
			reqPos = atoi(arg);
		else if (http.isHeader(PCX_HS_PORT))
			listenPort = (unsigned short) atoi(arg);
		else if (http.isHeader("icy-metadata"))
			addMetadata = atoi(arg) > 0;
		else if (http.isHeader(HTTP_HS_AGENT))
			agent = arg;
		else if (http.isHeader("Pragma"))
		{
			char *ssc = stristr(arg,"stream-switch-count=");
			char *so = stristr(arg,"stream-offset");

			if (ssc || so)
			{
				nsSwitchNum=1;
				//nsSwitchNum = atoi(ssc+20);
			}
		}

		LOG_DEBUG("Stream: %s",http.cmdLine);
	}


	if ((!gotPCP) && (outputProtocol == ChanInfo::SP_PCP))
		outputProtocol = ChanInfo::SP_PEERCAST;

	if (outputProtocol == ChanInfo::SP_HTTP)
	{
		if  ( (chanInfo.srcProtocol == ChanInfo::SP_MMS)
			  || (chanInfo.contentType == ChanInfo::T_WMA)
			  || (chanInfo.contentType == ChanInfo::T_WMV)
			  || (chanInfo.contentType == ChanInfo::T_ASX)
			)
		outputProtocol = ChanInfo::SP_MMS;
	}


	bool chanFound=false;
	bool chanReady=false;

	ChanHit *sourceHit = NULL;

	Channel *ch = chanMgr->findChannelByID(chanInfo.id);
	if (ch)
	{
		sendHeader = true;
		if (reqPos || !isIndexTxt(&chanInfo))
		{
			streamPos = ch->rawData.findOldestPos(reqPos);
			//streamPos = ch->rawData.getLatestPos();
		}else
		{
 			streamPos = ch->rawData.getLatestPos();
		}

		chanID = chanInfo.id;
		serventHit.host.ip = getHost().ip;
		serventHit.host.port = listenPort;
		if (serventHit.host.globalIP())
			serventHit.rhost[0] = serventHit.host;
		else
			serventHit.rhost[1] = serventHit.host;
		serventHit.chanID = chanID;

		canStreamLock.on();
 		chanReady = canStream(ch);
		if (0 && !chanReady && ch->isPlaying())
		{
			if (ch->info.getUptime() > 60
				&& servMgr->numStreams(chanID, Servent::T_RELAY, false) == 0)
			{
				sourceHit = &ch->sourceHost;  // send source host info

				if (listenPort)
				{
					// connect "this" host later
					chanMgr->addHit(serventHit);
				}

				char tmp[50];
				getHost().toStr(tmp);
				LOG_DEBUG("Bump channel, hand over sourceHost to %s", tmp);
				ch->bump = true;
			}
			else if (servMgr->kickUnrelayableHost(chanID, serventHit) != 0)
			{
				chanReady = canStream(ch);
				if (!chanReady)
					LOG_DEBUG("*** Kicked unrelayable host, but still cannot stream");
			}
		}
		if (!chanReady) type = T_INCOMING;
		thread.active = chanReady;
		setStatus(S_CONNECTED);
		canStreamLock.off();

		//JP-Patch add-s
		if (servMgr->isCheckPushStream())
		{
			if (chanReady == true)
			{
				Host h = getHost();

				if (!h.isLocalhost()) 
				{
					do 
					{
						if (strstr(agent.cstr(),"PeerCast/0.119") != NULL) 
						{						
							char strip[256];
							h.toStr(strip);
							LOG_ERROR("Block v0.119 Servent : %s (%s)",strip,agent.cstr());
							chanReady = false;
							break;
						}

/*						ChanHitList *hits[ChanMgr::MAX_HITLISTS];
						int numHits=0;
						for(int i=0; i<ChanMgr::MAX_HITLISTS; i++)
						{
							ChanHitList *chl = &chanMgr->hitlists[i];
							if (chl->isUsed())
								hits[numHits++] = chl;
						}
						bool isfw = false;
						int numRelay = 0;
						for(int i=0; i<numHits; i++)
						{
							ChanHitList *chl = hits[i];
							if (chl->isUsed())
							{
								for (int j=0; j<ChanHitList::MAX_HITS; j++)
								{
									ChanHit *hit = &chl->hits[j];
									if (hit->host.isValid() && (h.ip == hit->host.ip)) 
									{
										if (hit->firewalled)
											isfw = true;
										numRelay = hit->numRelays;
									}
								}
							}
						}
						if ((isfw == true) && (numRelay == 0))
						{
							char strip[256];
							h.toStr(strip);
							LOG_ERROR("Block firewalled Servent : %s",strip);
							chanReady = false;
						}*/

						ChanHitList *chl = chanMgr->findHitList(chanInfo);
						ChanHit *hit = chl->hit;
						while(hit){
							if (hit->host.isValid() && (h.ip == hit->host.ip))
							{
								if ((hit->firewalled) && (hit->numRelays == 0)){
									char strip[256];
									h.toStr(strip);
									LOG_ERROR("Block firewalled Servent : %s",strip);
									chanReady = false;
								}
							}
							hit = hit->next;
						}
					}while (0);
				}		
			}
		}
		//JP-Patch add-e
	}

//	LockBlock lockblock(chanMgr->hitlistlock);

//	lockblock.lockon();
	ChanHitList *chl = chanMgr->findHitList(chanInfo);

	if (chl)
	{
		chanFound = true;
	}


	bool result = false;

	char idStr[64];
	chanInfo.id.toStr(idStr);

	char sidStr[64];
	servMgr->sessionID.toStr(sidStr);

	Host rhost = sock->host;




	AtomStream atom(*sock);



	if (!chanFound)
	{
		sock->writeLine(HTTP_SC_NOTFOUND);
	    sock->writeLine("");
		LOG_DEBUG("Sending channel not found");
		return false;
	}


	if (!chanReady)
	{
		if (outputProtocol == ChanInfo::SP_PCP)
		{

			char tbuf[8196];
			MemoryStream mem(tbuf, sizeof(tbuf));
			mem.writeLine(HTTP_SC_UNAVAILABLE);
			mem.writeLineF("%s %s",HTTP_HS_CONTENT,MIME_XPCP);
			mem.writeLine("");
			sock->write(tbuf, mem.getPosition());

			handshakeIncomingPCP(atom,rhost,remoteID,agent);

			char ripStr[64];
			rhost.toStr(ripStr);

			LOG_DEBUG("Sending channel unavailable");

			ChanHitSearch chs;

			mem.rewind();
			AtomStream atom2(mem);

			int error = PCP_ERROR_QUIT+PCP_ERROR_UNAVAILABLE;

			if (sourceHit) {
				sourceHit->writeAtoms(atom2,chanInfo.id);	
				char tmp[50];
				sourceHit->host.toStr(tmp);
				LOG_DEBUG("relay info(sourceHit): %s", tmp);
			}

			chanMgr->hitlistlock.on();

			chl = chanMgr->findHitList(chanInfo);

			if (chl && !sourceHit)
			{
				ChanHit best;
				
				// search for up to 8 other hits
				int cnt=0;
				int i;
				for(i=0; i<8; i++)
				{
					best.init();


					// find best hit this network if local IP
					if (!rhost.globalIP())
					{
						chs.init();
						chs.matchHost = servMgr->serverHost;
						chs.waitDelay = 2;
						chs.excludeID = remoteID;
						if (chl->pickHits(chs)){
							best = chs.best[0];
							LOG_DEBUG("find best hit this network if local IP");
						}
					}

					// find best hit on same network
					if (!best.host.ip)
					{
						chs.init();
						chs.matchHost = rhost;
						chs.waitDelay = 2;
						chs.excludeID = remoteID;
						if (chl->pickHits(chs)){
							best = chs.best[0];
							LOG_DEBUG("find best hit on same network");
						}

					}

					// find best hit on other networks
/*					if (!best.host.ip)
					{
						chs.init();
						chs.waitDelay = 2;
						chs.excludeID = remoteID;
						if (chl->pickHits(chs)){
							best = chs.best[0];
							LOG_DEBUG("find best hit on other networks");
						}

					}*/
					
					if (!best.host.ip)
						break;

					best.writeAtoms(atom2,chanInfo.id);				
					cnt++;
				}

				if (!best.host.ip){
					char tmp[50];
//					chanMgr->hitlistlock.on();
					int rhcnt = chs.getRelayHost(servMgr->serverHost, rhost, remoteID, chl);
//					chanMgr->hitlistlock.off();
					for (int i = 0; i < rhcnt; i++){
						chs.best[i].writeAtoms(atom2, chanInfo.id);
						chs.best[i].host.toStr(tmp);
						LOG_DEBUG("relay info: %s hops = %d", tmp, chs.best[i].numHops);
						best.host.ip = chs.best[i].host.ip;
					}
					cnt += rhcnt;
				}

				if (cnt)
				{
					LOG_DEBUG("Sent %d channel hit(s) to %s",cnt,ripStr);

				}
				else if (rhost.port)
				{
					// find firewalled host
					chs.init();
					chs.waitDelay = 30;
					chs.useFirewalled = true;
					chs.excludeID = remoteID;
					if (chl->pickHits(chs))
					{
						best = chs.best[0];
						int cnt = servMgr->broadcastPushRequest(best,rhost,chl->info.id,Servent::T_RELAY);
						LOG_DEBUG("Broadcasted channel push request to %d clients for %s",cnt,ripStr);
					}
				} 

				// if all else fails, use tracker
				if (!best.host.ip)
				{
					// find best tracker on this network if local IP
					if (!rhost.globalIP())
					{
						chs.init();
						chs.matchHost = servMgr->serverHost;
						chs.trackersOnly = true;
						chs.excludeID = remoteID;
						if (chl->pickHits(chs))
							best = chs.best[0];

					}

					// find local tracker
					if (!best.host.ip)
					{
						chs.init();
						chs.matchHost = rhost;
						chs.trackersOnly = true;
						chs.excludeID = remoteID;
						if (chl->pickHits(chs))
							best = chs.best[0];
					}

					// find global tracker
					if (!best.host.ip)
					{
						chs.init();
						chs.trackersOnly = true;
						chs.excludeID = remoteID;
						if (chl->pickHits(chs))
							best = chs.best[0];
					}

					if (best.host.ip)
					{
						best.writeAtoms(atom2,chanInfo.id);				
						LOG_DEBUG("Sent 1 tracker hit to %s",ripStr);
					}else if (rhost.port)
					{
						// find firewalled tracker
						chs.init();
						chs.useFirewalled = true;
						chs.trackersOnly = true;
						chs.excludeID = remoteID;
						chs.waitDelay = 30;
						if (chl->pickHits(chs))
						{
							best = chs.best[0];
							int cnt = servMgr->broadcastPushRequest(best,rhost,chl->info.id,Servent::T_CIN);
							LOG_DEBUG("Broadcasted tracker push request to %d clients for %s",cnt,ripStr);
						}
					}

				}


			}

			chanMgr->hitlistlock.off();

			// return not available yet code
			atom2.writeInt(PCP_QUIT,error);
			sock->write(tbuf, mem.getPosition());
			result = false;

			/*
			char c[512];
			// wait disconnect from other host
			try{
				while(sock->read(c, sizeof(c))){
					sys->sleep(10);
				}
			}catch(StreamException &e){
				LOG_DEBUG("RelayInfoWait: %s",e.msg);
			}
			*/
		}else
		{
			LOG_DEBUG("Sending channel unavailable");
			sock->writeLine(HTTP_SC_UNAVAILABLE);
			sock->writeLine("");
			result = false;
		}

	} else {

		if (chanInfo.contentType != ChanInfo::T_MP3)
			addMetadata=false;

		if (addMetadata && (outputProtocol == ChanInfo::SP_HTTP))		// winamp mp3 metadata check
		{

			sock->writeLine(ICY_OK);

			sock->writeLineF("%s %s",HTTP_HS_SERVER,PCX_AGENT);
			sock->writeLineF("icy-name:%s",chanInfo.name.cstr());
			sock->writeLineF("icy-br:%d",chanInfo.bitrate);
			sock->writeLineF("icy-genre:%s",chanInfo.genre.cstr());
			sock->writeLineF("icy-url:%s",chanInfo.url.cstr());
			sock->writeLineF("icy-metaint:%d",chanMgr->icyMetaInterval);
			sock->writeLineF("%s %s",PCX_HS_CHANNELID,idStr);

			sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_MP3);

		}else
		{

			sock->writeLine(HTTP_SC_OK);

			if ((chanInfo.contentType != ChanInfo::T_ASX) && (chanInfo.contentType != ChanInfo::T_WMV) && (chanInfo.contentType != ChanInfo::T_WMA))
			{
				sock->writeLineF("%s %s",HTTP_HS_SERVER,PCX_AGENT);

				sock->writeLine("Accept-Ranges: none");

				sock->writeLineF("x-audiocast-name: %s",chanInfo.name.cstr());
				sock->writeLineF("x-audiocast-bitrate: %d",chanInfo.bitrate);
				sock->writeLineF("x-audiocast-genre: %s",chanInfo.genre.cstr());
				sock->writeLineF("x-audiocast-description: %s",chanInfo.desc.cstr());
				sock->writeLineF("x-audiocast-url: %s",chanInfo.url.cstr());
				sock->writeLineF("%s %s",PCX_HS_CHANNELID,idStr);
			}


			if (outputProtocol == ChanInfo::SP_HTTP)
			{
				switch (chanInfo.contentType)
				{
					case ChanInfo::T_OGG:
						sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_XOGG);
						break;
					case ChanInfo::T_MP3:
						sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_MP3);
						break;
					case ChanInfo::T_MOV:
						sock->writeLine("Connection: close");
						sock->writeLine("Content-Length: 10000000");
						sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_MOV);
						break;
					case ChanInfo::T_MPG:
						sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_MPG);
						break;
					case ChanInfo::T_NSV:
						sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_NSV);
						break;
					case ChanInfo::T_ASX:
						sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_ASX);
						break;
					case ChanInfo::T_WMA:
						sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_WMA);
						break;
					case ChanInfo::T_WMV:
						sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_WMV);
						break;
				}
			} else if (outputProtocol == ChanInfo::SP_MMS)
			{
				sock->writeLine("Server: Rex/9.0.0.2980");
				sock->writeLine("Cache-Control: no-cache");
				sock->writeLine("Pragma: no-cache");
				sock->writeLine("Pragma: client-id=3587303426");
				sock->writeLine("Pragma: features=\"broadcast,playlist\"");

				if (nsSwitchNum)
				{
					sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_MMS);
				}else
				{
					sock->writeLine("Content-Type: application/vnd.ms.wms-hdr.asfv1");
					if (ch)
						sock->writeLineF("Content-Length: %d",ch->headPack.len);
					sock->writeLine("Connection: Keep-Alive");
				}
			
			} else if (outputProtocol == ChanInfo::SP_PCP)
			{
				sock->writeLineF("%s %d",PCX_HS_POS,streamPos);
				sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_XPCP);

			}else if (outputProtocol == ChanInfo::SP_PEERCAST)
			{
				sock->writeLineF("%s %s",HTTP_HS_CONTENT,MIME_XPEERCAST);
			}
		}
		sock->writeLine("");
		result = true;

		if (gotPCP)
		{
			handshakeIncomingPCP(atom,rhost,remoteID,agent);
			atom.writeInt(PCP_OK,0);
			if (rhost.globalIP())
				serventHit.rhost[0] = rhost;
			else
				serventHit.rhost[1] = rhost;
			serventHit.sessionID = remoteID;
			serventHit.numHops = 1;
			chanMgr->addHit(serventHit);
		}

	}



	return result;
}

// -----------------------------------
void Servent::handshakeGiv(GnuID &id)
{
	if (id.isSet())
	{
		char idstr[64];
		id.toStr(idstr);
		sock->writeLineF("GIV /%s",idstr);
	}else
		sock->writeLine("GIV");

	sock->writeLine("");
}


// -----------------------------------
void Servent::processGnutella()
{
	type = T_PGNU;

	//if (servMgr->isRoot && !servMgr->needConnections())
	if (servMgr->isRoot)
	{
		processRoot();
		return;
	}



	gnuStream.init(sock);
	setStatus(S_CONNECTED);

	if (!servMgr->isRoot)
	{
		chanMgr->broadcastRelays(this, 1, 1);
		GnuPacket *p;

		if ((p=outPacketsNorm.curr())) 	
			gnuStream.sendPacket(*p);
		return;
	}

	gnuStream.ping(2);

//	if (type != T_LOOKUP)
//		chanMgr->broadcastRelays(this,chanMgr->minBroadcastTTL,2);

	lastPacket = lastPing = sys->getTime();
	bool doneBigPing=false;

	const unsigned int	abortTimeoutSecs = 60;		// abort connection after 60 secs of no activitiy
	const unsigned int	packetTimeoutSecs = 30;		// ping connection after 30 secs of no activity

	unsigned int currBytes=0;
	unsigned int lastWait=0;

	unsigned int lastTotalIn=0,lastTotalOut=0;

	while (thread.active && sock->active())
	{

		if (sock->readReady())
		{
			lastPacket = sys->getTime();

			if (gnuStream.readPacket(pack))
			{
				char ipstr[64];
				sock->host.toStr(ipstr);

				GnuID routeID;
				GnuStream::R_TYPE ret = GnuStream::R_PROCESS;

				if (pack.func != GNU_FUNC_PONG)
					if (servMgr->seenPacket(pack))
						ret = GnuStream::R_DUPLICATE;

				seenIDs.add(pack.id);


				if (ret == GnuStream::R_PROCESS)
				{
					GnuID routeID;
					ret = gnuStream.processPacket(pack,this,routeID);

					if (flowControl && (ret == GnuStream::R_BROADCAST))
						ret = GnuStream::R_DROP;

				}

				switch(ret)
				{
					case GnuStream::R_BROADCAST:
						if (servMgr->broadcast(pack,this))
							stats.add(Stats::NUMBROADCASTED);
						else
							stats.add(Stats::NUMDROPPED);
						break;
					case GnuStream::R_ROUTE:
						if (servMgr->route(pack,routeID,NULL))
							stats.add(Stats::NUMROUTED);
						else
							stats.add(Stats::NUMDROPPED);
						break;
					case GnuStream::R_ACCEPTED:
						stats.add(Stats::NUMACCEPTED);
						break;
					case GnuStream::R_DUPLICATE:
						stats.add(Stats::NUMDUP);
						break;
					case GnuStream::R_DEAD:
						stats.add(Stats::NUMDEAD);
						break;
					case GnuStream::R_DISCARD:
						stats.add(Stats::NUMDISCARDED);
						break;
					case GnuStream::R_BADVERSION:
						stats.add(Stats::NUMOLD);
						break;
					case GnuStream::R_DROP:
						stats.add(Stats::NUMDROPPED);
						break;
				}


				LOG_NETWORK("packet in: %s-%s, %d bytes, %d hops, %d ttl, from %s",GNU_FUNC_STR(pack.func),GnuStream::getRouteStr(ret),pack.len,pack.hops,pack.ttl,ipstr);



			}else{
				LOG_ERROR("Bad packet");
			}
		}


		GnuPacket *p;

		if ((p=outPacketsPri.curr()))				// priority packet
		{
			gnuStream.sendPacket(*p);
			seenIDs.add(p->id);
			outPacketsPri.next();
		} else if ((p=outPacketsNorm.curr())) 		// or.. normal packet
		{
			gnuStream.sendPacket(*p);
			seenIDs.add(p->id);
			outPacketsNorm.next();
		}

		int lpt =  sys->getTime()-lastPacket;

		if (!doneBigPing)
		{
			if ((sys->getTime()-lastPing) > 15)
			{
				gnuStream.ping(7);
				lastPing = sys->getTime();
				doneBigPing = true;
			}
		}else{
			if (lpt > packetTimeoutSecs)
			{
				
				if ((sys->getTime()-lastPing) > packetTimeoutSecs)
				{
					gnuStream.ping(1);
					lastPing = sys->getTime();
				}

			}
		}
		if (lpt > abortTimeoutSecs)
			throw TimeoutException();


		unsigned int totIn = sock->totalBytesIn-lastTotalIn;
		unsigned int totOut = sock->totalBytesOut-lastTotalOut;

		unsigned int bytes = totIn+totOut;

		lastTotalIn = sock->totalBytesIn;
		lastTotalOut = sock->totalBytesOut;

		const int serventBandwidth = 1000;

		int delay = sys->idleSleepTime;
		if ((bytes) && (serventBandwidth >= 8))
			delay = (bytes*1000)/(serventBandwidth/8);	// set delay relative packetsize

		if (delay < (int)sys->idleSleepTime)
			delay = sys->idleSleepTime;
		//LOG("delay %d, in %d, out %d",delay,totIn,totOut);

		sys->sleep(delay);
	}

}


// -----------------------------------
void Servent::processRoot()
{
	try 
	{
	
		gnuStream.init(sock);
		setStatus(S_CONNECTED);

		gnuStream.ping(2);

		unsigned int lastConnect = sys->getTime();

		while (thread.active && sock->active())
		{
			if (gnuStream.readPacket(pack))
			{
				char ipstr[64];
				sock->host.toStr(ipstr);
				
				LOG_NETWORK("packet in: %d from %s",pack.func,ipstr);


				if (pack.func == GNU_FUNC_PING)		// if ping then pong back some hosts and close
				{
					
					Host hl[32];
					int cnt = servMgr->getNewestServents(hl,32,sock->host);	
					if (cnt)
					{
						int start = sys->rnd() % cnt;
						int max = cnt>8?8:cnt;

						for(int i=0; i<max; i++)
						{
							GnuPacket pong;
							pack.hops = 1;
							pong.initPong(hl[start],false,pack);
							gnuStream.sendPacket(pong);

							char ipstr[64];
							hl[start].toStr(ipstr);

							//LOG_NETWORK("Pong %d: %s",start+1,ipstr);
							start = (start+1) % cnt;
						}
						char str[64];
						sock->host.toStr(str);
						LOG_NETWORK("Sent %d pong(s) to %s",max,str);
					}else
					{
						LOG_NETWORK("No Pongs to send");
						//return;
					}
				}else if (pack.func == GNU_FUNC_PONG)		// pong?
				{
					MemoryStream pong(pack.data,pack.len);

					int ip,port;
					port = pong.readShort();
					ip = pong.readLong();
					ip = SWAP4(ip);


					Host h(ip,port);
					if ((ip) && (port) && (h.globalIP()))
					{

						LOG_NETWORK("added pong: %d.%d.%d.%d:%d",ip>>24&0xff,ip>>16&0xff,ip>>8&0xff,ip&0xff,port);
						servMgr->addHost(h,ServHost::T_SERVENT,sys->getTime());
					}
					//return;
				} else if (pack.func == GNU_FUNC_HIT)
				{
					MemoryStream data(pack.data,pack.len);
					ChanHit hit;
					gnuStream.readHit(data,hit,pack.hops,pack.id);
				}

				//if (gnuStream.packetsIn > 5)	// die if we get too many packets
				//	return;
			}

			if((sys->getTime()-lastConnect > 60))
				break;
		}


	}catch(StreamException &e)
	{
		LOG_ERROR("Relay: %s",e.msg);
	}

	
}	

// -----------------------------------
int Servent::givProc(ThreadInfo *thread)
{
//	thread->lock();
	Servent *sv = (Servent*)thread->data;
	try 
	{
		sv->handshakeGiv(sv->givID);
		sv->handshakeIncoming();

	}catch(StreamException &e)
	{
		LOG_ERROR("GIV: %s",e.msg);
	}

	sv->kill();
	sys->endThread(thread);
	return 0;
}

// -----------------------------------
void Servent::handshakeOutgoingPCP(AtomStream &atom, Host &rhost, GnuID &rid, String &agent, bool isTrusted)
{

	bool nonFW = (servMgr->getFirewall() != ServMgr::FW_ON);
	bool testFW = (servMgr->getFirewall() == ServMgr::FW_UNKNOWN);

	bool sendBCID = isTrusted && chanMgr->isBroadcasting();

	char tbuf[1024];
	MemoryStream mem(tbuf, sizeof(tbuf));
	AtomStream atom2(mem);
	atom2.writeParent(PCP_HELO,3 + (testFW?1:0) + (nonFW?1:0) + (sendBCID?1:0));
		atom2.writeString(PCP_HELO_AGENT,PCX_AGENT);
		atom2.writeInt(PCP_HELO_VERSION,PCP_CLIENT_VERSION);
		atom2.writeBytes(PCP_HELO_SESSIONID,servMgr->sessionID.id,16);
		if (nonFW)
			atom2.writeShort(PCP_HELO_PORT,servMgr->serverHost.port);
		if (testFW)
			atom2.writeShort(PCP_HELO_PING,servMgr->serverHost.port);
		if (sendBCID)
			atom2.writeBytes(PCP_HELO_BCID,chanMgr->broadcastID.id,16);
	atom.io.write(tbuf, mem.getPosition());


	LOG_DEBUG("PCP outgoing waiting for OLEH..");

	int numc,numd;
	ID4 id = atom.read(numc,numd);
	if (id != PCP_OLEH)
	{
		LOG_DEBUG("PCP outgoing reply: %s",id.getString().str());
		atom.writeInt(PCP_QUIT,PCP_ERROR_QUIT+PCP_ERROR_BADRESPONSE);
		throw StreamException("Got unexpected PCP response");
	}



	char arg[64];

	GnuID clientID;
	clientID.clear();
	rid.clear();
	int version=0;
	int disable=0;

	Host thisHost;

	// read OLEH response
	for(int i=0; i<numc; i++)
	{
		int c,dlen;
		ID4 id = atom.read(c,dlen);

		if (id == PCP_HELO_AGENT)
		{
			atom.readString(arg,sizeof(arg),dlen);
			agent.set(arg);

		}else if (id == PCP_HELO_REMOTEIP)
		{
			thisHost.ip = atom.readInt();

		}else if (id == PCP_HELO_PORT)
		{
			thisHost.port = atom.readShort();

		}else if (id == PCP_HELO_VERSION)
		{
			version = atom.readInt();

		}else if (id == PCP_HELO_DISABLE)
		{
			disable = atom.readInt();

		}else if (id == PCP_HELO_SESSIONID)
		{
			atom.readBytes(rid.id,16);
			if (rid.isSame(servMgr->sessionID))
				throw StreamException("Servent loopback");

		}else
		{
			LOG_DEBUG("PCP handshake skip: %s",id.getString().str());
			atom.skip(c,dlen);
		}

    }


	// update server ip/firewall status
	if (isTrusted)
	{
		if (thisHost.isValid())
		{
			if ((servMgr->serverHost.ip != thisHost.ip) && (servMgr->forceIP.isEmpty()))
			{
				char ipstr[64];
				thisHost.toStr(ipstr);
				LOG_DEBUG("Got new ip: %s",ipstr);
				servMgr->serverHost.ip = thisHost.ip;
			}

			if (servMgr->getFirewall() == ServMgr::FW_UNKNOWN)
			{
				if (thisHost.port && thisHost.globalIP())
					servMgr->setFirewall(ServMgr::FW_OFF);
				else
					servMgr->setFirewall(ServMgr::FW_ON);
			}
		}

		if (disable == 1)
		{
			LOG_ERROR("client disabled: %d",disable);
			servMgr->isDisabled = true;		
		}else
		{
			servMgr->isDisabled = false;		
		}
	}



	if (!rid.isSet())
	{
		atom.writeInt(PCP_QUIT,PCP_ERROR_QUIT+PCP_ERROR_NOTIDENTIFIED);
		throw StreamException("Remote host not identified");
	}

	LOG_DEBUG("PCP Outgoing handshake complete.");

}

// -----------------------------------
void Servent::handshakeIncomingPCP(AtomStream &atom, Host &rhost, GnuID &rid, String &agent)
{
	int numc,numd;
	ID4 id = atom.read(numc,numd);


	if (id != PCP_HELO)
	{
		LOG_DEBUG("PCP incoming reply: %s",id.getString().str());
		atom.writeInt(PCP_QUIT,PCP_ERROR_QUIT+PCP_ERROR_BADRESPONSE);
		throw StreamException("Got unexpected PCP response");
	}

	char arg[64];

	ID4 osType;

	int version=0;

	int pingPort=0;

	GnuID bcID;
	GnuID clientID;

	bcID.clear();
	clientID.clear();

	rhost.port = 0;

	for(int i=0; i<numc; i++)
	{

		int c,dlen;
		ID4 id = atom.read(c,dlen);

		if (id == PCP_HELO_AGENT)
		{
			atom.readString(arg,sizeof(arg),dlen);
			agent.set(arg);

		}else if (id == PCP_HELO_VERSION)
		{
			version = atom.readInt();

		}else if (id == PCP_HELO_SESSIONID)
		{
			atom.readBytes(rid.id,16);
			if (rid.isSame(servMgr->sessionID))
				throw StreamException("Servent loopback");

		}else if (id == PCP_HELO_BCID)
		{
			atom.readBytes(bcID.id,16);

		}else if (id == PCP_HELO_OSTYPE)
		{
			osType = atom.readInt();
		}else if (id == PCP_HELO_PORT)
		{
			rhost.port = atom.readShort();
		}else if (id == PCP_HELO_PING)
		{
			pingPort = atom.readShort();
		}else
		{
			LOG_DEBUG("PCP handshake skip: %s",id.getString().str());
			atom.skip(c,dlen);
		}

    }

	if (version)
		LOG_DEBUG("Incoming PCP is %s : v%d", agent.cstr(),version);


	if (!rhost.globalIP() && servMgr->serverHost.globalIP())
		rhost.ip = servMgr->serverHost.ip;

	if (pingPort)
	{
		char ripStr[64];
		rhost.toStr(ripStr);
		LOG_DEBUG("Incoming firewalled test request: %s ", ripStr);
		rhost.port = pingPort;
		if (!rhost.globalIP() || !pingHost(rhost,rid))
			rhost.port = 0;
	}

	if (servMgr->isRoot)
	{
		if (bcID.isSet())
		{
			if (bcID.getFlags() & 1)	// private
			{
				BCID *bcid = servMgr->findValidBCID(bcID);
				if (!bcid || (bcid && !bcid->valid))
				{
					atom.writeParent(PCP_OLEH,1);
					atom.writeInt(PCP_HELO_DISABLE,1);
					throw StreamException("Client is banned");
				}
			}
		}
	}


	char tbuf[1024];
	MemoryStream mem(tbuf, sizeof(tbuf));
	AtomStream atom2(mem);
	atom2.writeParent(PCP_OLEH,5);
		atom2.writeString(PCP_HELO_AGENT,PCX_AGENT);
		atom2.writeBytes(PCP_HELO_SESSIONID,servMgr->sessionID.id,16);
		atom2.writeInt(PCP_HELO_VERSION,PCP_CLIENT_VERSION);
		atom2.writeInt(PCP_HELO_REMOTEIP,rhost.ip);
		atom2.writeShort(PCP_HELO_PORT,rhost.port);

	if (version)
	{
		if (version < PCP_CLIENT_MINVERSION)
		{
			atom2.writeInt(PCP_QUIT,PCP_ERROR_QUIT+PCP_ERROR_BADAGENT);
			atom.io.write(tbuf, mem.getPosition());
			throw StreamException("Agent is not valid");
		}
	}

	if (!rid.isSet())
	{
		atom2.writeInt(PCP_QUIT,PCP_ERROR_QUIT+PCP_ERROR_NOTIDENTIFIED);
		atom.io.write(tbuf, mem.getPosition());
		throw StreamException("Remote host not identified");
	}



	if (servMgr->isRoot)
	{
		servMgr->writeRootAtoms(atom2,false);
	}

	atom.io.write(tbuf, mem.getPosition());

	LOG_DEBUG("PCP Incoming handshake complete.");

}

// -----------------------------------
void Servent::processIncomingPCP(bool suggestOthers)
{
	PCPStream::readVersion(*sock);


	AtomStream atom(*sock);
	Host rhost = sock->host;

	handshakeIncomingPCP(atom,rhost,remoteID,agent);


	bool alreadyConnected = (servMgr->findConnection(Servent::T_COUT,remoteID)!=NULL)
							|| (servMgr->findConnection(Servent::T_CIN,remoteID)!=NULL);
	bool unavailable = servMgr->controlInFull();
	bool offair = !servMgr->isRoot && !chanMgr->isBroadcasting();

	char rstr[64];
	rhost.toStr(rstr);

	if (unavailable || alreadyConnected || offair)
	{
		int error;

		if (alreadyConnected)
			error = PCP_ERROR_QUIT+PCP_ERROR_ALREADYCONNECTED;
		else if (unavailable)
			error = PCP_ERROR_QUIT+PCP_ERROR_UNAVAILABLE;
		else if (offair)
			error = PCP_ERROR_QUIT+PCP_ERROR_OFFAIR;
		else 
			error = PCP_ERROR_QUIT;


		if (suggestOthers)
		{

			ChanHit best;
			ChanHitSearch chs;

			int cnt=0;
			for(int i=0; i<8; i++)
			{
				best.init();

				// find best hit on this network			
				if (!rhost.globalIP())
				{
					chs.init();
					chs.matchHost = servMgr->serverHost;
					chs.waitDelay = 2;
					chs.excludeID = remoteID;
					chs.trackersOnly = true;
					chs.useBusyControls = false;
					if (chanMgr->pickHits(chs))
						best = chs.best[0];
				}

				// find best hit on same network			
				if (!best.host.ip)
				{
					chs.init();
					chs.matchHost = rhost;
					chs.waitDelay = 2;
					chs.excludeID = remoteID;
					chs.trackersOnly = true;
					chs.useBusyControls = false;
					if (chanMgr->pickHits(chs))
						best = chs.best[0];
				}

				// else find best hit on other networks
				if (!best.host.ip)
				{
					chs.init();
					chs.waitDelay = 2;
					chs.excludeID = remoteID;
					chs.trackersOnly = true;
					chs.useBusyControls = false;
					if (chanMgr->pickHits(chs))
						best = chs.best[0];
				}

				if (!best.host.ip)
					break;
				
				GnuID noID;
				noID.clear();
				best.writeAtoms(atom,noID);
				cnt++;
			}
			if (cnt)
			{
				LOG_DEBUG("Sent %d tracker(s) to %s",cnt,rstr);
			}
			else if (rhost.port)
			{
				// send push request to best firewalled tracker on other network
				chs.init();
				chs.waitDelay = 30;
				chs.excludeID = remoteID;
				chs.trackersOnly = true;
				chs.useFirewalled = true;
				chs.useBusyControls = false;
				if (chanMgr->pickHits(chs))
				{
					best = chs.best[0];
					GnuID noID;
					noID.clear();
					int cnt = servMgr->broadcastPushRequest(best,rhost,noID,Servent::T_CIN);
					LOG_DEBUG("Broadcasted tracker push request to %d clients for %s",cnt,rstr);
				}
			}else
			{
				LOG_DEBUG("No available trackers");
			}
		}


		LOG_ERROR("Sending QUIT to incoming: %d",error);

		atom.writeInt(PCP_QUIT,error);
		return;		
	}
	

	type = T_CIN;
	setStatus(S_CONNECTED);

	atom.writeInt(PCP_OK,0);

	// ask for update
	atom.writeParent(PCP_ROOT,1);
		atom.writeParent(PCP_ROOT_UPDATE,0);

	pcpStream = new PCPStream(remoteID);

	int error = 0;
	BroadcastState bcs;
	while (!error && thread.active && !sock->eof())
	{
		error = pcpStream->readPacket(*sock,bcs);
		sys->sleepIdle();

		if (!servMgr->isRoot && !chanMgr->isBroadcasting())
			error = PCP_ERROR_OFFAIR;
		if (peercastInst->isQuitting)
			error = PCP_ERROR_SHUTDOWN;
	}

	pcpStream->flush(*sock);

	error += PCP_ERROR_QUIT;
	atom.writeInt(PCP_QUIT,error);

	LOG_DEBUG("PCP Incoming to %s closed: %d",rstr,error);

}

// -----------------------------------
int Servent::outgoingProc(ThreadInfo *thread)
{
//	thread->lock();
	LOG_DEBUG("COUT started");

	Servent *sv = (Servent*)thread->data;
		
	GnuID noID;
	noID.clear();
	sv->pcpStream = new PCPStream(noID);

	while (sv->thread.active)
	{
		sv->setStatus(S_WAIT);

		if (chanMgr->isBroadcasting() && servMgr->autoServe)
		{
			ChanHit bestHit;
			ChanHitSearch chs;
			char ipStr[64];

			do
			{
				bestHit.init();

				if (servMgr->rootHost.isEmpty())
					break;

				if (sv->pushSock)
				{
					sv->sock = sv->pushSock;
					sv->pushSock = NULL;
					bestHit.host = sv->sock->host;
					break;
				}

				GnuID noID;
				noID.clear();
				ChanHitList *chl = chanMgr->findHitListByID(noID);
				if (chl)
				{
					// find local tracker
					chs.init();
					chs.matchHost = servMgr->serverHost;
					chs.waitDelay = MIN_TRACKER_RETRY;
					chs.excludeID = servMgr->sessionID;
					chs.trackersOnly = true;
					if (!chl->pickHits(chs))
					{
						// else find global tracker
						chs.init();
						chs.waitDelay = MIN_TRACKER_RETRY;
						chs.excludeID = servMgr->sessionID;
						chs.trackersOnly = true;
						chl->pickHits(chs);
					}

					if (chs.numResults)
					{
						bestHit = chs.best[0];
					}
				}


				unsigned int ctime = sys->getTime();

				if ((!bestHit.host.ip) && ((ctime-chanMgr->lastYPConnect) > MIN_YP_RETRY))
				{
					bestHit.host.fromStrName(servMgr->rootHost.cstr(),DEFAULT_PORT);
					bestHit.yp = true;
					chanMgr->lastYPConnect = ctime;
				}
				sys->sleepIdle();

			}while (!bestHit.host.ip && (sv->thread.active));


			if (!bestHit.host.ip)		// give up
			{
				LOG_ERROR("COUT giving up");
				break;
			}


			bestHit.host.toStr(ipStr);

			int error=0;
			try 
			{

				LOG_DEBUG("COUT to %s: Connecting..",ipStr);

				if (!sv->sock)
				{
					sv->setStatus(S_CONNECTING);
					sv->sock = sys->createSocket();
					if (!sv->sock)
						throw StreamException("Unable to create socket");
					sv->sock->open(bestHit.host);
					sv->sock->connect();

				}

				sv->sock->setReadTimeout(30000);
				AtomStream atom(*sv->sock);

				sv->setStatus(S_HANDSHAKE);

				Host rhost = sv->sock->host;
				atom.writeInt(PCP_CONNECT,1);
				handshakeOutgoingPCP(atom,rhost,sv->remoteID,sv->agent,bestHit.yp);

				sv->setStatus(S_CONNECTED);

				LOG_DEBUG("COUT to %s: OK",ipStr);

				sv->pcpStream->init(sv->remoteID);

				BroadcastState bcs;
				bcs.servent_id = sv->servent_id;
				error = 0;
				while (!error && sv->thread.active && !sv->sock->eof() && servMgr->autoServe)
				{
					error = sv->pcpStream->readPacket(*sv->sock,bcs);

					sys->sleepIdle();

					if (!chanMgr->isBroadcasting())
						error = PCP_ERROR_OFFAIR;
					if (peercastInst->isQuitting)
						error = PCP_ERROR_SHUTDOWN;

					if (sv->pcpStream->nextRootPacket)
						if (sys->getTime() > (sv->pcpStream->nextRootPacket+30))
							error = PCP_ERROR_NOROOT;
				}
				sv->setStatus(S_CLOSING);

				sv->pcpStream->flush(*sv->sock);

				error += PCP_ERROR_QUIT;
				atom.writeInt(PCP_QUIT,error);

				LOG_ERROR("COUT to %s closed: %d",ipStr,error);

			}catch(TimeoutException &e)
			{
				LOG_ERROR("COUT to %s: timeout (%s)",ipStr,e.msg);
				sv->setStatus(S_TIMEOUT);
			}catch(StreamException &e)
			{
				LOG_ERROR("COUT to %s: %s",ipStr,e.msg);
				sv->setStatus(S_ERROR);
			}

			try
			{
				if (sv->sock)
				{
					sv->sock->close();
					delete sv->sock;
					sv->sock = NULL;
				}

			}catch(StreamException &) {}

			// don`t discard this hit if we caused the disconnect (stopped broadcasting)
			if (error != (PCP_ERROR_QUIT+PCP_ERROR_OFFAIR))
				chanMgr->deadHit(bestHit);

		}

		sys->sleepIdle();
	}

	sv->kill();
	sys->endThread(thread);
	LOG_DEBUG("COUT ended");
	return 0;
}
// -----------------------------------
int Servent::incomingProc(ThreadInfo *thread)
{
//	thread->lock();

	Servent *sv = (Servent*)thread->data;
	
	char ipStr[64];
	sv->sock->host.toStr(ipStr);

	try 
	{
		sv->handshakeIncoming();
	}catch(HTTPException &e)
	{
		try
		{
			sv->sock->writeLine(e.msg);
			if (e.code == 401)
				sv->sock->writeLine("WWW-Authenticate: Basic realm=\"PeerCast\"");
			sv->sock->writeLine("");
		}catch(StreamException &){}
		LOG_ERROR("Incoming from %s: %s",ipStr,e.msg);
	}catch(StreamException &e)
	{
		LOG_ERROR("Incoming from %s: %s",ipStr,e.msg);
	}


	sv->kill();
	sys->endThread(thread);
	return 0;
}
// -----------------------------------
void Servent::processServent()
{
	setStatus(S_HANDSHAKE);

	handshakeIn();

	if (!sock)
		throw StreamException("Servent has no socket");

	processGnutella();
}

// -----------------------------------
void Servent::processStream(bool doneHandshake,ChanInfo &chanInfo)
{	
	if (!doneHandshake)
	{
		setStatus(S_HANDSHAKE);

		if (!handshakeStream(chanInfo))
			return;
	}

	if (chanInfo.id.isSet())
	{

		chanID = chanInfo.id;

		LOG_CHANNEL("Sending channel: %s ",ChanInfo::getProtocolStr(outputProtocol));

		if (!waitForChannelHeader(chanInfo))
			throw StreamException("Channel not ready");

		servMgr->totalStreams++;

		Host host = sock->host;
		host.port = 0;	// force to 0 so we ignore the incoming port

		Channel *ch = chanMgr->findChannelByID(chanID);
		if (!ch)
			throw StreamException("Channel not found");

		if (outputProtocol == ChanInfo::SP_HTTP)
		{
			if ((addMetadata) && (chanMgr->icyMetaInterval))
				sendRawMetaChannel(chanMgr->icyMetaInterval);
			else 
				sendRawChannel(true,true);

		}else if (outputProtocol == ChanInfo::SP_MMS)
		{
			if (nsSwitchNum)
			{
				sendRawChannel(true,true);
			}else
			{
				sendRawChannel(true,false);
			}

		}else if (outputProtocol  == ChanInfo::SP_PCP)
		{
			sendPCPChannel();

		} else if (outputProtocol  == ChanInfo::SP_PEERCAST)
		{
			sendPeercastChannel();
		}
	}

	setStatus(S_CLOSING);
}

// -----------------------------------------
#if 0
// debug
		FileStream file;
		file.openReadOnly("c://test.mp3");

		LOG_DEBUG("raw file read");
		char buf[4000];
		int cnt=0;
		while (!file.eof())
		{
			LOG_DEBUG("send %d",cnt++);
			file.read(buf,sizeof(buf));
			sock->write(buf,sizeof(buf));

		}
		file.close();
		LOG_DEBUG("raw file sent");

	return;
// debug
#endif
// -----------------------------------
bool Servent::waitForChannelHeader(ChanInfo &info)
{
	for(int i=0; i<30*10; i++)
	{
		Channel *ch = chanMgr->findChannelByID(info.id);
		if (!ch)
			return false;

		if (ch->isPlaying() && (ch->rawData.writePos>0))
			return true;

		if (!thread.active || !sock->active())
			break;
		sys->sleep(100);
	}
	return false;
}
// -----------------------------------
void Servent::sendRawChannel(bool sendHead, bool sendData)
{
	try
	{

		sock->setWriteTimeout(DIRECT_WRITE_TIMEOUT*1000);

		Channel *ch = chanMgr->findChannelByID(chanID);
		if (!ch)
			throw StreamException("Channel not found");

		setStatus(S_CONNECTED);

		LOG_DEBUG("Starting Raw stream of %s at %d",ch->info.name.cstr(),streamPos);

		if (sendHead)
		{
			ch->headPack.writeRaw(*sock);
			streamPos = ch->headPack.pos + ch->headPack.len;
			LOG_DEBUG("Sent %d bytes header ",ch->headPack.len);
		}

		if (sendData)
		{

			unsigned int streamIndex = ch->streamIndex;
			unsigned int connectTime = sys->getTime();
			unsigned int lastWriteTime = connectTime;

			while ((thread.active) && sock->active())
			{
				ch = chanMgr->findChannelByID(chanID);

				if (ch)
				{

					if (streamIndex != ch->streamIndex)
					{
						streamIndex = ch->streamIndex;
						streamPos = ch->headPack.pos;
						LOG_DEBUG("sendRaw got new stream index");
					}

					ChanPacket rawPack;
					if (ch->rawData.findPacket(streamPos,rawPack))
					{
						if (syncPos != rawPack.sync)
							LOG_ERROR("Send skip: %d",rawPack.sync-syncPos);
						syncPos = rawPack.sync+1;

						if ((rawPack.type == ChanPacket::T_DATA) || (rawPack.type == ChanPacket::T_HEAD))
						{
							rawPack.writeRaw(*sock);
							lastWriteTime = sys->getTime();
						}

						if (rawPack.pos < streamPos)
							LOG_DEBUG("raw: skip back %d",rawPack.pos-streamPos);
						streamPos = rawPack.pos+rawPack.len;
					} else if (sock->readReady()) {
						char c;
						int error = sock->readUpto(&c, 1);
						if (error == 0) sock->close();
					}
				}

				if ((sys->getTime()-lastWriteTime) > DIRECT_WRITE_TIMEOUT)
					throw TimeoutException();

				sys->sleepIdle();
			}
		}
	}catch(StreamException &e)
	{
		LOG_ERROR("Stream channel: %s",e.msg);
	}
}

#if 0
// -----------------------------------
void Servent::sendRawMultiChannel(bool sendHead, bool sendData)
{
	try
	{
		unsigned int chanStreamIndex[ChanMgr::MAX_CHANNELS];
		unsigned int chanStreamPos[ChanMgr::MAX_CHANNELS];
		GnuID chanIDs[ChanMgr::MAX_CHANNELS];
		int numChanIDs=0;
		for(int i=0; i<ChanMgr::MAX_CHANNELS; i++)
		{
			Channel *ch = &chanMgr->channels[i];
			if (ch->isPlaying())
				chanIDs[numChanIDs++]=ch->info.id;
		}



		setStatus(S_CONNECTED);


		if (sendHead)
		{
			for(int i=0; i<numChanIDs; i++)
			{
				Channel *ch = chanMgr->findChannelByID(chanIDs[i]);
				if (ch)
				{
					LOG_DEBUG("Starting RawMulti stream: %s",ch->info.name.cstr());
					ch->headPack.writeRaw(*sock);
					chanStreamPos[i] = ch->headPack.pos + ch->headPack.len;
					chanStreamIndex[i] = ch->streamIndex;
					LOG_DEBUG("Sent %d bytes header",ch->headPack.len);

				}
			}
		}

		if (sendData)
		{

			unsigned int connectTime=sys->getTime();

			while ((thread.active) && sock->active())
			{

				for(int i=1; i<numChanIDs; i++)
				{
					Channel *ch = chanMgr->findChannelByID(chanIDs[i]);
					if (ch)
					{
						if (chanStreamIndex[i] != ch->streamIndex)
						{
							chanStreamIndex[i] = ch->streamIndex;
							chanStreamPos[i] = ch->headPack.pos;
							LOG_DEBUG("sendRawMulti got new stream index for chan %d",i);
						}

						ChanPacket rawPack;
						if (ch->rawData.findPacket(chanStreamPos[i],rawPack))
						{
							if ((rawPack.type == ChanPacket::T_DATA) || (rawPack.type == ChanPacket::T_HEAD))
								rawPack.writeRaw(*sock);


							if (rawPack.pos < chanStreamPos[i])
								LOG_DEBUG("raw: skip back %d",rawPack.pos-chanStreamPos[i]);
							chanStreamPos[i] = rawPack.pos+rawPack.len;


							//LOG("raw at %d: %d %d",streamPos,ch->rawData.getStreamPos(ch->rawData.firstPos),ch->rawData.getStreamPos(ch->rawData.lastPos));
						}						
					}
					break;
				}
				

				sys->sleepIdle();
			}
		}
	}catch(StreamException &e)
	{
		LOG_ERROR("Stream channel: %s",e.msg);
	}
}
#endif

// -----------------------------------
void Servent::sendRawMetaChannel(int interval)
{

	try
	{
		Channel *ch = chanMgr->findChannelByID(chanID);
		if (!ch)
			throw StreamException("Channel not found");

		sock->setWriteTimeout(DIRECT_WRITE_TIMEOUT*1000);

		setStatus(S_CONNECTED);

		LOG_DEBUG("Starting Raw Meta stream of %s (metaint: %d) at %d",ch->info.name.cstr(),interval,streamPos);


		String lastTitle,lastURL;

		int		lastMsgTime=sys->getTime();
		bool	showMsg=true;

		char buf[16384];
		int bufPos=0;

		if ((interval > sizeof(buf)) || (interval < 1))
			throw StreamException("Bad ICY Meta Interval value");

		unsigned int connectTime = sys->getTime();
		unsigned int lastWriteTime = connectTime;

		streamPos = 0;		// raw meta channel has no header (its MP3)

		while ((thread.active) && sock->active())
		{
			ch = chanMgr->findChannelByID(chanID);

			if (ch)
			{

				ChanPacket rawPack;
				if (ch->rawData.findPacket(streamPos,rawPack))
				{

					if (syncPos != rawPack.sync)
						LOG_ERROR("Send skip: %d",rawPack.sync-syncPos);
					syncPos = rawPack.sync+1;

					MemoryStream mem(rawPack.data,rawPack.len);

					if (rawPack.type == ChanPacket::T_DATA)
					{

						int len = rawPack.len;
						char *p = rawPack.data;
						while (len)
						{
							int rl = len;
							if ((bufPos+rl) > interval)
								rl = interval-bufPos;
							memcpy(&buf[bufPos],p,rl);
							bufPos+=rl;
							p+=rl;
							len-=rl;

							if (bufPos >= interval)
							{
								bufPos = 0;	
								sock->write(buf,interval);
								lastWriteTime = sys->getTime();

								if (chanMgr->broadcastMsgInterval)
									if ((sys->getTime()-lastMsgTime) >= chanMgr->broadcastMsgInterval)
									{
										showMsg ^= true;
										lastMsgTime = sys->getTime();
									}

								String *metaTitle = &ch->info.track.title;
								if (!ch->info.comment.isEmpty() && (showMsg))
									metaTitle = &ch->info.comment;


								if (!metaTitle->isSame(lastTitle) || !ch->info.url.isSame(lastURL))
								{

									char tmp[1024];
									String title,url;

									title = *metaTitle;
									url = ch->info.url;

									title.convertTo(String::T_META);
									url.convertTo(String::T_META);

									sprintf(tmp,"StreamTitle='%s';StreamUrl='%s';\0",title.cstr(),url.cstr());
									int len = ((strlen(tmp) + 15+1) / 16);
									sock->writeChar(len);
									sock->write(tmp,len*16);

									lastTitle = *metaTitle;
									lastURL = ch->info.url;

									LOG_DEBUG("StreamTitle: %s, StreamURL: %s",lastTitle.cstr(),lastURL.cstr());

								}else
								{
									sock->writeChar(0);					
								}

							}
						}
					}
					streamPos = rawPack.pos + rawPack.len;
				}
			}
			if ((sys->getTime()-lastWriteTime) > DIRECT_WRITE_TIMEOUT)
				throw TimeoutException();

			sys->sleepIdle();

		}
	}catch(StreamException &e)
	{
		LOG_ERROR("Stream channel: %s",e.msg);
	}
}
// -----------------------------------
void Servent::sendPeercastChannel()
{
	try
	{
		setStatus(S_CONNECTED);

		Channel *ch = chanMgr->findChannelByID(chanID);
		if (!ch)
			throw StreamException("Channel not found");

		LOG_DEBUG("Starting PeerCast stream: %s",ch->info.name.cstr());

		sock->writeTag("PCST");

		ChanPacket pack;

		ch->headPack.writePeercast(*sock);

		pack.init(ChanPacket::T_META,ch->insertMeta.data,ch->insertMeta.len,ch->streamPos);
		pack.writePeercast(*sock);
	
		streamPos = 0;
		unsigned int syncPos=0;
		while ((thread.active) && sock->active())
		{
			ch = chanMgr->findChannelByID(chanID);
			if (ch)
			{

				ChanPacket rawPack;
				if (ch->rawData.findPacket(streamPos,rawPack))
				{
					if ((rawPack.type == ChanPacket::T_DATA) || (rawPack.type == ChanPacket::T_HEAD))
					{
						sock->writeTag("SYNC");
						sock->writeShort(4);
						sock->writeShort(0);
						sock->write(&syncPos,4);
						syncPos++;

						rawPack.writePeercast(*sock);
					}
					streamPos = rawPack.pos + rawPack.len;
				}
			}
			sys->sleepIdle();
		}

	}catch(StreamException &e)
	{
		LOG_ERROR("Stream channel: %s",e.msg);
	}
}

//WLock canStreamLock;

// -----------------------------------
void Servent::sendPCPChannel()
{
	bool skipCheck = false;
	unsigned int ptime = 0;
	int npacket = 0, upsize = 0;

	Channel *ch = chanMgr->findChannelByID(chanID);
	if (!ch)
		throw StreamException("Channel not found");

	AtomStream atom(*sock);

	pcpStream = new PCPStream(remoteID);
	int error=0;

	try
	{

		LOG_DEBUG("Starting PCP stream of channel at %d",streamPos);


//		setStatus(S_CONNECTED);

		//canStreamLock.on();
		//thread.active = canStream(ch);
		//setStatus(S_CONNECTED);
		//canStreamLock.off();

		lastSkipTime = 0;
		lastSkipCount = 0;
		waitPort = 0;

		if (thread.active){
			atom.writeParent(PCP_CHAN,3 + ((sendHeader)?1:0));
				atom.writeBytes(PCP_CHAN_ID,chanID.id,16);
				ch->info.writeInfoAtoms(atom);
				ch->info.writeTrackAtoms(atom);
				if (sendHeader)
				{
					atom.writeParent(PCP_CHAN_PKT,3);
						atom.writeID4(PCP_CHAN_PKT_TYPE,PCP_CHAN_PKT_HEAD);
						atom.writeInt(PCP_CHAN_PKT_POS,ch->headPack.pos);
						atom.writeBytes(PCP_CHAN_PKT_DATA,ch->headPack.data,ch->headPack.len);

					if (streamPos == 0)
						streamPos = ch->headPack.pos+ch->headPack.len;
					LOG_DEBUG("Sent %d bytes header",ch->headPack.len);
				}
		}

		unsigned int streamIndex = ch->streamIndex;

		ChanPacket rawPack;
		char pbuf[ChanPacket::MAX_DATALEN*3];
		MemoryStream mems(pbuf,sizeof(pbuf));
		AtomStream atom2(mems);

		while (thread.active)
		{

			Channel *ch = chanMgr->findChannelByID(chanID);

			if (ch)
			{

				if (streamIndex != ch->streamIndex)
				{
					streamIndex = ch->streamIndex;
					streamPos = ch->headPack.pos;
					LOG_DEBUG("sendPCPStream got new stream index");						
				}

				mems.rewind();

				if (ch->rawData.findPacket(streamPos,rawPack))
				{
					if ((streamPos < rawPack.pos) && !rawPack.skip){
						if (skipCheck){
							char tmp[32];
							getHost().IPtoStr(tmp);
							LOG_NETWORK("##### send skipping ##### %d (%d, %d) -> %s", (rawPack.pos - streamPos), streamPos, rawPack.pos, tmp);

							if (sys->getTime() == lastSkipTime) {
								LOG_DEBUG("##### skip all buffer");
								streamPos = ch->rawData.getLatestPos();
								continue;
							}

							lastSkipTime = sys->getTime();
							lastSkipCount++;
						} else {
							skipCheck = true;
						}
					}

					if (rawPack.type == ChanPacket::T_HEAD)
					{
						atom2.writeParent(PCP_CHAN,2);
							atom2.writeBytes(PCP_CHAN_ID,chanID.id,16);
							atom2.writeParent(PCP_CHAN_PKT,3);
								atom2.writeID4(PCP_CHAN_PKT_TYPE,PCP_CHAN_PKT_HEAD);
								atom2.writeInt(PCP_CHAN_PKT_POS,rawPack.pos);
								atom2.writeBytes(PCP_CHAN_PKT_DATA,rawPack.data,rawPack.len);

						sock->write(pbuf, mems.getPosition());
					}else if (rawPack.type == ChanPacket::T_DATA)
					{
						atom2.writeParent(PCP_CHAN,2);
							atom2.writeBytes(PCP_CHAN_ID,chanID.id,16);
							atom2.writeParent(PCP_CHAN_PKT,3);
								atom2.writeID4(PCP_CHAN_PKT_TYPE,PCP_CHAN_PKT_DATA);
								atom2.writeInt(PCP_CHAN_PKT_POS,rawPack.pos);
								atom2.writeBytes(PCP_CHAN_PKT_DATA,rawPack.data,rawPack.len);

#ifdef NULL//WIN32
						sock->bufferingWrite(pbuf, mems.getPosition());
						lastSkipTime = sock->bufList.lastSkipTime;
						lastSkipCount = sock->bufList.skipCount;
#else
						sock->write(pbuf, mems.getPosition());
#endif
					}

					if (rawPack.pos < streamPos)
						LOG_DEBUG("pcp: skip back %d",rawPack.pos-streamPos);

					//LOG_DEBUG("Sending %d-%d (%d,%d,%d)",rawPack.pos,rawPack.pos+rawPack.len,ch->streamPos,ch->rawData.getLatestPos(),ch->rawData.getOldestPos());

					streamPos = rawPack.pos+rawPack.len;
				}
			} else {
				throw StreamException("Channel not found");
			}

#ifdef NULL//WIN32
			sock->bufferingWrite(NULL, 0);
			lastSkipTime = sock->bufList.lastSkipTime;
			lastSkipCount = sock->bufList.skipCount;
#endif
			BroadcastState bcs;
			bcs.servent_id = servent_id;
//			error = pcpStream->readPacket(*sock,bcs);

			unsigned int t = sys->getTime();
			if (t != ptime) {
				ptime = t;
				npacket = MAX_PROC_PACKETS;
				upsize = MAX_OUTWARD_SIZE;
			}

			int len = pcpStream->flushUb(*sock, upsize);
			upsize -= len;

			while (npacket > 0 && sock->readReady()) {
				npacket--;
				error = pcpStream->readPacket(*sock,bcs);
				if (error)
					throw StreamException("PCP exception");
			}

			sys->sleepIdle();

		}

		LOG_DEBUG("PCP channel stream closed normally.");

	}catch(StreamException &e)
	{
		LOG_ERROR("Stream channel: %s",e.msg);
	}

	try
	{
		pcpStream->flush(*sock);
		atom.writeInt(PCP_QUIT,error);
	}catch(StreamException &) {}

}

// -----------------------------------
int Servent::serverProc(ThreadInfo *thread)
{
//	thread->lock();


	Servent *sv = (Servent*)thread->data;

	try 
	{
		if (!sv->sock)
			throw StreamException("Server has no socket");

		sv->setStatus(S_LISTENING);


		char servIP[64];
		sv->sock->host.toStr(servIP);

		if (servMgr->isRoot)
			LOG_DEBUG("Root Server started: %s",servIP);
		else
			LOG_DEBUG("Server started: %s",servIP);
		

		while ((thread->active) && (sv->sock->active()))
		{
			if (servMgr->numActiveOnPort(sv->sock->host.port) < servMgr->maxServIn)
			{
				ClientSocket *cs = sv->sock->accept();
				if (cs)
				{	
					LOG_DEBUG("accepted incoming");
					Servent *ns = servMgr->allocServent();
					if (ns)
					{
						servMgr->lastIncoming = sys->getTime();
						ns->servPort = sv->sock->host.port;
						ns->networkID = servMgr->networkID;
						ns->initIncoming(cs,sv->allow);
					}else
						LOG_ERROR("Out of servents");
				}
			}
			sys->sleep(100);
		}
	}catch(StreamException &e)
	{
		LOG_ERROR("Server Error: %s:%d",e.msg,e.err);
	}

	
	LOG_DEBUG("Server stopped");

	sv->kill();
	sys->endThread(thread);
	return 0;
}
 
// -----------------------------------
bool	Servent::writeVariable(Stream &s, const String &var)
{
	char buf[1024];

	if (var == "type")
		strcpy(buf,getTypeStr());
	else if (var == "status")
		strcpy(buf,getStatusStr());
	else if (var == "address")
	{
		if (servMgr->enableGetName) //JP-EX s
		{
			getHost().toStr(buf);
			char h_ip[64];
			Host h = getHost();
			h.toStr(h_ip);

/*			ChanHitList *hits[ChanMgr::MAX_HITLISTS];
			int numHits=0;
			for(int i=0; i<ChanMgr::MAX_HITLISTS; i++)
			{
				ChanHitList *chl = &chanMgr->hitlists[i];
				if (chl->isUsed())
					hits[numHits++] = chl;
			}
			bool ishit,isfw;
			ishit = isfw = false;
			int numRelay = 0;
			if (numHits) 
			{
				for(int k=0; k<numHits; k++)
				{
					ChanHitList *chl = hits[k];
					if (chl->isUsed())
					{
						for (int j=0; j<ChanHitList::MAX_HITS; j++)
						{
							ChanHit *hit = &chl->hits[j];
							if (hit->host.isValid() && (h.ip == hit->host.ip))
							{
								ishit = true;
								if (hit->firewalled)
									isfw = true;
								numRelay += hit->numRelays;
							}
						}
					}
				}
			}
			strcpy(buf,"");
			if (ishit == true)
			{
				if (isfw == true)
				{
					if (numRelay== 0)
						strcat(buf,"<font color=red>");
					else 
						strcat(buf,"<font color=orange>");
				}
				else
					strcat(buf,"<font color=green>");
			}
			strcat(buf,h_ip);
			char h_name[128];
			if (ClientSocket::getHostname(h_name,h.ip))
			{
				strcat(buf,"[");
				strcat(buf,h_name);
				strcat(buf,"]");
			}
			if (ishit == true) 
			{
				strcat(buf,"</font>");
			}
		} //JP-EX e*/


			bool isfw = false;
			bool isRelay = true;
			int numRelay = 0;
			ChanHitList *chl = chanMgr->findHitListByID(chanID);
			if (chl){
				ChanHit *hit = chl->hit;
				while(hit){
					if (hit->host.isValid() && (h.ip == hit->host.ip)){
						isfw = hit->firewalled;
						isRelay = hit->relay;
						numRelay = hit->numRelays;
						break;
					}
					hit = hit->next;
				}
			}
			strcpy(buf, "");
			if (isfw){
				if (numRelay == 0){
					strcat(buf,"<font color=red>");
				} else {
					strcat(buf,"<font color=orange>");
				}
			} else {
				if (!isRelay){
					if (numRelay==0){
						strcpy(buf,"<font color=purple>");
					} else {
						strcpy(buf,"<font color=blue>");
					}
				} else {
					strcpy(buf,"<font color=green>");
				}
			}
			strcat(buf,h_ip);
			char h_name[128];
			if (ClientSocket::getHostname(h_name,sizeof(h_name),h.ip)) //JP-MOD
			{
				strcat(buf,"[");
				strcat(buf,h_name);
				strcat(buf,"]");
			}
			strcat(buf,"</font>");
		}
		else 
			getHost().toStr(buf);
	}
	else if (var == "agent")
		strcpy(buf,agent.cstr());
	else if (var == "bitrate")
	{
		if (sock)
		{
			unsigned int tot = sock->bytesInPerSec+sock->bytesOutPerSec;
			sprintf(buf,"%.1f",BYTES_TO_KBPS(tot));
		}else
			strcpy(buf,"0");
	}else if (var == "uptime")
	{
		String uptime;
		if (lastConnect)
			uptime.setFromStopwatch(sys->getTime()-lastConnect);
		else
			uptime.set("-");
		strcpy(buf,uptime.cstr());
	}else if (var.startsWith("gnet."))
	{

		float ctime = (float)(sys->getTime()-lastConnect);
		if (var == "gnet.packetsIn")
			sprintf(buf,"%d",gnuStream.packetsIn);
		else if (var == "gnet.packetsInPerSec")
			sprintf(buf,"%.1f",ctime>0?((float)gnuStream.packetsIn)/ctime:0);
		else if (var == "gnet.packetsOut")
			sprintf(buf,"%d",gnuStream.packetsOut);
		else if (var == "gnet.packetsOutPerSec")
			sprintf(buf,"%.1f",ctime>0?((float)gnuStream.packetsOut)/ctime:0);
		else if (var == "gnet.normQueue")
			sprintf(buf,"%d",outPacketsNorm.numPending());
		else if (var == "gnet.priQueue")
			sprintf(buf,"%d",outPacketsPri.numPending());
		else if (var == "gnet.flowControl")
			sprintf(buf,"%d",flowControl?1:0);
		else if (var == "gnet.routeTime")
		{
			int nr = seenIDs.numUsed();
			unsigned int tim = sys->getTime()-seenIDs.getOldest();
		
			String tstr;
			tstr.setFromStopwatch(tim);

			if (nr)
				strcpy(buf,tstr.cstr());
			else
				strcpy(buf,"-");
		}
		else
			return false;

	}else
		return false;

	s.writeString(buf);

	return true;
}
