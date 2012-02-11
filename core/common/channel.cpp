// ------------------------------------------------
// File : channel.cpp
// Date: 4-apr-2002
// Author: giles
// Desc: 
//		Channel streaming classes. These do the actual 
//		streaming of media between clients. 
//
// (c) 2002 peercast.org
// 
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

#include <string.h>
#include <stdlib.h>
#include "common.h"
#include "socket.h"
#include "channel.h"
#include "gnutella.h"
#include "servent.h"
#include "servmgr.h"
#include "sys.h"
#include "xml.h"
#include "http.h"
#include "peercast.h"
#include "atom.h"
#include "pcp.h"

#include "mp3.h"
#include "ogg.h"
#include "mms.h"
#include "nsv.h"

#include "icy.h"
#include "url.h"

#include "version2.h"
#ifdef _DEBUG
#include "chkMemoryLeak.h"
#define DEBUG_NEW new(__FILE__, __LINE__)
#define new DEBUG_NEW
#endif

// -----------------------------------
char *Channel::srcTypes[]=
{
	"NONE",
	"PEERCAST",
	"SHOUTCAST",
	"ICECAST",
	"URL"
};
// -----------------------------------
char *Channel::statusMsgs[]=
{
	"NONE",
	"WAIT",
	"CONNECT",
	"REQUEST",
	"CLOSE",
	"RECEIVE",
	"BROADCAST",
	"ABORT",
	"SEARCH",
	"NOHOSTS",
	"IDLE",
	"ERROR",
	"NOTFOUND"
};


// for PCRaw start.
bool isIndexTxt(ChanInfo *info)
{
	size_t len;

	if(	info &&
		info->contentType == ChanInfo::T_RAW &&
		info->bitrate <= 32 &&
		(len = strlen(info->name.cstr())) >= 9 &&
		!memcmp(info->name.cstr(), "index", 5) &&
		!memcmp(info->name.cstr()+len-4, ".txt", 4))
	{
		return true;
	}
	else
	{
		return false;
	}
}

bool isIndexTxt(Channel *ch)
{
	if(ch && !ch->isBroadcasting() && isIndexTxt(&ch->info))
		return true;
	else
		return false;
}

int numMaxRelaysIndexTxt(Channel *ch)
{
	return ((servMgr->maxRelaysIndexTxt < 1) ? 1 : servMgr->maxRelaysIndexTxt);
}

int canStreamIndexTxt(Channel *ch)
{
	int ret;

	// Ž©•ª‚ª”zM‚µ‚Ä‚¢‚éê‡‚ÍŠÖŒW‚È‚¢
	if(!ch || ch->isBroadcasting())
		return -1;

	ret = numMaxRelaysIndexTxt(ch) - ch->localRelays();
	if(ret < 0)
		ret = 0;

	return ret;
}
// for PCRaw end.

// -----------------------------------
void readXMLString(String &str, XML::Node *n, const char *arg)
{
	char *p;
	p = n->findAttr(arg);
	if (p)
	{
		str.set(p,String::T_HTML);
		str.convertTo(String::T_ASCII);
	}
}

int channel_count=1;
// -----------------------------------------------------------------------------
// Initialise the channel to its default settings of unallocated and reset.
// -----------------------------------------------------------------------------
Channel::Channel()
{
	next = NULL;
	reset();
	channel_id = channel_count++;
}
// -----------------------------------------------------------------------------
void Channel::endThread(bool flg)
{
	if (pushSock)
	{
		pushSock->close();
		delete pushSock;
		pushSock = NULL;
	}

	if (sock)
	{
		sock->close();
		sock = NULL;
	}

	if (sourceData)
	{
		delete sourceData;
		sourceData = NULL;
	}

	if (flg == true){
		reset();

		chanMgr->channellock.on();
		chanMgr->deleteChannel(this);
		chanMgr->channellock.off();

		sys->endThread(&thread);
	}
}
// -----------------------------------------------------------------------------
void Channel::resetPlayTime()
{
	info.lastPlayStart = sys->getTime();
}
// -----------------------------------------------------------------------------
void Channel::setStatus(STATUS s)
{
	if (s != status)
	{
		bool wasPlaying = isPlaying();

		status = s;

		if (isPlaying())
		{
			info.status = ChanInfo::S_PLAY;
			resetPlayTime();
		}else
		{
			if (wasPlaying)
				info.lastPlayEnd = sys->getTime();
			info.status = ChanInfo::S_UNKNOWN;
		}

		if (isBroadcasting())
		{
			ChanHitList *chl = chanMgr->findHitListByID(info.id);
			if (!chl)
				chanMgr->addHitList(info);
		}

		peercastApp->channelUpdate(&info);

	}
}
	
// -----------------------------------------------------------------------------
// Reset channel and make it available 
// -----------------------------------------------------------------------------
void Channel::reset()
{
	sourceHost.init();
	remoteID.clear();

	streamIndex = 0;

	lastIdleTime = 0;

	info.init();

	mount.clear();
	bump = false;
	stayConnected = false;
	stealth = false; //JP-MOD
	overrideMaxRelaysPerChannel = -1; //JP-MOD
	bClap = false; //JP-MOD

	icyMetaInterval = 0;
	streamPos = 0;
	skipCount = 0; //JP-EX
	lastSkipTime = 0;

	insertMeta.init();

	headPack.init();

	sourceStream = NULL;

	rawData.init();
	rawData.accept = ChanPacket::T_HEAD|ChanPacket::T_DATA;

	setStatus(S_NONE);
	type = T_NONE;

	readDelay = false;
	sock = NULL;
	pushSock = NULL;

	sourceURL.clear();
	sourceData = NULL;

	lastTrackerUpdate = 0;
	lastMetaUpdate = 0;

	srcType = SRC_NONE;


	startTime = 0;
	syncTime = 0;

	channel_id = 0;
	finthread = NULL;

	trackerHit.init();
}

// -----------------------------------
void	Channel::newPacket(ChanPacket &pack)
{
	if (pack.type != ChanPacket::T_PCP)
	{
		rawData.writePacket(pack,true);
	}
}


// -----------------------------------
bool	Channel::checkIdle()
{
	return ( (info.getUptime() > chanMgr->prefetchTime) && (localListeners() == 0) && (!stayConnected) && (status != S_BROADCASTING));
}

// -----------------------------------
bool	Channel::isFull()
{
	if (overrideMaxRelaysPerChannel > 0){ //JP-MOD ŒÂ•Ê‚ÉƒŠƒŒ[”Ý’è
		return localRelays() >= overrideMaxRelaysPerChannel;
	}

	// for PCRaw (relay) start.
	if(isIndexTxt(this))
	{
		int ret = canStreamIndexTxt(this);
		
		if(ret > 0)
			return false;
		else if(ret == 0)
			return true;
	}
	// for PCRaw (relay) end.

	return chanMgr->maxRelaysPerChannel ? localRelays() >= chanMgr->maxRelaysPerChannel : false;
}
// -----------------------------------
int Channel::localRelays()
{
	return servMgr->numStreams(info.id,Servent::T_RELAY,true);
}
// -----------------------------------
int Channel::localListeners()
{
	return servMgr->numStreams(info.id,Servent::T_DIRECT,true);
}

// -----------------------------------
int Channel::totalRelays()
{
	int tot = 0;
	ChanHitList *chl = chanMgr->findHitListByID(info.id);
	if (chl)
		tot += chl->numHits();
	return tot;
}
// -----------------------------------
int Channel::totalListeners()
{
	int tot = localListeners();
	ChanHitList *chl = chanMgr->findHitListByID(info.id);
	if (chl)
		tot += chl->numListeners();
	return tot;
}
// -----------------------------------
int Channel::totalClaps()	//JP-MOD
{
	ChanHitList *chl = chanMgr->findHitListByID(info.id);
	return chl ? chl->numClaps() : 0;
}

// -----------------------------------
void	Channel::startGet()
{
	srcType = SRC_PEERCAST;
	type = T_RELAY;
	info.srcProtocol = ChanInfo::SP_PCP;


	sourceData = new PeercastSource();

	startStream();
}
// -----------------------------------
void	Channel::startURL(const char *u)
{
	sourceURL.set(u);

	srcType = SRC_URL;
	type = T_BROADCAST;
	stayConnected = true;

	resetPlayTime();

	sourceData = new URLSource(u);

	startStream();

}
// -----------------------------------
void Channel::startStream()
{
	thread.data = this;
	thread.func = stream;
	if (!sys->startThread(&thread))
		reset();
}

// -----------------------------------
void Channel::sleepUntil(double time)
{
	double sleepTime = time - (sys->getDTime()-startTime);

//	LOG("sleep %g",sleepTime);
	if (sleepTime > 0)
	{
		if (sleepTime > 60) sleepTime = 60;

		double sleepMS = sleepTime*1000;

		sys->sleep((int)sleepMS);
	}
}


// -----------------------------------
void Channel::checkReadDelay(unsigned int len)
{
	if (readDelay)
	{
		unsigned int time = (len*1000)/((info.bitrate*1024)/8);
		sys->sleep(time);
	}


}


// -----------------------------------
THREAD_PROC	Channel::stream(ThreadInfo *thread)
{
//	thread->lock();

	Channel *ch = (Channel *)thread->data;

	LOG_CHANNEL("Channel started");

	while (thread->active && !peercastInst->isQuitting && !thread->finish)
	{
		ChanHitList *chl = chanMgr->findHitList(ch->info);
		if (!chl)
			chanMgr->addHitList(ch->info);

		ch->sourceData->stream(ch);

		LOG_CHANNEL("Channel stopped");
		ch->rawData.init();

		if (!ch->stayConnected)
		{
			thread->active = false;
			break;
		}else
		{
			if (!ch->info.lastPlayEnd)
				ch->info.lastPlayEnd = sys->getTime();

			unsigned int diff = (sys->getTime()-ch->info.lastPlayEnd) + 5;

			LOG_DEBUG("Channel sleeping for %d seconds",diff);
			for(unsigned int i=0; i<diff; i++)
			{
				if (ch->info.lastPlayEnd == 0) // reconnected
					break;
				if (!thread->active || peercastInst->isQuitting){
					thread->active = false;
					break;
				}
				sys->sleep(1000);	
			}
		}
	}

	LOG_DEBUG("thread.active = %d, thread.finish = %d",
		ch->thread.active, ch->thread.finish);

	if (!thread->finish){
		ch->endThread(false);

		if (!ch->finthread){
			ch->finthread = new ThreadInfo();
			ch->finthread->func = waitFinish;
			ch->finthread->data = ch;
			sys->startThread(ch->finthread);
		}
	} else {
		ch->endThread(true);
	}
	return 0;
}	

// -----------------------------------
THREAD_PROC Channel::waitFinish(ThreadInfo *thread)
{
	Channel *ch = (Channel*)thread->data;
	LOG_DEBUG("Wait channel finish");

	while(!(ch->thread.finish) && !thread->finish){
		sys->sleep(1000);
	}

	if (ch->thread.finish){
		LOG_DEBUG("channel finish");
		ch->endThread(true);
	} else {
		LOG_DEBUG("channel restart");
	}

	delete thread;
	return 0;
}

// -----------------------------------
bool Channel::acceptGIV(ClientSocket *givSock)
{
	if (!pushSock)
	{
		pushSock = givSock;
		return true;
	}else
		return false;
}
// -----------------------------------
void Channel::connectFetch()
{
	sock = sys->createSocket();
	
	if (!sock)
		throw StreamException("Can`t create socket");

	if (sourceHost.tracker || sourceHost.yp)
	{
		sock->setReadTimeout(30000);
		sock->setWriteTimeout(30000);
		LOG_CHANNEL("Channel using longer timeouts");
	} else {
		sock->setReadTimeout(5000);
		sock->setWriteTimeout(5000);
	}

	sock->open(sourceHost.host);
		
	sock->connect();
}

// -----------------------------------
int Channel::handshakeFetch()
{
	char idStr[64];
	info.id.toStr(idStr);

	char sidStr[64];
	servMgr->sessionID.toStr(sidStr);

	sock->writeLineF("GET /channel/%s HTTP/1.0",idStr);
	sock->writeLineF("%s %d",PCX_HS_POS,streamPos);
	sock->writeLineF("%s %d",PCX_HS_PCP,1);
	sock->writeLineF("%s %d",PCX_HS_PORT,servMgr->serverHost.port);

	sock->writeLine("");

	HTTP http(*sock);

	int r = http.readResponse();

	LOG_CHANNEL("Got response: %d",r);

	while (http.nextHeader())
	{
		char *arg = http.getArgStr();
		if (!arg) 
			continue;

		if (http.isHeader(PCX_HS_POS))
			streamPos = atoi(arg);
		else
			Servent::readICYHeader(http, info, NULL);

		LOG_CHANNEL("Channel fetch: %s",http.cmdLine);
	}

	if ((r != 200) && (r != 503))
		return r;

	if (!servMgr->keepDownstreams) {
		if (rawData.getLatestPos() > streamPos)
			rawData.init();
	}

	AtomStream atom(*sock);

	String agent;

	Host rhost = sock->host;

	if (info.srcProtocol == ChanInfo::SP_PCP)
	{
		// don`t need PCP_CONNECT here
		Servent::handshakeOutgoingPCP(atom,rhost,remoteID,agent,sourceHost.yp|sourceHost.tracker);
	}

	if (r == 503) return 503;
	return 0;

}

// -----------------------------------
void PeercastSource::stream(Channel *ch)
{
	int numYPTries=0;
	int numYPTries2=0;
	bool next_yp = false;
	bool tracker_check = (ch->trackerHit.host.ip != 0);
	int connFailCnt = 0;
	int keepDownstreamTime = 7;

	if (isIndexTxt(&ch->info)) keepDownstreamTime = 30;

	ch->lastStopTime = 0;
	ch->bumped = false;

	while (ch->thread.active)
	{
		ch->skipCount = 0; //JP-EX
		ch->lastSkipTime = 0;
		
		ChanHitList *chl = NULL;

		ch->sourceHost.init();

		if (connFailCnt >= 3 && (ch->localListeners() == 0) && (!ch->stayConnected) && !ch->isBroadcasting()) {
			ch->lastIdleTime = sys->getTime();
			ch->setStatus(Channel::S_IDLE);
			ch->skipCount = 0;
			ch->lastSkipTime = 0;
			break;
		}

		if (!servMgr->keepDownstreams && !ch->bumped) {
			ch->trackerHit.lastContact = sys->getTime() - 30 + (rand() % 30);
		}

		ch->setStatus(Channel::S_SEARCHING);
		LOG_CHANNEL("Channel searching for hit..");
		do 
		{
	
			if (ch->pushSock)
			{
				ch->sock = ch->pushSock;
				ch->pushSock = NULL;
				ch->sourceHost.host = ch->sock->host;
				break;
			}

			chanMgr->hitlistlock.on();

			chl = chanMgr->findHitList(ch->info);
			if (chl)
			{
				ChanHitSearch chs;

				// find local hit 
				if (!ch->sourceHost.host.ip){
					chs.init();
					chs.matchHost = servMgr->serverHost;
					chs.waitDelay = MIN_RELAY_RETRY;
					chs.excludeID = servMgr->sessionID;
					if (chl->pickSourceHits(chs)){
						ch->sourceHost = chs.best[0];
						LOG_DEBUG("use local hit");
					}
				}
				
				// else find global hit
				if (!ch->sourceHost.host.ip)
				{
					chs.init();
					chs.waitDelay = MIN_RELAY_RETRY;
					chs.excludeID = servMgr->sessionID;
					if (chl->pickSourceHits(chs)){
						ch->sourceHost = chs.best[0];
						LOG_DEBUG("use global hit");
					}
				}


				// else find local tracker
				if (!ch->sourceHost.host.ip)
				{
					chs.init();
					chs.matchHost = servMgr->serverHost;
					chs.waitDelay = MIN_TRACKER_RETRY;
					chs.excludeID = servMgr->sessionID;
					chs.trackersOnly = true;
					if (chl->pickSourceHits(chs)){
						ch->sourceHost = chs.best[0];
						LOG_DEBUG("use local tracker");
					}
				}

				// else find global tracker
				if (!ch->sourceHost.host.ip)
				{
					chs.init();
					chs.waitDelay = MIN_TRACKER_RETRY;
					chs.excludeID = servMgr->sessionID;
					chs.trackersOnly = true;
					if (chl->pickSourceHits(chs)){
						ch->sourceHost = chs.best[0];
						tracker_check = true;
						ch->trackerHit = chs.best[0];
						LOG_DEBUG("use global tracker");
					}
				}
				// find tracker
				unsigned int ctime = sys->getTime();
				if (!ch->sourceHost.host.ip && tracker_check && ch->trackerHit.host.ip){
					if (ch->trackerHit.lastContact + 30 < ctime){
						ch->sourceHost = ch->trackerHit;
						ch->trackerHit.lastContact = ctime;
						LOG_DEBUG("use saved tracker");
					}
				}
			}

			chanMgr->hitlistlock.off();

			if (servMgr->keepDownstreams && ch->lastStopTime
				&& ch->lastStopTime < sys->getTime() - keepDownstreamTime) {
				ch->lastStopTime = 0;
				LOG_DEBUG("------------ disconnect all downstreams");
				ChanPacket pack;
				MemoryStream mem(pack.data,sizeof(pack.data));
				AtomStream atom(mem);
				atom.writeInt(PCP_QUIT,PCP_ERROR_QUIT+PCP_ERROR_OFFAIR);
				pack.len = mem.pos;
				pack.type = ChanPacket::T_PCP;
				GnuID noID;
				noID.clear();
				servMgr->broadcastPacket(pack,ch->info.id,ch->remoteID,noID,Servent::T_RELAY);

				chanMgr->hitlistlock.on();
				ChanHitList *hl = chanMgr->findHitList(ch->info);
				if (hl){
					hl->clearHits(false);
				}
				chanMgr->hitlistlock.off();
			}

			// no trackers found so contact YP
			if (!tracker_check && !ch->sourceHost.host.ip)
			{
				next_yp = false;
				if (servMgr->rootHost.isEmpty())
					goto yp2;

				if (numYPTries >= 3)
					goto yp2;

				if  ((!servMgr->rootHost2.isEmpty()) && (numYPTries > numYPTries2))
					goto yp2;

				unsigned int ctime=sys->getTime();
				if ((ctime-chanMgr->lastYPConnect) > MIN_YP_RETRY)
				{
					ch->sourceHost.host.fromStrName(servMgr->rootHost.cstr(),DEFAULT_PORT);
					ch->sourceHost.yp = true;
					chanMgr->lastYPConnect=ctime;
					numYPTries++;
				}
				if (numYPTries < 3)
					next_yp = true;
			}

yp2:
			// no trackers found so contact YP2
			if (!tracker_check && !ch->sourceHost.host.ip)
			{
//				next_yp = false;
				if (servMgr->rootHost2.isEmpty())
					goto yp0;

				if (numYPTries2 >= 3)
					goto yp0;

				unsigned int ctime=sys->getTime();
				if ((ctime-chanMgr->lastYPConnect2) > MIN_YP_RETRY)
				{
					ch->sourceHost.host.fromStrName(servMgr->rootHost2.cstr(),DEFAULT_PORT);
					ch->sourceHost.yp = true;
					chanMgr->lastYPConnect2=ctime;
					numYPTries2++;
				}
				if (numYPTries2 < 3)
					next_yp = true;
			}
yp0:
			if (!tracker_check && !ch->sourceHost.host.ip && !next_yp) break;

			sys->sleepIdle();

		}while((ch->sourceHost.host.ip==0) && (ch->thread.active));

		if (!ch->sourceHost.host.ip)
		{
			LOG_ERROR("Channel giving up");
			ch->setStatus(Channel::S_ERROR);			
			break;
		}

		if (ch->sourceHost.yp)
		{
			LOG_CHANNEL("Channel contacting YP, try %d",numYPTries);
		}else
		{
			LOG_CHANNEL("Channel found hit");
			numYPTries=0;
			numYPTries2=0;
		}

		if (ch->sourceHost.host.ip)
		{
			bool isTrusted = ch->sourceHost.tracker | ch->sourceHost.yp;


			//if (ch->sourceHost.tracker)
			//	peercastApp->notifyMessage(ServMgr::NT_PEERCAST,"Contacting tracker, please wait...");
			
			char ipstr[64];
			ch->sourceHost.host.toStr(ipstr);

			char *type = "";
			if (ch->sourceHost.tracker)
				type = "(tracker)";
			else if (ch->sourceHost.yp)
				type = "(YP)";

			int error=-1;
			int got503 = 0;
			try
			{
				ch->setStatus(Channel::S_CONNECTING);
				ch->sourceHost.lastContact = sys->getTime();

				if (!ch->sock)
				{
					LOG_CHANNEL("Channel connecting to %s %s",ipstr,type);
					ch->connectFetch();
				}

				error = ch->handshakeFetch();
				if (error == 503) {
					got503 = 1;
					error = 0;
				}
				if (error)
					throw StreamException("Handshake error");
				if (ch->sourceHost.tracker) connFailCnt = 0;

				if (servMgr->autoMaxRelaySetting) //JP-EX
				{	
					double setMaxRelays = ch->info.bitrate?servMgr->maxBitrateOut/(ch->info.bitrate*1.3):0;
					if ((unsigned int)setMaxRelays == 0)
						servMgr->maxRelays = 1;
					else if ((unsigned int)setMaxRelays > servMgr->autoMaxRelaySetting)
						servMgr->maxRelays = servMgr->autoMaxRelaySetting;
					else
						servMgr->maxRelays = (unsigned int)setMaxRelays;
				}

				ch->sourceStream = ch->createSource();

				error = ch->readStream(*ch->sock,ch->sourceStream);
				if (error)
					throw StreamException("Stream error");

				error = 0;		// no errors, closing normally.
//				ch->setStatus(Channel::S_CLOSING);			
				ch->setStatus(Channel::S_IDLE);

				LOG_CHANNEL("Channel closed normally");
			}catch(StreamException &e)
			{
				ch->setStatus(Channel::S_ERROR);			
				LOG_ERROR("Channel to %s %s : %s",ipstr,type,e.msg);
				if (!servMgr->allowConnectPCST) //JP-EX
				{
					if (ch->info.srcProtocol == ChanInfo::SP_PEERCAST)
						ch->thread.active = false;
				}
				//if (!ch->sourceHost.tracker || ((error != 503) && ch->sourceHost.tracker))
				if (!ch->sourceHost.tracker || (!got503 && ch->sourceHost.tracker))
					chanMgr->deadHit(ch->sourceHost);
				if (ch->sourceHost.tracker && error == -1) {
					LOG_ERROR("can't connect to tracker");
					connFailCnt++;
				}
			}

			unsigned int ctime = sys->getTime();
			if (ch->rawData.lastWriteTime) {
				ch->lastStopTime = ch->rawData.lastWriteTime;
				if (isIndexTxt(ch) && ctime - ch->lastStopTime < 60)
					ch->lastStopTime = ctime;
			}

			if (tracker_check && ch->sourceHost.tracker)
				ch->trackerHit.lastContact = ctime - 30 + (rand() % 30);

			// broadcast source host
			if (!got503 && !error && ch->sourceHost.host.ip) { // if closed normally
				ChanPacket pack;
				MemoryStream mem(pack.data,sizeof(pack.data));
				AtomStream atom(mem);
				ch->sourceHost.writeAtoms(atom, ch->info.id);
				pack.len = mem.pos;
				pack.type = ChanPacket::T_PCP;
				GnuID noID;
				noID.clear();
				servMgr->broadcastPacket(pack,ch->info.id,ch->remoteID,noID,Servent::T_RELAY);
				LOG_DEBUG("stream: broadcast sourceHost");
			}

			// broadcast quit to any connected downstream servents
			if (!servMgr->keepDownstreams || !got503 && (ch->sourceHost.tracker || !error)) {
				ChanPacket pack;
				MemoryStream mem(pack.data,sizeof(pack.data));
				AtomStream atom(mem);
				atom.writeInt(PCP_QUIT,PCP_ERROR_QUIT+PCP_ERROR_OFFAIR);
				pack.len = mem.pos;
				pack.type = ChanPacket::T_PCP;
				GnuID noID;
				noID.clear();
				servMgr->broadcastPacket(pack,ch->info.id,ch->remoteID,noID,Servent::T_RELAY);
				LOG_DEBUG("------------ broadcast quit to all downstreams");

				chanMgr->hitlistlock.on();
				ChanHitList *hl = chanMgr->findHitList(ch->info);
				if (hl){
					hl->clearHits(false);
				}
				chanMgr->hitlistlock.off();
			}


			if (ch->sourceStream)
			{
				try
				{
					if (!error)
					{
						ch->sourceStream->updateStatus(ch);
						ch->sourceStream->flush(*ch->sock);
					}
				}catch(StreamException &)
				{}
				ChannelStream *cs = ch->sourceStream;
				ch->sourceStream = NULL;
				cs->kill();
				delete cs;
			}

			if (ch->sock)
			{
				ch->sock->close();
				delete ch->sock;
				ch->sock = NULL;
			}

			if (error == 404)
			{
				LOG_ERROR("Channel not found");
				//if (!next_yp){
				if ((ch->sourceHost.yp && !next_yp) || ch->sourceHost.tracker) {
					chanMgr->hitlistlock.on();
					ChanHitList *hl = chanMgr->findHitList(ch->info);
					if (hl){
						hl->clearHits(true);
					}
					chanMgr->hitlistlock.off();

					if(!isIndexTxt(&ch->info))	// for PCRaw (popup)
						peercastApp->notifyMessage(ServMgr::NT_PEERCAST,"Channel not found");
					return;
				}
			}


		}

		ch->lastIdleTime = sys->getTime();
		ch->setStatus(Channel::S_IDLE);
		ch->skipCount = 0; //JP-EX
		ch->lastSkipTime = 0;
		while ((ch->checkIdle()) && (ch->thread.active))
		{
			sys->sleepIdle();
		}

		sys->sleepIdle();
	}

}
// -----------------------------------
void	Channel::startICY(ClientSocket *cs, SRC_TYPE st)
{
	srcType = st;
	type = T_BROADCAST;
	cs->setReadTimeout(0);	// stay connected even when theres no data coming through
	sock = cs;
	info.srcProtocol = ChanInfo::SP_HTTP;

	streamIndex = ++chanMgr->icyIndex;

	sourceData = new ICYSource();
	startStream();
}

// -----------------------------------
static char *nextMetaPart(char *str,char delim)
{
	while (*str)
	{
		if (*str == delim)
		{
			*str++ = 0;
			return str;
		}
		str++;
	}
	return NULL;
}
// -----------------------------------
static void copyStr(char *to,char *from,int max)
{
	char c;
	while ((c=*from++) && (--max))
		if (c != '\'')
			*to++ = c;

	*to = 0;
}

// -----------------------------------
void Channel::processMp3Metadata(char *str)
{
	ChanInfo newInfo = info;
	
	char *cmd=str;
	while (cmd)
	{
		char *arg = nextMetaPart(cmd,'=');
		if (!arg)
			break;

		char *next = nextMetaPart(arg,';');

		if (strcmp(cmd,"StreamTitle")==0)
		{
			newInfo.track.title.setUnquote(arg,String::T_ASCII);
			newInfo.track.title.convertTo(String::T_UNICODE);

		}else if (strcmp(cmd,"StreamUrl")==0)
		{
			newInfo.track.contact.setUnquote(arg,String::T_ASCII);
			newInfo.track.contact.convertTo(String::T_UNICODE);
		}


		cmd = next;
	}

	updateInfo(newInfo);
}

// -----------------------------------
XML::Node *ChanHit::createXML()
{
	// IP
	char ipStr[64];
	host.toStr(ipStr);
	
	return new XML::Node("host ip=\"%s\" hops=\"%d\" listeners=\"%d\" relays=\"%d\" uptime=\"%d\" push=\"%d\" relay=\"%d\" direct=\"%d\" cin=\"%d\" stable=\"%d\" version=\"%d\" update=\"%d\" tracker=\"%d\"",
		ipStr,
		numHops,
		numListeners,
		numRelays,
		upTime,
		firewalled?1:0,
		relay?1:0,
		direct?1:0,
		cin?1:0,
		stable?1:0,
		version,
		sys->getTime()-time,
		tracker
		);

}

// -----------------------------------
XML::Node *ChanHitList::createXML(bool addHits)
{
	XML::Node *hn = new XML::Node("hits hosts=\"%d\" listeners=\"%d\" relays=\"%d\" firewalled=\"%d\" closest=\"%d\" furthest=\"%d\" newest=\"%d\"",
		numHits(),
		numListeners(),
		numRelays(),
		numFirewalled(),
		closestHit(),
		furthestHit(),
		sys->getTime()-newestHit()
		);		

	if (addHits)
	{
		ChanHit *h = hit;
		while (h)
		{
			if (h->host.ip)
				hn->add(h->createXML());
			h = h->next;
		}
	}

	return hn;
}

// -----------------------------------
XML::Node *Channel::createRelayXML(bool showStat)
{
	const char *ststr;
	ststr = getStatusStr();
	if (!showStat)
		if ((status == S_RECEIVING) || (status == S_BROADCASTING))
			ststr = "OK";

	ChanHitList *chl = chanMgr->findHitList(info);

	return new XML::Node("relay listeners=\"%d\" relays=\"%d\" hosts=\"%d\" status=\"%s\"",
		localListeners(),
		localRelays(),
		(chl!=NULL)?chl->numHits():0,
		ststr
		);	
}

// -----------------------------------
void ChanMeta::fromXML(XML &xml)
{
	MemoryStream tout(data,MAX_DATALEN);
	xml.write(tout);

	len = tout.pos;
}
// -----------------------------------
void ChanMeta::fromMem(void *p, int l)
{
	len = l;
	memcpy(data,p,len);
}
// -----------------------------------
void ChanMeta::addMem(void *p, int l)
{
	if ((len+l) <= MAX_DATALEN)
	{
		memcpy(data+len,p,l);
		len += l;
	}
}
// -----------------------------------
void Channel::broadcastTrackerUpdate(GnuID &svID, bool force)
{
	unsigned int ctime = sys->getTime();

	if (((ctime-lastTrackerUpdate) > 30) || (force))
	{
		ChanPacket pack;

		MemoryStream mem(pack.data,sizeof(pack));

		AtomStream atom(mem);
			
		ChanHit hit;

		ChanHitList *chl = chanMgr->findHitListByID(info.id);
		if (!chl)
			throw StreamException("Broadcast channel has no hitlist");

		int numListeners = stealth ? -1 : totalListeners();
		int numRelays = stealth ? -1 : totalRelays();

		unsigned int oldp = rawData.getOldestPos();
		unsigned int newp = rawData.getLatestPos();

		hit.initLocal(numListeners,numRelays,info.numSkips,info.getUptime(),isPlaying(), false, 0, this, oldp,newp);
		hit.tracker = true;

		if (VERSION_EX==0){
			atom.writeParent(PCP_BCST,8);
		} else {
			atom.writeParent(PCP_BCST,10);
		}
			atom.writeChar(PCP_BCST_GROUP,PCP_BCST_GROUP_ROOT);
			atom.writeChar(PCP_BCST_HOPS,0);
			atom.writeChar(PCP_BCST_TTL,11);
			atom.writeBytes(PCP_BCST_FROM,servMgr->sessionID.id,16);
			atom.writeInt(PCP_BCST_VERSION,PCP_CLIENT_VERSION);
			atom.writeInt(PCP_BCST_VERSION_VP,PCP_CLIENT_VERSION_VP);
			if (VERSION_EX){
				atom.writeBytes(PCP_BCST_VERSION_EX_PREFIX,PCP_CLIENT_VERSION_EX_PREFIX,2);
				atom.writeShort(PCP_BCST_VERSION_EX_NUMBER,PCP_CLIENT_VERSION_EX_NUMBER);
			}
			atom.writeParent(PCP_CHAN,4);
				atom.writeBytes(PCP_CHAN_ID,info.id.id,16);
				atom.writeBytes(PCP_CHAN_BCID,chanMgr->broadcastID.id,16);
				info.writeInfoAtoms(atom);
				info.writeTrackAtoms(atom);
			hit.writeAtoms(atom,info.id);


		pack.len = mem.pos;
		pack.type = ChanPacket::T_PCP;

		GnuID noID;
		noID.clear();
		int cnt = servMgr->broadcastPacket(pack,noID,servMgr->sessionID,svID,Servent::T_COUT);

		if (cnt)
		{
			LOG_DEBUG("Sent tracker update for %s to %d client(s)",info.name.cstr(),cnt);
			lastTrackerUpdate = ctime;
		}
	}
}

// -----------------------------------
bool	Channel::sendPacketUp(ChanPacket &pack,GnuID &cid,GnuID &sid,GnuID &did)
{
	if ( isActive() 
		&& (!cid.isSet() || info.id.isSame(cid)) 
		&& (!sid.isSet() || !remoteID.isSame(sid))
		&& sourceStream 
	   )
		return sourceStream->sendPacket(pack,did);

	return false;
}

// -----------------------------------
void Channel::updateInfo(ChanInfo &newInfo)
{
	if (info.update(newInfo))
	{
		if (isBroadcasting())
		{
			unsigned int ctime = sys->getTime();
			if ((ctime-lastMetaUpdate) > 30)
			{
				lastMetaUpdate = ctime;

				ChanPacket pack;

				MemoryStream mem(pack.data,sizeof(pack));

				AtomStream atom(mem);

				if (VERSION_EX==0){
					atom.writeParent(PCP_BCST,8);
				} else {
					atom.writeParent(PCP_BCST,10);
				}

					atom.writeChar(PCP_BCST_HOPS,0);
					atom.writeChar(PCP_BCST_TTL,7);
					atom.writeChar(PCP_BCST_GROUP,PCP_BCST_GROUP_RELAYS);
					atom.writeBytes(PCP_BCST_FROM,servMgr->sessionID.id,16);
					atom.writeInt(PCP_BCST_VERSION,PCP_CLIENT_VERSION);
					atom.writeInt(PCP_BCST_VERSION_VP,PCP_CLIENT_VERSION_VP);
					if (VERSION_EX){
						atom.writeBytes(PCP_BCST_VERSION_EX_PREFIX,PCP_CLIENT_VERSION_EX_PREFIX,2);
						atom.writeShort(PCP_BCST_VERSION_EX_NUMBER,PCP_CLIENT_VERSION_EX_NUMBER);
					}
					atom.writeBytes(PCP_BCST_CHANID,info.id.id,16);
					atom.writeParent(PCP_CHAN,3);
						atom.writeBytes(PCP_CHAN_ID,info.id.id,16);
						info.writeInfoAtoms(atom);
						info.writeTrackAtoms(atom);

				pack.len = mem.pos;
				pack.type = ChanPacket::T_PCP;
				GnuID noID;
				noID.clear();
				servMgr->broadcastPacket(pack,info.id,servMgr->sessionID,noID,Servent::T_RELAY);

				broadcastTrackerUpdate(noID);
			}
		}

		ChanHitList *chl = chanMgr->findHitList(info);
		if (chl)
			chl->info = info;

		peercastApp->channelUpdate(&info);

	}

}
// -----------------------------------
ChannelStream *Channel::createSource()
{
//	if (servMgr->relayBroadcast)
//		chanMgr->broadcastRelays(NULL,chanMgr->minBroadcastTTL,chanMgr->maxBroadcastTTL);


	ChannelStream *source=NULL;

	if (info.srcProtocol == ChanInfo::SP_PEERCAST)
	{
		LOG_CHANNEL("Channel is Peercast");
		if (servMgr->allowConnectPCST) //JP-EX
			source = new PeercastStream();
		else
			throw StreamException("Channel is not allowed");
	}
	else if (info.srcProtocol == ChanInfo::SP_PCP)
	{
		LOG_CHANNEL("Channel is PCP");
		PCPStream *pcp = new PCPStream(remoteID);
		source = pcp;
	}
	else if (info.srcProtocol == ChanInfo::SP_MMS)
	{
		LOG_CHANNEL("Channel is MMS");
		source = new MMSStream();
	}else
	{
		switch(info.contentType)
		{
			case ChanInfo::T_MP3:
				LOG_CHANNEL("Channel is MP3 - meta: %d",icyMetaInterval);
				source = new MP3Stream();
				break;
			case ChanInfo::T_NSV:
				LOG_CHANNEL("Channel is NSV");
				source = new NSVStream();
				break;
			case ChanInfo::T_WMA:
			case ChanInfo::T_WMV:
				throw StreamException("Channel is WMA/WMV - but not MMS");
				break;
			case ChanInfo::T_OGG:
			case ChanInfo::T_OGM:
				LOG_CHANNEL("Channel is OGG");
				source = new OGGStream();
				break;
			default:
				LOG_CHANNEL("Channel is Raw");
				source = new RawStream();
				break;
		}
	}

	source->parent = this;

	return source;
}
// ------------------------------------------
void ChannelStream::updateStatus(Channel *ch)
{
	ChanPacket pack;
	if (getStatus(ch,pack))
	{
		if (!ch->isBroadcasting())
		{
			GnuID noID;
			noID.clear();
			int cnt = chanMgr->broadcastPacketUp(pack,ch->info.id,servMgr->sessionID,noID);
			LOG_CHANNEL("Sent channel status update to %d clients",cnt);
		}
	}
}

// ------------------------------------------
bool ChannelStream::getStatus(Channel *ch,ChanPacket &pack)
{
	unsigned int ctime = sys->getTime();

	if ((ch->isPlaying() == isPlaying)){
		if ((ctime-lastUpdate) < 10){
			return false;
		}

		if ((ctime-lastCheckTime) < 5){
			return false;
		}
		lastCheckTime = ctime;
	}

	ChanHitList *chl = chanMgr->findHitListByID(ch->info.id);

	if (!chl)
		return false;

/*	int newLocalListeners = ch->localListeners();
	int newLocalRelays = ch->localRelays();

	if (
		(
		(numListeners != newLocalListeners) 
		|| (numRelays != newLocalRelays) 
		|| (ch->isPlaying() != isPlaying) 
		|| (servMgr->getFirewall() != fwState)
		|| (((ctime-lastUpdate)>chanMgr->hostUpdateInterval) && chanMgr->hostUpdateInterval)
		)
		&& ((ctime-lastUpdate) > 10)
	   )
	{

		numListeners = newLocalListeners;
		numRelays = newLocalRelays;
		isPlaying = ch->isPlaying();
		fwState = servMgr->getFirewall();
		lastUpdate = ctime;

		ChanHit hit;

		hit.initLocal(ch->localListeners(),ch->localRelays(),ch->info.numSkips,ch->info.getUptime(),isPlaying, ch->isFull(), ch->info.bitrate, ch);
		hit.tracker = ch->isBroadcasting();*/

	int newLocalListeners = ch->localListeners();
	int newLocalRelays = ch->localRelays();

	unsigned int oldp = ch->rawData.getOldestPos();
	unsigned int newp = ch->rawData.getLatestPos();

	ChanHit hit;

//	LOG_DEBUG("isPlaying-------------------------------------- %d %d", ch->isPlaying(), isPlaying);

	hit.initLocal(newLocalListeners,newLocalRelays,ch->info.numSkips,ch->info.getUptime(),ch->isPlaying(), ch->isFull(), ch->info.bitrate, ch, oldp, newp);
	{ //JP-MOD
		if(!(ch->info.ppFlags & ServMgr::bcstClap))
			ch->bClap = false;
		hit.initLocal_pp(ch->stealth, ch->bClap ? 1 : 0);
	}
	hit.tracker = ch->isBroadcasting();

	if	(	(((ctime-lastUpdate)>chanMgr->hostUpdateInterval) && chanMgr->hostUpdateInterval)
		||	(newLocalListeners != numListeners)
		||	(newLocalRelays != numRelays)
		||	(ch->isPlaying() != isPlaying)
		||	(servMgr->getFirewall() != fwState)
		||	(ch->chDisp.relay != hit.relay)
		||	(ch->chDisp.relayfull != hit.relayfull)
		||	(ch->chDisp.chfull != hit.chfull)
		||	(ch->chDisp.ratefull != hit.ratefull)
		||	(ch->bClap && ((ctime-lastClapped) > 60)) //JP-MOD
	){
		numListeners = newLocalListeners;
		numRelays = newLocalRelays;
		isPlaying = ch->isPlaying();
		fwState = servMgr->getFirewall();
		lastUpdate = ctime;

		if(ch->bClap){ //JP-MOD
			lastClapped = ctime;
			ch->bClap = false;
		}
	
		ch->chDisp = hit;

		if ((numRelays) && ((servMgr->getFirewall() == ServMgr::FW_OFF) && (servMgr->autoRelayKeep!=0))) //JP-EX
			ch->stayConnected = true;

		if ((!numRelays && !numListeners) && (servMgr->autoRelayKeep==2)) //JP-EX
			ch->stayConnected = false;

		MemoryStream pmem(pack.data,sizeof(pack.data));
		AtomStream atom(pmem);

		GnuID noID;
		noID.clear();

		if (VERSION_EX==0){
			atom.writeParent(PCP_BCST,8);
		} else {
			atom.writeParent(PCP_BCST,10);
		}

			atom.writeChar(PCP_BCST_GROUP,PCP_BCST_GROUP_TRACKERS);
			atom.writeChar(PCP_BCST_HOPS,0);
			atom.writeChar(PCP_BCST_TTL,11);
			atom.writeBytes(PCP_BCST_FROM,servMgr->sessionID.id,16);
			atom.writeInt(PCP_BCST_VERSION,PCP_CLIENT_VERSION);
			atom.writeInt(PCP_BCST_VERSION_VP,PCP_CLIENT_VERSION_VP);
			if (VERSION_EX){
				atom.writeBytes(PCP_BCST_VERSION_EX_PREFIX,PCP_CLIENT_VERSION_EX_PREFIX,2);
				atom.writeShort(PCP_BCST_VERSION_EX_NUMBER,PCP_CLIENT_VERSION_EX_NUMBER);
			}
			atom.writeBytes(PCP_BCST_CHANID,ch->info.id.id,16);
			hit.writeAtoms(atom,noID);

		pack.len = pmem.pos;
		pack.type = ChanPacket::T_PCP;
		return true;
	}else
		return false;
}
// -----------------------------------
bool	Channel::checkBump()
{
	unsigned int maxIdleTime = 30;
	if (isIndexTxt(this)) maxIdleTime = 60;

	if (!isBroadcasting() && (!sourceHost.tracker))
		if (rawData.lastWriteTime && ((sys->getTime() - rawData.lastWriteTime) > maxIdleTime))
		{
			LOG_ERROR("Channel Auto bumped");
			bump = true;
		}
	
	if (bump)
	{
		bump = false;
		return true;
	}else
		return false;
}

// -----------------------------------
int Channel::readStream(Stream &in,ChannelStream *source)
{
	//sys->sleep(300);

	int error = 0;

	info.numSkips = 0;

	source->readHeader(in,this);

	peercastApp->channelStart(&info);

	rawData.lastWriteTime = 0;

	bool wasBroadcasting=false;

	unsigned int receiveStartTime = 0;

	unsigned int ptime = 0;
	unsigned int upsize = 0;

	try
	{
		while (thread.active && !peercastInst->isQuitting)
		{			
			if (checkIdle())
			{
				LOG_DEBUG("Channel idle");
				break;
			}

			if (checkBump())
			{
				LOG_DEBUG("Channel bumped");
				error = -1;
				bumped = true;
				break;
			}

			if (in.eof())
			{
				LOG_DEBUG("Channel eof");
				break;
			}

			if (in.readReady())
			{
				error = source->readPacket(in,this);

				if (error)
					break;

				//if (rawData.writePos > 0)
				if (rawData.lastWriteTime > 0 || rawData.lastSkipTime > 0)
				{
					if (isBroadcasting())
					{					
						if ((sys->getTime()-lastTrackerUpdate) >= chanMgr->hostUpdateInterval)
						{
							GnuID noID;
							noID.clear();
							broadcastTrackerUpdate(noID);
						}
						wasBroadcasting = true;

					}else
					{
/*						if (status != Channel::S_RECEIVING){
							receiveStartTime = sys->getTime();
						} else if (receiveStartTime && receiveStartTime + 10 > sys->getTime()){
							chanMgr->hitlistlock.on();
							ChanHitList *hl = chanMgr->findHitList(info);
							if (hl){
								hl->clearHits(true);
							}
							chanMgr->hitlistlock.off();
							receiveStartTime = 0;
						}*/
						setStatus(Channel::S_RECEIVING);
						bumped = false;
					}
//					source->updateStatus(this);
				}
			}
			if (rawData.lastWriteTime > 0 || rawData.lastSkipTime > 0)
				source->updateStatus(this);

			unsigned int t = sys->getTime();
			if (t != ptime) {
				ptime = t;
				upsize = Servent::MAX_OUTWARD_SIZE;
			}

			unsigned int len = source->flushUb(in, upsize);
			upsize -= len;

			sys->sleepIdle();
		}
	}catch(StreamException &e)
	{
		LOG_ERROR("readStream: %s",e.msg);
		error = -1;
	}

	if (!servMgr->keepDownstreams) {
		if (status == Channel::S_RECEIVING){
			chanMgr->hitlistlock.on();
			ChanHitList *hl = chanMgr->findHitList(info);
			if (hl){
				hl->clearHits(false);
			}
			chanMgr->hitlistlock.off();
		}
	}

//	setStatus(S_CLOSING);
	setStatus(S_IDLE);

	if (wasBroadcasting)
	{
		GnuID noID;
		noID.clear();
		broadcastTrackerUpdate(noID,true);
	}

	peercastApp->channelStop(&info);

	source->readEnd(in,this);

	return error;
}

// -----------------------------------
void PeercastStream::readHeader(Stream &in,Channel *ch)
{
	if (in.readTag() != 'PCST')
		throw StreamException("Not PeerCast stream");

}
// -----------------------------------
void PeercastStream::readEnd(Stream &,Channel *)
{

}
// -----------------------------------
int PeercastStream::readPacket(Stream &in,Channel *ch)
{
	ChanPacket pack;

	{

		pack.readPeercast(in);

		MemoryStream mem(pack.data,pack.len);

		switch(pack.type)
		{
			case ChanPacket::T_HEAD:
				// update sync pos
				ch->headPack = pack;
				pack.pos = ch->streamPos;
				ch->newPacket(pack);
				ch->streamPos+=pack.len;
				break;
			case ChanPacket::T_DATA:
				pack.pos = ch->streamPos;
				ch->newPacket(pack);
				ch->streamPos+=pack.len;
				break;
			case ChanPacket::T_META:
				ch->insertMeta.fromMem(pack.data,pack.len);
				{
					if (pack.len)
					{
						XML xml;
						xml.read(mem);
						XML::Node *n = xml.findNode("channel");					
						if (n)
						{
							ChanInfo newInfo = ch->info;
							newInfo.updateFromXML(n);
							ChanHitList *chl = chanMgr->findHitList(ch->info);
							if (chl)
								newInfo.updateFromXML(n);
							ch->updateInfo(newInfo);
						}
					}
				}
				break;
#if 0
			case ChanPacket::T_SYNC:
				{
					unsigned int s = mem.readLong();
					if ((s-ch->syncPos) != 1)
					{
						LOG_CHANNEL("Ch.%d SKIP: %d to %d (%d)",ch->index,ch->syncPos,s,ch->info.numSkips);
						if (ch->syncPos)
						{
							ch->info.numSkips++;
							if (ch->info.numSkips>50)
								throw StreamException("Bumped - Too many skips");
						}
					}

					ch->syncPos = s;
				}
				break;
#endif

		}

	}
	return 0;
}

// -----------------------------------
void ChannelStream::readRaw(Stream &in, Channel *ch)
{
	ChanPacket pack;

	const int readLen = 8192;

	pack.init(ChanPacket::T_DATA,pack.data,readLen,ch->streamPos);
	in.read(pack.data,pack.len);
	ch->newPacket(pack);
	ch->checkReadDelay(pack.len);

	ch->streamPos+=pack.len;

}
// ------------------------------------------
void RawStream::readHeader(Stream &,Channel *)
{
}

// ------------------------------------------
int RawStream::readPacket(Stream &in,Channel *ch)
{
	readRaw(in,ch);
	return 0;
}

// ------------------------------------------
void RawStream::readEnd(Stream &,Channel *)
{
}



// -----------------------------------
void ChanPacket::init(ChanPacketv &p)
{
	type = p.type;
	len = p.len;
	if (len > MAX_DATALEN)
		throw StreamException("Packet data too large");
	pos = p.pos;
	sync = p.sync;
	skip = p.skip;
	priority = p.priority;
	memcpy(data, p.data, len);
}
// -----------------------------------
void ChanPacket::init(TYPE t, const void *p, unsigned int l,unsigned int _pos)
{
	type = t;
	if (l > MAX_DATALEN)
		throw StreamException("Packet data too large");
	len = l;
	memcpy(data,p,len);
	pos = _pos;
	skip = false;
	priority = 0;
}
// -----------------------------------
void ChanPacket::writeRaw(Stream &out)
{
	out.write(data,len);
}
// -----------------------------------
void ChanPacket::writePeercast(Stream &out)
{
	unsigned int tp = 0;
	switch (type)
	{
		case T_HEAD: tp = 'HEAD'; break;
		case T_META: tp = 'META'; break;
		case T_DATA: tp = 'DATA'; break;
	}

	if (type != T_UNKNOWN)
	{
		out.writeTag(tp);
		out.writeShort(len);
		out.writeShort(0);
		out.write(data,len);
	}
}
// -----------------------------------
void ChanPacket::readPeercast(Stream &in)
{
	unsigned int tp = in.readTag();

	switch (tp)
	{
		case 'HEAD':	type = T_HEAD; break;
		case 'DATA':	type = T_DATA; break;
		case 'META':	type = T_META; break;
		default:		type = T_UNKNOWN;
	}
	len = in.readShort();
	in.readShort();
	if (len > MAX_DATALEN)
		throw StreamException("Bad ChanPacket");
	in.read(data,len);
}
// -----------------------------------
int ChanPacketBuffer::copyFrom(ChanPacketBuffer &buf, unsigned int reqPos)
{
	lock.on();
	buf.lock.on();

	firstPos = 0;
	lastPos = 0;
	safePos = 0;
	readPos = 0;

	for(unsigned int i=buf.firstPos; i<=buf.lastPos; i++)
	{
		//ChanPacket *src = &buf.packets[i%MAX_PACKETS];
		ChanPacketv *src = &buf.packets[i%MAX_PACKETS];
		if (src->type & accept)
		{
			if (src->pos >= reqPos)
			{
				lastPos = writePos;
				//packets[writePos++] = *src;
				packets[writePos++].init(*src);
			}
		}

	}


	buf.lock.off();
	lock.off();
	return lastPos-firstPos;
}

// -----------------------------------
bool ChanPacketBuffer::findPacket(unsigned int spos, ChanPacket &pack)
{
	if (writePos == 0)
		return false;

	lock.on();

	unsigned int bound = packets[0].len * ChanPacketBuffer::MAX_PACKETS * 2; // max packets to wait
	unsigned int fpos = getFirstDataPos();
	unsigned int lpos = getLatestPos();
	if ((spos < fpos && fpos <= lpos && spos != getStreamPosEnd(lastPos)) // --s-----f---l--
		|| (spos < fpos && lpos < fpos && spos > lpos + bound)            // -l-------s--f--
	    || (spos > lpos && lpos >= fpos && spos - lpos > bound))          // --f---l------s-
		spos = fpos;


	for(unsigned int i=firstPos; i<=lastPos; i++)
	{
		//ChanPacket &p = packets[i%MAX_PACKETS];
		ChanPacketv &p = packets[i%MAX_PACKETS];
		if (p.pos >= spos && p.pos - spos <= bound)
		{
			pack.init(p);
			lock.off();
			return true;
		}
	}

	lock.off();
	return false;
}
// -----------------------------------
unsigned int	ChanPacketBuffer::getFirstDataPos()
{
	if (!writePos)
		return 0;
	for(unsigned int i=firstPos; i<=lastPos; i++)
	{
		if (packets[i%MAX_PACKETS].type == ChanPacket::T_DATA)
			return packets[i%MAX_PACKETS].pos;
	}
	return 0;
}
// -----------------------------------
unsigned int	ChanPacketBuffer::getLatestPos()
{
	if (!writePos)
		return 0;
	else
		return getStreamPos(lastPos);
		
}
// -----------------------------------
unsigned int	ChanPacketBuffer::getOldestPos()
{
	if (!writePos)
		return 0;
	else
		return getStreamPos(firstPos);
}

// -----------------------------------
unsigned int	ChanPacketBuffer::findOldestPos(unsigned int spos)
{
	unsigned int min = getStreamPos(safePos);
	unsigned int max = getStreamPos(lastPos);

	if (min > spos)
		return min;

	if (max < spos)
		return max;

	return spos;
}

// -----------------------------------
unsigned int	ChanPacketBuffer::getStreamPos(unsigned int index)
{
	return packets[index%MAX_PACKETS].pos;
}
// -----------------------------------
unsigned int	ChanPacketBuffer::getStreamPosEnd(unsigned int index)
{
	return packets[index%MAX_PACKETS].pos+packets[index%MAX_PACKETS].len;
}
// -----------------------------------
bool ChanPacketBuffer::writePacket(ChanPacket &pack, bool updateReadPos)
{
	if (pack.len)
	{
		if (servMgr->keepDownstreams) {
			unsigned int lpos = getLatestPos();
			unsigned int diff = pack.pos - lpos;
			if (packets[lastPos%MAX_PACKETS].type == ChanPacket::T_HEAD) lpos = 0;
			if (lpos && (diff == 0 || diff > 0xfff00000)) {
				LOG_DEBUG("*   latest pos=%d, pack pos=%d", getLatestPos(), pack.pos);
				lastSkipTime = sys->getTime();
				return false;
			}
		}

		if (willSkip())	// too far behind
		{
			lastSkipTime = sys->getTime();
			return false;
		}

		lock.on();

		pack.sync = writePos;
		packets[writePos%MAX_PACKETS].init(pack);

//		LOG_DEBUG("packet.len = %d",pack.len);


		lastPos = writePos;
		writePos++;

		if (writePos >= MAX_PACKETS)
			firstPos = writePos-MAX_PACKETS;
		else
			firstPos = 0;

		if (writePos >= NUM_SAFEPACKETS)
			safePos = writePos - NUM_SAFEPACKETS;
		else
			safePos = 0;

		if (updateReadPos)
			readPos = writePos;

		lastWriteTime = sys->getTime();

		lock.off();
		return true;
	}

	return false;
}
// -----------------------------------
void	ChanPacketBuffer::readPacket(ChanPacket &pack)
{

	unsigned int tim = sys->getTime();

	if (readPos < firstPos)	
		throw StreamException("Read too far behind");

	while (readPos >= writePos)
	{
		sys->sleepIdle();
		if ((sys->getTime() - tim) > 30)
			throw TimeoutException();
	}
	lock.on();
	pack.init(packets[readPos%MAX_PACKETS]);
	readPos++;
	lock.off();
}
// -----------------------------------
void	ChanPacketBuffer::readPacketPri(ChanPacket &pack)
{

	unsigned int tim = sys->getTime();

	if (readPos < firstPos)	
		throw StreamException("Read too far behind");

	while (readPos >= writePos)
	{
		sys->sleepIdle();
		if ((sys->getTime() - tim) > 30)
			throw TimeoutException();
	}
	lock.on();
	ChanPacketv *best = &packets[readPos % MAX_PACKETS];
	for (unsigned int i = readPos + 1; i < writePos; i++) {
		if (packets[i % MAX_PACKETS].priority > best->priority)
			best = &packets[i % MAX_PACKETS];
	}
	pack.init(*best);
	best->init(packets[readPos % MAX_PACKETS]);
	readPos++;
	lock.off();
}
// -----------------------------------
bool	ChanPacketBuffer::willSkip()
{
	return ((writePos-readPos) >= MAX_PACKETS);
}

// -----------------------------------
void Channel::getStreamPath(char *str)
{
	char idStr[64];

	getIDStr(idStr);

	sprintf(str,"/stream/%s%s",idStr,info.getTypeExt(info.contentType));
}



// -----------------------------------
void ChanMgr::startSearch(ChanInfo &info)
{
	searchInfo = info;
	clearHitLists();
	numFinds = 0;
	lastHit = 0;
//	lastSearch = 0;
	searchActive = true;
}
// -----------------------------------
void ChanMgr::quit()
{
	LOG_DEBUG("ChanMgr is quitting..");
	closeAll();
}
// -----------------------------------
int ChanMgr::numIdleChannels()
{
	int cnt=0;
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
			if (ch->thread.active)
				if (ch->status == Channel::S_IDLE)
					cnt++;
		ch=ch->next;
	}
	return cnt;
}
// -----------------------------------
void ChanMgr::closeOldestIdle()
{
	unsigned int idleTime = (unsigned int)-1;
	Channel *ch = channel,*oldest=NULL;
	while (ch)
	{
		if (ch->isActive())
			if (ch->thread.active)
				if (ch->status == Channel::S_IDLE)
					if (ch->lastIdleTime < idleTime)
					{
						oldest = ch;
						idleTime = ch->lastIdleTime;
					}
		ch=ch->next;
	}

	if (oldest)
		oldest->thread.active = false;
}

// -----------------------------------
void ChanMgr::closeAll()
{
	Channel *ch = channel;
	while (ch)
	{
		if (ch->thread.active)
			ch->thread.shutdown();
		ch=ch->next;
	}
}
// -----------------------------------
Channel *ChanMgr::findChannelByNameID(ChanInfo &info)
{
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
			if (ch->info.matchNameID(info))
				return ch;
		ch=ch->next;
	}
	return NULL;
}

// -----------------------------------
Channel *ChanMgr::findChannelByName(const char *n)
{
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
			if (stricmp(ch->info.name,n)==0)
				return ch;
		ch=ch->next;
	}

	return NULL;
}
// -----------------------------------
Channel *ChanMgr::findChannelByIndex(int index)
{
	int cnt=0;
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
		{
			if (cnt == index)
				return ch;
			cnt++;
		}
		ch=ch->next;
	}
	return NULL;
}	
// -----------------------------------
Channel *ChanMgr::findChannelByMount(const char *str)
{
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
			if (strcmp(ch->mount,str)==0)
				return ch;
		ch=ch->next;
	}

	return NULL;
}	
// -----------------------------------
Channel *ChanMgr::findChannelByID(GnuID &id)
{
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
			if (ch->info.id.isSame(id))
				return ch;
		ch=ch->next;
	}
	return NULL;
}	
// -----------------------------------
Channel *ChanMgr::findChannelByChannelID(int id)
{
	int cnt=0;
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive()){
			if (ch->channel_id == id){
				return ch;
			}
		}
		ch = ch->next;
	}
	return NULL;
}
// -----------------------------------
int ChanMgr::findChannels(ChanInfo &info, Channel **chlist, int max)
{
	int cnt=0;
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
			if (ch->info.match(info))
			{
				chlist[cnt++] = ch;
				if (cnt >= max)
					break;
			}
		ch=ch->next;
	}
	return cnt;
}
// -----------------------------------
int ChanMgr::findChannelsByStatus(Channel **chlist, int max, Channel::STATUS status)
{
	int cnt=0;
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
			if (ch->status == status)
			{
				chlist[cnt++] = ch;
				if (cnt >= max)
					break;
			}
		ch=ch->next;
	}
	return cnt;
}
// -----------------------------------
Channel *ChanMgr::createRelay(ChanInfo &info, bool stayConnected)
{
	Channel *c = chanMgr->createChannel(info,NULL);
	if (c)
	{
		c->stayConnected = stayConnected;
		c->startGet();
		return c;
	}
	return NULL;
}
// -----------------------------------
Channel *ChanMgr::findAndRelay(ChanInfo &info)
{
	char idStr[64];
	info.id.toStr(idStr);
	LOG_CHANNEL("Searching for: %s (%s)",idStr,info.name.cstr());

	if(!isIndexTxt(&info))	// for PCRaw (popup)
		peercastApp->notifyMessage(ServMgr::NT_PEERCAST,"Finding channel...");


	Channel *c = NULL;

	WLockBlock wb(&(chanMgr->channellock));

	wb.on();

	c = findChannelByNameID(info);

	if (!c)
	{
		c = chanMgr->createChannel(info,NULL);
		if (c)
		{
			c->setStatus(Channel::S_SEARCHING);			
			c->startGet();
		}
	} else if (!(c->thread.active)){
		c->thread.active = true;
		c->thread.finish = false;
		c->info.lastPlayStart = 0; // reconnect 
		c->info.lastPlayEnd = 0;
		if (c->finthread){
			c->finthread->finish = true;
			c->finthread = NULL;
		}
		if (c->status != Channel::S_CONNECTING && c->status != Channel::S_SEARCHING){
			c->setStatus(Channel::S_SEARCHING);	
			c->startGet();
		}
	}

	wb.off();

	for(int i=0; i<600; i++)	// search for 1 minute.
	{


		wb.on();
		c = findChannelByNameID(info);

		if (!c)
		{
//			peercastApp->notifyMessage(ServMgr::NT_PEERCAST,"Channel not found");
			return NULL;
		}

		
		if (c->isPlaying() && (c->info.contentType!=ChanInfo::T_UNKNOWN))
			break;

		wb.off();

		sys->sleep(100);
	}

	return c;
}
// -----------------------------------
ChanMgr::ChanMgr()
{
	channel = NULL;
	
	hitlist = NULL;

	currFindAndPlayChannel.clear();

	broadcastMsg.clear();
	broadcastMsgInterval=10;

	broadcastID.generate(PCP_BROADCAST_FLAGS);

	deadHitAge = 600;

	icyIndex = 0;
	icyMetaInterval = 8192;	
	maxRelaysPerChannel = 1;

	searchInfo.init();

	minBroadcastTTL = 1;
	maxBroadcastTTL = 7;

	pushTimeout = 60;	// 1 minute
	pushTries = 5;		// 5 times
	maxPushHops = 8;	// max 8 hops away
	maxUptime = 0;		// 0 = none

	prefetchTime = 10;	// n seconds

	hostUpdateInterval = 120; // 2 minutes

	bufferTime = 5;

	autoQuery = 0;	
	lastQuery = 0;

	lastYPConnect = 0;
	lastYPConnect2 = 0;

}

// -----------------------------------
bool ChanMgr::writeVariable(Stream &out, const String &var, int index)
{
	char buf[1024];
	if (var == "numHitLists")
		sprintf(buf,"%d",numHitLists());
	
	else if (var == "numChannels")
		sprintf(buf,"%d",numChannels());
	else if (var == "djMessage") //JP-MOD
	{
		String utf8 = broadcastMsg;
		utf8.convertTo(String::T_UNICODESAFE);
		strcpy(buf,utf8.cstr());
	}
	else if (var == "icyMetaInterval")
		sprintf(buf,"%d",icyMetaInterval);
	else if (var == "maxRelaysPerChannel")
		sprintf(buf,"%d",maxRelaysPerChannel);
	else if (var == "hostUpdateInterval")
		sprintf(buf,"%d",hostUpdateInterval);
	else if (var == "broadcastID")
		broadcastID.toStr(buf);
	else
		return false;



	out.writeString(buf);
	return true;
}

// -----------------------------------
bool Channel::writeVariable(Stream &out, const String &var, int index)
{
	char buf[1024];

	buf[0]=0;

	String utf8;

	if (var == "name")
	{
		utf8 = info.name;
		utf8.convertTo(String::T_UNICODESAFE);
		strcpy(buf,utf8.cstr());

	}else if (var == "bitrate")
	{
		sprintf(buf,"%d",info.bitrate);

	}else if (var == "srcrate")
	{
		if (sourceData)
		{
			unsigned int tot = sourceData->getSourceRate();
			sprintf(buf,"%.1f",BYTES_TO_KBPS(tot));
		}else
			strcpy(buf,"0");

	}else if (var == "genre")
	{
		utf8 = info.genre;
		utf8.convertTo(String::T_UNICODESAFE);
		strcpy(buf,utf8.cstr());
	}else if (var == "desc")
	{
		utf8 = info.desc;
		utf8.convertTo(String::T_UNICODESAFE);
		strcpy(buf,utf8.cstr());
	}else if (var == "comment")
	{
		utf8 = info.comment;
		utf8.convertTo(String::T_UNICODESAFE);
		strcpy(buf,utf8.cstr());
	}else if (var == "bcstClap") //JP-MOD
	{
		strcpy(buf,info.ppFlags & ServMgr::bcstClap ? "1":"0");
	}else if (var == "uptime")
	{
		String uptime;
		if (info.lastPlayStart)
			uptime.setFromStopwatch(sys->getTime()-info.lastPlayStart);
		else
			uptime.set("-");
		strcpy(buf,uptime.cstr());
	}
	else if (var == "type")
		sprintf(buf,"%s",ChanInfo::getTypeStr(info.contentType));
	else if (var == "ext")
		sprintf(buf,"%s",ChanInfo::getTypeExt(info.contentType));
	else if (var == "proto") {
		switch(info.contentType) {
		case ChanInfo::T_WMA:
		case ChanInfo::T_WMV:
			sprintf(buf, "mms://");
			break;
		default:
			sprintf(buf, "http://");
		}
	}
	else if (var == "localRelays")
		sprintf(buf,"%d",localRelays());
	else if (var == "localListeners")
		sprintf(buf,"%d",localListeners());

	else if (var == "totalRelays")
		sprintf(buf,"%d",totalRelays());
	else if (var == "totalListeners")
		sprintf(buf,"%d",totalListeners());
	else if (var == "totalClaps") //JP-MOD
		sprintf(buf,"%d",totalClaps());

	else if (var == "status")
		sprintf(buf,"%s",getStatusStr());
	else if (var == "keep")
		sprintf(buf,"%s",stayConnected?"Yes":"No");
	else if (var == "id")
		info.id.toStr(buf);
	else if (var.startsWith("track."))
	{

		if (var == "track.title")
			utf8 = info.track.title;
		else if (var == "track.artist")
			utf8 = info.track.artist;
		else if (var == "track.album")
			utf8 = info.track.album;
		else if (var == "track.genre")
			utf8 = info.track.genre;
		else if (var == "track.contactURL")
			utf8 = info.track.contact;

		utf8.convertTo(String::T_UNICODESAFE);
		strcpy(buf,utf8.cstr());

	}else if (var == "contactURL")
		sprintf(buf,"%s",info.url.cstr());
	else if (var == "streamPos")
		sprintf(buf,"%d",streamPos);
	else if (var == "sourceType")
		strcpy(buf,getSrcTypeStr());
	else if (var == "sourceProtocol")
		strcpy(buf,ChanInfo::getProtocolStr(info.srcProtocol));
	else if (var == "sourceURL")
	{
		if (sourceURL.isEmpty())
			sourceHost.host.toStr(buf);
		else
			strcpy(buf,sourceURL.cstr());
	}
	else if (var == "headPos")
		sprintf(buf,"%d",headPack.pos);
	else if (var == "headLen")
		sprintf(buf,"%d",headPack.len);
	else if (var == "numHits")
	{
		ChanHitList *chl = chanMgr->findHitListByID(info.id);
		int numHits = 0;
		if (chl){
//			numHits = chl->numHits();
			ChanHit *hit;
			hit = chl->hit;
			while(hit){
				numHits++;
				hit = hit->next;
			}
		}
		sprintf(buf,"%d",numHits);
	} else if (var == "isBroadcast")
		strcpy(buf, (type == T_BROADCAST) ? "1":"0");
	else
		return false;

	out.writeString(buf);
	return true;
}

// -----------------------------------
void ChanMgr::broadcastTrackerUpdate(GnuID &svID, bool force)
{
	Channel *c = channel;
	while(c)
	{
		if ( c->isActive() && c->isBroadcasting() )
			c->broadcastTrackerUpdate(svID,force);
		c=c->next;
	}
}


// -----------------------------------
int ChanMgr::broadcastPacketUp(ChanPacket &pack,GnuID &chanID, GnuID &srcID, GnuID &destID)
{
	int cnt=0;

	Channel *c = channel;
	while(c)
	{
		if (c->sendPacketUp(pack,chanID,srcID,destID))
			cnt++;
		c=c->next;
	}

	return cnt;
}

// -----------------------------------
void ChanMgr::broadcastRelays(Servent *serv, int minTTL, int maxTTL)
{
	//if ((servMgr->getFirewall() == ServMgr::FW_OFF) || servMgr->serverHost.localIP())
	{

		Host sh = servMgr->serverHost;
		bool push = (servMgr->getFirewall()!=ServMgr::FW_OFF);
		bool busy = (servMgr->pubInFull() && servMgr->outFull()) || servMgr->relaysFull();
		bool stable = servMgr->totalStreams>0;


		GnuPacket hit;

		int numChans=0;

		Channel *c = channel;
		while(c)
		{
			if (c->isPlaying())
			{

				bool tracker = c->isBroadcasting();

				int ttl = (c->info.getUptime() / servMgr->relayBroadcast);	// 1 hop per N seconds

				if (ttl < minTTL)
					ttl = minTTL;

				if (ttl > maxTTL)
					ttl = maxTTL;

				if (hit.initHit(sh,c,NULL,push,busy,stable,tracker,ttl))
				{
					int numOut=0;
					numChans++;
					if (serv)
					{
						serv->outputPacket(hit,false);
						numOut++;
					}

					LOG_NETWORK("Sent channel to %d servents, TTL %d",numOut,ttl);		

				}
			}
			c=c->next;
		}
		//if (numChans)
		//	LOG_NETWORK("Sent %d channels to %d servents",numChans,numOut);		
	}
}
// -----------------------------------
void ChanMgr::setUpdateInterval(unsigned int v)
{
	hostUpdateInterval = v;
}


// -----------------------------------
// message check
#if 0
				ChanPacket pack;
				MemoryStream mem(pack.data,sizeof(pack.data));
				AtomStream atom(mem);
				atom.writeParent(PCP_BCST,3);
					atom.writeChar(PCP_BCST_GROUP,PCP_BCST_GROUP_ALL);
					atom.writeBytes(PCP_BCST_FROM,servMgr->sessionID.id,16);
					atom.writeParent(PCP_MESG,1);
						atom.writeString(PCP_MESG_DATA,msg.cstr());

				mem.len = mem.pos;
				mem.rewind();
				pack.len = mem.len;

				GnuID noID;
				noID.clear();

				BroadcastState bcs;
				PCPStream::readAtom(atom,bcs);
				//int cnt = servMgr->broadcastPacketUp(pack,noID,servMgr->sessionID);
				//int cnt = servMgr->broadcastPacketDown(pack,noID,servMgr->sessionID);
				//int cnt = chanMgr->broadcastPacketUp(pack,noID,servMgr->sessionID);
				//LOG_DEBUG("Sent message to %d clients",cnt);
#endif
// -----------------------------------
void ChanMgr::setBroadcastMsg(String &msg)
{
	if (!msg.isSame(broadcastMsg))
	{
		broadcastMsg = msg;

		Channel *c = channel;
		while(c)
		{
			if (c->isActive() && c->isBroadcasting())
			{
				ChanInfo newInfo = c->info;
				newInfo.comment = broadcastMsg;
				c->updateInfo(newInfo);
			}
			c=c->next;
		}

	}
}


// -----------------------------------
void ChanMgr::clearHitLists()
{

//	LOG_DEBUG("clearHitLists HITLISTLOCK ON-------------");
	chanMgr->hitlistlock.on();
	while (hitlist)
	{
		peercastApp->delChannel(&hitlist->info);
	
		ChanHitList *next = hitlist->next;

		delete hitlist;

		hitlist = next;
	}
//	LOG_DEBUG("clearHitLists HITLISTLOCK OFF-------------");
	chanMgr->hitlistlock.off();
}
// -----------------------------------
Channel *ChanMgr::deleteChannel(Channel *delchan)
{
	lock.on();

	Channel *ch = channel,*prev=NULL,*next=NULL;

	while (ch)
	{
		if (ch == delchan)
		{
			Channel *next = ch->next;
			if (prev)
				prev->next = next;
			else
				channel = next;

			if (delchan->sourceStream){
				delchan->sourceStream->parent = NULL;
			}

			delete delchan;

			break;
		}
		prev = ch;
		ch=ch->next;
	}


	lock.off();
	return next;
}

// -----------------------------------
Channel *ChanMgr::createChannel(ChanInfo &info, const char *mount)
{
	lock.on();

	Channel *nc=NULL;

	nc = new Channel();

	nc->next = channel;
	channel = nc;


	nc->info = info;
	nc->info.lastPlayStart = 0;
	nc->info.lastPlayEnd = 0;
	nc->info.status = ChanInfo::S_UNKNOWN;
	if (mount)
		nc->mount.set(mount);
	nc->setStatus(Channel::S_WAIT);
	nc->type = Channel::T_ALLOCATED;
	nc->info.createdTime = sys->getTime();

	LOG_CHANNEL("New channel created");

	lock.off();
	return nc;
}
// -----------------------------------
int ChanMgr::pickHits(ChanHitSearch &chs)
{
	ChanHitList *chl = hitlist;
	while(chl)
	{
		if (chl->isUsed())
			if (chl->pickHits(chs))
			{
				chl->info.id;
				return 1;
			}
		chl = chl->next;
	}
	return 0;
}

// -----------------------------------
ChanHitList *ChanMgr::findHitList(ChanInfo &info)
{
	ChanHitList *chl = hitlist;
	while(chl)
	{
		if (chl->isUsed())
			if (chl->info.matchNameID(info))
				return chl;

		chl = chl->next;
	}
	return NULL;
}
// -----------------------------------
ChanHitList *ChanMgr::findHitListByID(GnuID &id)
{
	ChanHitList *chl = hitlist;
	while(chl)
	{
		if (chl->isUsed())
			if (chl->info.id.isSame(id))
				return chl;
		chl = chl->next;
	}
	return NULL;
}
// -----------------------------------
int ChanMgr::numHitLists()
{
	int num=0;
	ChanHitList *chl = hitlist;
	while(chl)
	{
		if (chl->isUsed())
			num++;
		chl = chl->next;
	}
	return num;
}
// -----------------------------------
ChanHitList *ChanMgr::addHitList(ChanInfo &info)
{	
	ChanHitList *chl = new ChanHitList();	


	chl->next = hitlist;
	hitlist = chl;

	chl->used = true;
	chl->info = info;
	chl->info.createdTime = sys->getTime();
	peercastApp->addChannel(&chl->info);


	return chl;
}
// -----------------------------------
void ChanMgr::clearDeadHits(bool clearTrackers)
{
	unsigned int interval;
	
	if (servMgr->isRoot)
		interval = 1200;		// mainly for old 0.119 clients
	else
		interval = hostUpdateInterval+120;

	chanMgr->hitlistlock.on();
	ChanHitList *chl = hitlist,*prev = NULL;
	while (chl)
	{
		if (chl->isUsed())
		{
			if (chl->clearDeadHits(interval,clearTrackers) == 0)
			{
				if (!isBroadcasting(chl->info.id))
				{
					if (!chanMgr->findChannelByID(chl->info.id))
					{
						peercastApp->delChannel(&chl->info);

						ChanHitList *next = chl->next;
						if (prev)
							prev->next = next;
						else
							hitlist = next;

						delete chl;
						chl = next;
						continue;
					}
				}
			}
		}
		prev = chl;
		chl = chl->next;
	}
	chanMgr->hitlistlock.off();
}
// -----------------------------------
bool	ChanMgr::isBroadcasting(GnuID &id)
{
	Channel *ch = findChannelByID(id);
	if (ch)
		return ch->isBroadcasting();

	return false;
}
// -----------------------------------
bool	ChanMgr::isBroadcasting()
{
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
			if (ch->isBroadcasting())
				return true;

		ch = ch->next;
	}
	return false;
}

// -----------------------------------
int ChanMgr::numChannels()
{
	int tot = 0;
	Channel *ch = channel;
	while (ch)
	{
		if (ch->isActive())
			tot++;
		ch = ch->next;
	}
	return tot;
}

// -----------------------------------
void ChanMgr::deadHit(ChanHit &hit)
{
	ChanHitList *chl = findHitListByID(hit.chanID);
	if (chl)
		chl->deadHit(hit);
}
// -----------------------------------
void ChanMgr::delHit(ChanHit &hit)
{
	ChanHitList *chl = findHitListByID(hit.chanID);
	if (chl)
		chl->delHit(hit);
}

// -----------------------------------
void ChanMgr::addHit(Host &h,GnuID &id,bool tracker)
{
	ChanHit hit;
	hit.init();
	hit.host = h; 
	hit.rhost[0] = h;
	hit.rhost[1].init();
	hit.tracker = tracker;
	hit.recv = true;
	hit.chanID = id;
	addHit(hit);
}
// -----------------------------------
ChanHit *ChanMgr::addHit(ChanHit &h)
{
	if (searchActive)
		lastHit = sys->getTime();

	ChanHitList *hl=NULL;

	hl = findHitListByID(h.chanID);

	if (!hl)
	{
		ChanInfo info;
		info.id = h.chanID;
		hl = addHitList(info);
	}

	if (hl)
	{
		return hl->addHit(h);
	}else
		return NULL;
}
// -----------------------------------
bool ChanMgr::findParentHit(ChanHit &p)
{
	ChanHitList *hl=NULL;

	chanMgr->hitlistlock.on();

	hl = findHitListByID(p.chanID);

	if (hl)
	{
		ChanHit *ch = hl->hit;
		while (ch)
		{
			if (!ch->dead && (ch->rhost[0].ip == p.uphost.ip)
				&& (ch->rhost[0].port == p.uphost.port))
			{
				chanMgr->hitlistlock.off();
				return 1;
			}
			ch = ch->next;
		}
	}

	chanMgr->hitlistlock.off();

	return 0;
}

// -----------------------------------
class ChanFindInfo : public ThreadInfo
{
public:
	ChanInfo	info;
	bool	keep;
};
// -----------------------------------
THREAD_PROC findAndPlayChannelProc(ThreadInfo *th)
{
	ChanFindInfo *cfi = (ChanFindInfo *)th;

	ChanInfo info;
	info = cfi->info;


	Channel *ch = chanMgr->findChannelByNameID(info);

	chanMgr->currFindAndPlayChannel = info.id;

	if (!ch)
		ch = chanMgr->findAndRelay(info);

	if (ch)
	{
		// check that a different channel hasn`t be selected already.
		if (chanMgr->currFindAndPlayChannel.isSame(ch->info.id))
			chanMgr->playChannel(ch->info);

		if (cfi->keep)
			ch->stayConnected = cfi->keep;
	}

	delete cfi;
	return 0;
}
// -----------------------------------
void ChanMgr::findAndPlayChannel(ChanInfo &info, bool keep)
{
	ChanFindInfo *cfi = new ChanFindInfo;
	cfi->info = info;
	cfi->keep = keep;
	cfi->func = findAndPlayChannelProc;


	sys->startThread(cfi);
}
// -----------------------------------
void ChanMgr::playChannel(ChanInfo &info)
{

	char str[128],fname[256],idStr[128];

	sprintf(str,"http://localhost:%d",servMgr->serverHost.port);
	info.id.toStr(idStr);

	PlayList::TYPE type;


	if ((info.contentType == ChanInfo::T_WMA) || (info.contentType == ChanInfo::T_WMV))
	{
		type = PlayList::T_ASX;
		// WMP seems to have a bug where it doesn`t re-read asx files if they have the same name
		// so we prepend the channel id to make it unique - NOTE: should be deleted afterwards.
		if (servMgr->getModulePath) //JP-EX
		{
			peercastApp->getDirectory();
			sprintf(fname,"%s/%s.asx",servMgr->modulePath,idStr);	
		}else
			sprintf(fname,"%s/%s.asx",peercastApp->getPath(),idStr);
	}else if (info.contentType == ChanInfo::T_OGM)
	{
		type = PlayList::T_RAM;
		if (servMgr->getModulePath) //JP-EX
		{
			peercastApp->getDirectory();
			sprintf(fname,"%s/play.ram",servMgr->modulePath);
		}else
			sprintf(fname,"%s/play.ram",peercastApp->getPath());

	}else
	{
		type = PlayList::T_SCPLS;
		if (servMgr->getModulePath) //JP-EX
		{
			peercastApp->getDirectory();
			sprintf(fname,"%s/play.pls",servMgr->modulePath);
		}else
			sprintf(fname,"%s/play.pls",peercastApp->getPath());
	}


	PlayList *pls = new PlayList(type,1);
	pls->addChannel(str,info);


	LOG_DEBUG("Writing %s",fname);
	FileStream file;
	file.openWriteReplace(fname);
	pls->write(file);
	file.close();


	LOG_DEBUG("Executing: %s",fname);
	sys->executeFile(fname);
	delete pls;

}

// -----------------------------------
ChanHitList::ChanHitList()
{
	info.init();
	lastHitTime = 0;
	used = false;
	hit = NULL;
}
// -----------------------------------
ChanHitList::~ChanHitList()
{
	chanMgr->hitlistlock.on();
	while (hit)
		hit = deleteHit(hit);
	chanMgr->hitlistlock.off();
}
// -----------------------------------
void ChanHit::pickNearestIP(Host &h)
{
	for(int i=0; i<2; i++)
	{
		if (h.classType() == rhost[i].classType())
		{
			host = rhost[i];
			break;
		}
	}
}

// -----------------------------------
void ChanHit::init()
{
	host.init();

	rhost[0].init();
	rhost[1].init();

	next = NULL;

	numListeners = 0;
	numRelays = 0;
	clap_pp = 0; //JP-MOD

	dead = tracker = firewalled = stable = yp = false;
	recv = cin = direct = relay = true;
	relayfull = chfull = ratefull = false;
	direct = 0;
	numHops = 0;
	time = upTime = 0;
	lastContact = 0;

	version = 0;
	version_vp = 0;

	version_ex_prefix[0] = ' ';
	version_ex_prefix[1] = ' ';
	version_ex_number = 0;

	status = 0;
	servent_id = 0;

	sessionID.clear();
	chanID.clear();


	oldestPos = newestPos = 0;
	uphost.init();
	uphostHops = 0;
}

// -----------------------------------
void ChanHit::initLocal(int numl,int numr,int,int uptm,bool connected,bool isFull,unsigned int bitrate, Channel* ch, unsigned int oldp,unsigned int newp)
{
	init();
	firewalled = (servMgr->getFirewall() != ServMgr::FW_OFF);
	numListeners = numl;
	numRelays = numr;
	upTime = uptm;
	stable = servMgr->totalStreams>0;
	sessionID = servMgr->sessionID;
	recv = connected;

	direct = !servMgr->directFull();
//	relay = !servMgr->relaysFull();
	cin = !servMgr->controlInFull();

	relayfull = servMgr->relaysFull();
	chfull = isFull;

	Channel *c = chanMgr->channel;
	int noRelay = 0;
	unsigned int needRate = 0;
	unsigned int allRate = 0;
	while(c){
		if (c->isPlaying()){
			allRate += c->info.bitrate * c->localRelays();
			if ((c != ch) && (c->localRelays() == 0)){
				if(!isIndexTxt(c))	// for PCRaw (relay)
					noRelay++;
				needRate+=c->info.bitrate;
			}
		}
		c = c->next;
	}

	unsigned int numRelay = servMgr->numStreams(Servent::T_RELAY,false);
	int diff = servMgr->maxRelays - numRelay;
	if (ch->localRelays()){
		if (noRelay > diff){
			noRelay = diff;
		}
	} else {
		noRelay = 0;
		needRate = 0;
	}

//	ratefull = servMgr->bitrateFull(needRate+bitrate);
	ratefull = (servMgr->maxBitrateOut < allRate + needRate + ch->info.bitrate);

	if (!isIndexTxt(ch))
		relay =	(!relayfull) && (!chfull) && (!ratefull) && (numRelay + noRelay < servMgr->maxRelays);
	else
		relay =	(!chfull) && (!ratefull); // for PCRaw (relay)

/*	if (relayfull){
		LOG_DEBUG("Reject by relay full");
	}
	if (chfull){
		LOG_DEBUG("Reject by channel full");
	}
	if (ratefull){
		LOG_DEBUG("Reject by rate: Max=%d Now=%d Need=%d ch=%d", servMgr->maxBitrateOut, allRate, needRate, ch->info.bitrate);
	}*/

	host = servMgr->serverHost;

	version = PCP_CLIENT_VERSION;
	version_vp = PCP_CLIENT_VERSION_VP;
	if (VERSION_EX){
		strncpy(version_ex_prefix, PCP_CLIENT_VERSION_EX_PREFIX,2);
		version_ex_number = PCP_CLIENT_VERSION_EX_NUMBER;
	} else {
		version_ex_prefix[0] = ' ';
		version_ex_prefix[1] = ' ';
		version_ex_number = 0;
	}

	status = ch->status;

	rhost[0] = Host(host.ip,host.port);
	rhost[1] = Host(ClientSocket::getIP(NULL),host.port);

	if (firewalled)
		rhost[0].port = 0;

	oldestPos = oldp;
	newestPos = newp;

	uphost.ip = ch->sourceHost.host.ip;
	uphost.port = ch->sourceHost.host.port;
	uphostHops = 1;
}

// -----------------------------------
void ChanHit::initLocal_pp(bool isStealth, int numClaps) //JP-MOD
{
	numListeners = numListeners && !isStealth ? 1 : 0;
	clap_pp = numClaps;
}

// -----------------------------------
void ChanHit::writeAtoms(AtomStream &atom,GnuID &chanID)
{
	bool addChan=chanID.isSet();
	bool uphostdata=(uphost.ip != 0);

	int fl1 = 0; 
	if (recv) fl1 |= PCP_HOST_FLAGS1_RECV;
	if (relay) fl1 |= PCP_HOST_FLAGS1_RELAY;
	if (direct) fl1 |= PCP_HOST_FLAGS1_DIRECT;
	if (cin) fl1 |= PCP_HOST_FLAGS1_CIN;
	if (tracker) fl1 |= PCP_HOST_FLAGS1_TRACKER;
	if (firewalled) fl1 |= PCP_HOST_FLAGS1_PUSH;

	atom.writeParent(PCP_HOST,13  + (addChan?1:0) + (uphostdata?3:0) + (version_ex_number?2:0) + (clap_pp?1:0/*JP-MOD*/));

		if (addChan)
			atom.writeBytes(PCP_HOST_CHANID,chanID.id,16);
		atom.writeBytes(PCP_HOST_ID,sessionID.id,16);
		atom.writeInt(PCP_HOST_IP,rhost[0].ip);
		atom.writeShort(PCP_HOST_PORT,rhost[0].port);
		atom.writeInt(PCP_HOST_IP,rhost[1].ip);
		atom.writeShort(PCP_HOST_PORT,rhost[1].port);
		atom.writeInt(PCP_HOST_NUML,numListeners);
		atom.writeInt(PCP_HOST_NUMR,numRelays);
		atom.writeInt(PCP_HOST_UPTIME,upTime);
		atom.writeInt(PCP_HOST_VERSION,version);
		atom.writeInt(PCP_HOST_VERSION_VP,version_vp);
		if (version_ex_number){
			atom.writeBytes(PCP_HOST_VERSION_EX_PREFIX,version_ex_prefix,2);
			atom.writeShort(PCP_HOST_VERSION_EX_NUMBER,version_ex_number);
		}
		atom.writeChar(PCP_HOST_FLAGS1,fl1);
		atom.writeInt(PCP_HOST_OLDPOS,oldestPos);
		atom.writeInt(PCP_HOST_NEWPOS,newestPos);
		if (uphostdata){
			atom.writeInt(PCP_HOST_UPHOST_IP,uphost.ip);
			atom.writeInt(PCP_HOST_UPHOST_PORT,uphost.port);
			atom.writeInt(PCP_HOST_UPHOST_HOPS,uphostHops);
		}
		if (clap_pp){	//JP-MOD
			atom.writeInt(PCP_HOST_CLAP_PP,clap_pp);
		}
}
// -----------------------------------
bool	ChanHit::writeVariable(Stream &out, const String &var)
{
	char buf[1024];

	if (var == "rhost0")
	{
		if (servMgr->enableGetName) //JP-EX s
		{
			char buf2[256];
			if (firewalled) 
			{
				if (numRelays==0) 
					strcpy(buf,"<font color=red>");
				else 
					strcpy(buf,"<font color=orange>");
			}
			else {
				if (!relay){
					if (numRelays==0){
						strcpy(buf,"<font color=purple>");
					} else {
						strcpy(buf,"<font color=blue>");
					}
				} else {
					strcpy(buf,"<font color=green>");
				}
			}

			rhost[0].toStr(buf2);
			strcat(buf,buf2);

			char h_name[128];
			if (ClientSocket::getHostname(h_name,sizeof(h_name),rhost[0].ip)) //JP-MOD
			{
				strcat(buf,"[");
				strcat(buf,h_name);
				strcat(buf,"]");
			}
			strcat(buf,"</font>");
		} //JP-EX e
		else
			rhost[0].toStr(buf);
	}
	else if (var == "rhost1")
		rhost[1].toStr(buf);
	else if (var == "numHops")
		sprintf(buf,"%d",numHops);
	else if (var == "numListeners")
		sprintf(buf,"%d",numListeners);
	else if (var == "numRelays")
		sprintf(buf,"%d",numRelays);
	else if (var == "uptime")
	{
		String timeStr;
		timeStr.setFromStopwatch(upTime);
		strcpy(buf,timeStr.cstr());
	}else if (var == "update")
	{
		String timeStr;
		if (timeStr)
			timeStr.setFromStopwatch(sys->getTime()-time);
		else
			timeStr.set("-");
		strcpy(buf,timeStr.cstr());
	}else if (var == "isFirewalled"){
		sprintf(buf,"%d",firewalled?1:0);
	}else if (var == "version"){
		sprintf(buf,"%d",version);
	}else if (var == "agent"){
		if (version){
			if (version_ex_number){
				sprintf(buf, "v0.%d(%c%c%04d)", version, version_ex_prefix[0], version_ex_prefix[1], version_ex_number);
			} else if (version_vp){
				sprintf(buf,"v0.%d(VP%04d)", version, version_vp);
			} else {
				sprintf(buf,"v0.%d", version);
			}
		} else {
			strcpy(buf, "0");
		}
	}
	else if (var == "check")
	{
		char buf2[256];
		strcpy(buf, "<a href=\"#\" onclick=\"checkip('");
		rhost[0].IPtoStr(buf2);
		strcat(buf, buf2);
		strcat(buf, "')\">_</a>");
	}
	else if (var == "uphost")		// tree
		uphost.toStr(buf);
	else if (var == "uphostHops")	// tree
		sprintf(buf,"%d",uphostHops);
	else if (var == "canRelay")		// tree
		sprintf(buf, "%d",relay);
	else
		return false;

	out.writeString(buf);
	return true;
}

// -----------------------------------
int ChanHitList::getTotalListeners()
{
	int cnt=0;
	ChanHit *h = hit;
	while (h)
	{
		if (h->host.ip)
			cnt+=h->numListeners;
		h=h->next;
	}
	return cnt;
}
// -----------------------------------
int ChanHitList::getTotalRelays()
{
	int cnt=0;
	ChanHit *h = hit;
	while (h)
	{
		if (h->host.ip)
			cnt+=h->numRelays;
		h=h->next;
	}
	return cnt;
}
// -----------------------------------
int ChanHitList::getTotalFirewalled()
{
	int cnt=0;
	ChanHit *h = hit;
	while (h)
	{
		if (h->host.ip)
			if (h->firewalled)
				cnt++;
		h=h->next;
	}
	return cnt;
}

// -----------------------------------
int ChanHitList::contactTrackers(bool connected, int numl, int nums, int uptm)
{
	return 0;
}

void ChanHitList::clearHits(bool flg)
{
	ChanHit *c = hit, *prev = NULL;

	while(c){
		if (flg || (c->numHops != 0)){
			ChanHit *next = c->next;
			if (prev)
				prev->next = next;
			else
				hit = next;

			delete c;
			c = next;
		} else {
			prev = c;
			c = c->next;
		}
	}
}

// -----------------------------------
ChanHit *ChanHitList::deleteHit(ChanHit *ch)
{
	ChanHit *c = hit,*prev=NULL;
	while (c)
	{
		if (c == ch)
		{
			ChanHit *next = c->next;
			if (prev)
				prev->next = next;
			else
				hit = next;

			delete c;

			return next;
		}
		prev=c;
		c=c->next;
	}

	return NULL;
}
// -----------------------------------
ChanHit *ChanHitList::addHit(ChanHit &h)
{
	char ip0str[64],ip1str[64];
	h.rhost[0].toStr(ip0str);
	h.rhost[1].toStr(ip1str);
	char uphostStr[64];
	h.uphost.toStr(uphostStr);
	if (h.uphost.ip){
		LOG_DEBUG("Add hit: F%dT%dR%d %s/%s <- %s(%d)",h.firewalled,h.tracker,h.relay,ip0str,ip1str,uphostStr, h.uphostHops);
	} else {
		LOG_DEBUG("Add hit: F%dT%dR%d %s/%s",h.firewalled,h.tracker,h.relay,ip0str,ip1str);
	}

	// dont add our own hits
	if (servMgr->sessionID.isSame(h.sessionID))
		return NULL;


	lastHitTime = sys->getTime();
	h.time = lastHitTime;

	ChanHit *ch = hit;
	while (ch)
	{
		if ((ch->rhost[0].ip == h.rhost[0].ip) && (ch->rhost[0].port == h.rhost[0].port))
			if (((ch->rhost[1].ip == h.rhost[1].ip) && (ch->rhost[1].port == h.rhost[1].port)) || (!ch->rhost[1].isValid()))
			{
				if (!ch->dead)
				{
					if (ch->numHops > 0 && h.numHops == 0)
						// downstream hit recieved as RelayHost
						return ch;
					ChanHit *next = ch->next;
					*ch = h;
					ch->next = next;
					return ch;
				}
			}
		ch=ch->next;
	}

	// clear hits with same session ID (IP may have changed)
	if (h.sessionID.isSet())
	{
		ChanHit *ch = hit;
		while (ch)
		{
			if (ch->host.ip)
				if (ch->sessionID.isSame(h.sessionID))
				{
					ch = deleteHit(ch);
					continue;						
				}
			ch=ch->next;
		}
	}


	// else add new hit
	{
		ChanHit *ch = new ChanHit();
		*ch = h;
		ch->chanID = info.id;
		ch->next = hit;
		hit = ch;
		return ch;
	}

	return NULL;
}


// -----------------------------------
int	ChanHitList::clearDeadHits(unsigned int timeout, bool clearTrackers)
{
	int cnt=0;
	unsigned int ctime = sys->getTime();

//	LOG_DEBUG("clearDeadHits HITLISTLOCK ON-------------");
	chanMgr->hitlistlock.on();
	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip)
		{
			if (ch->dead || ((ctime-ch->time) > timeout) && (clearTrackers || (!clearTrackers & !ch->tracker)))
			{
//				ch = deleteHit(ch);

				if (ch->firewalled){
//					LOG_DEBUG("kickKeepTime = %d, %d", servMgr->kickKeepTime, ctime-ch->time);
					if ( (servMgr->kickKeepTime == 0) || ((ctime-ch->time) > servMgr->kickKeepTime) ){
						ch = deleteHit(ch);
					} else {
						ch->numHops = 0;
						ch->numListeners = 0;
						ch = ch->next;
					}
				} else {
					ch = deleteHit(ch);
				}

				continue;
			}else
				cnt++;
		}
		ch = ch->next;
	}
//	LOG_DEBUG("clearDeadHits HITLISTLOCK OFF-------------");
	chanMgr->hitlistlock.off();
	return cnt;
}


// -----------------------------------
void	ChanHitList::deadHit(ChanHit &h)
{
	char ip0str[64],ip1str[64];
	h.rhost[0].toStr(ip0str);
	h.rhost[1].toStr(ip1str);
	LOG_DEBUG("Dead hit: %s/%s",ip0str,ip1str);

	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip)
			if (ch->rhost[0].isSame(h.rhost[0]) && ch->rhost[1].isSame(h.rhost[1]))
			{
				ch->dead = true;
			}
		ch = ch->next;
	}
}
// -----------------------------------
void	ChanHitList::delHit(ChanHit &h)
{
	char ip0str[64],ip1str[64];
	h.rhost[0].toStr(ip0str);
	h.rhost[1].toStr(ip1str);
	LOG_DEBUG("Del hit: %s/%s",ip0str,ip1str);

	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip)
			if (ch->rhost[0].isSame(h.rhost[0]) && ch->rhost[1].isSame(h.rhost[1]))
			{
				ch=deleteHit(ch);
				continue;
			}
		ch = ch->next;
	}
}
// -----------------------------------
int	ChanHitList::numHits()
{
	int cnt=0;
	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip && !ch->dead && ch->numHops)
			cnt++;
		ch = ch->next;
	}

	return cnt;
}
// -----------------------------------
int	ChanHitList::numListeners()
{
	int cnt=0;
	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip && !ch->dead && ch->numHops)
			cnt += (unsigned int)ch->numListeners > 3 ? 3 : ch->numListeners; //JP-MOD ƒŠƒXƒi[”‹U‘•–hŽ~
		ch=ch->next;
	}

	return cnt;
}
// -----------------------------------
int ChanHitList::numClaps()	//JP-MOD
{
	int cnt=0;
	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip && !ch->dead && ch->numHops && (ch->clap_pp & 1)){
			cnt++;
		}
		ch=ch->next;
	}

	return cnt;
}
// -----------------------------------
int	ChanHitList::numRelays()
{
	int cnt=0;
	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip && !ch->dead)
			cnt += ch->numRelays;
		ch=ch->next;
	}

	return cnt;
}

// -----------------------------------
int	ChanHitList::numTrackers()
{
	int cnt=0;
	ChanHit *ch = hit;
	while (ch)
	{
		if ((ch->host.ip && !ch->dead) && (ch->tracker))
			cnt++;
		ch=ch->next;
	}
	return cnt;
}
// -----------------------------------
int	ChanHitList::numFirewalled()
{
	int cnt=0;
	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip && !ch->dead)
			cnt += ch->firewalled?1:0;
		ch=ch->next;
	}
	return cnt;
}
// -----------------------------------
int	ChanHitList::closestHit()
{
	unsigned int hop=10000;
	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip && !ch->dead)
			if (ch->numHops < hop)
				hop = ch->numHops;
		ch=ch->next;
	}

	return hop;
}
// -----------------------------------
int	ChanHitList::furthestHit()
{
	unsigned int hop=0;
	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip && !ch->dead)
			if (ch->numHops > hop)
				hop = ch->numHops;
		ch=ch->next;
	}

	return hop;
}
// -----------------------------------
unsigned int	ChanHitList::newestHit()
{
	unsigned int time=0;
	ChanHit *ch = hit;
	while (ch)
	{
		if (ch->host.ip && !ch->dead)
			if (ch->time > time)
				time = ch->time;
		ch=ch->next;
	}

	return time;
}
// -----------------------------------
int ChanHitList::pickHits(ChanHitSearch &chs)
{
	ChanHit best,*bestP=NULL;
	best.init();
	best.numHops = 255;
	best.time = 0;

	unsigned int ctime = sys->getTime();

	ChanHit *c = hit;
	while (c)
	{
		if (c->host.ip && !c->dead)
		{
			if (!chs.excludeID.isSame(c->sessionID))
			if ((chs.waitDelay==0) || ((ctime-c->lastContact) >= chs.waitDelay))
			if ((c->numHops<=best.numHops))	// (c->time>=best.time))
			if (c->relay || (!c->relay && chs.useBusyRelays))
			if (c->cin || (!c->cin && chs.useBusyControls))
			{

				if (chs.trackersOnly && c->tracker)
				{
					if (chs.matchHost.ip)
					{
						if ((c->rhost[0].ip == chs.matchHost.ip) && c->rhost[1].isValid())
						{
							bestP = c;
							best = *c;
							best.host = best.rhost[1];	// use lan ip
						}
					}else if (c->firewalled == chs.useFirewalled)
					{
						bestP = c;
						best = *c;
						best.host = best.rhost[0];			// use wan ip
					}
				}else if (!chs.trackersOnly && !c->tracker)
				{
					if (chs.matchHost.ip)
					{
						if ((c->rhost[0].ip == chs.matchHost.ip) && c->rhost[1].isValid())
						{
							bestP = c;
							best = *c;
							best.host = best.rhost[1];	// use lan ip
						}
					}else if (c->firewalled == chs.useFirewalled && (!bestP || !bestP->relay))
					{
						bestP = c;
						best = *c;
						best.host = best.rhost[0];			// use wan ip
					}
				}
			}
		}
		c=c->next;
	}

	if (bestP)
	{
		if (chs.numResults < ChanHitSearch::MAX_RESULTS)
		{
			if (chs.waitDelay)
				bestP->lastContact = ctime;
			chs.best[chs.numResults++] = best;
			return 1;
		}

	}

	return 0;
}


// -----------------------------------
int ChanHitList::pickSourceHits(ChanHitSearch &chs)
{
	if (pickHits(chs) && chs.best[0].numHops == 0) return 1;
	return 0;
}

// -----------------------------------
unsigned int ChanHitList::getSeq()
{
	unsigned int seq;
	seqLock.on();
	seq = riSequence = (riSequence + 1) & 0xffffff;
	seqLock.off();
	return seq;
}

// -----------------------------------
const char *ChanInfo::getTypeStr(TYPE t)
{
	switch (t)
	{
		case T_RAW: return "RAW";

		case T_MP3: return "MP3";
		case T_OGG: return "OGG";
		case T_OGM: return "OGM";
		case T_WMA: return "WMA";

		case T_MOV: return "MOV";
		case T_MPG: return "MPG";
		case T_NSV: return "NSV";
		case T_WMV: return "WMV";

		case T_PLS: return "PLS";
		case T_ASX: return "ASX";

		default: return "UNKNOWN";
	}
}
// -----------------------------------
const char *ChanInfo::getProtocolStr(PROTOCOL t)
{
	switch (t)
	{
		case SP_PEERCAST: return "PEERCAST";
		case SP_HTTP: return "HTTP";
		case SP_FILE: return "FILE";
		case SP_MMS: return "MMS";
		case SP_PCP: return "PCP";
		default: return "UNKNOWN";
	}
}
// -----------------------------------
ChanInfo::PROTOCOL ChanInfo::getProtocolFromStr(const char *str)
{
	if (stricmp(str,"PEERCAST")==0)
		return SP_PEERCAST;
	else if (stricmp(str,"HTTP")==0)
		return SP_HTTP;
	else if (stricmp(str,"FILE")==0)
		return SP_FILE;
	else if (stricmp(str,"MMS")==0)
		return SP_MMS;
	else if (stricmp(str,"PCP")==0)
		return SP_PCP;
	else 
		return SP_UNKNOWN;
}

// -----------------------------------
const char *ChanInfo::getTypeExt(TYPE t)
{
	switch(t)
	{
		case ChanInfo::T_OGM:
		case ChanInfo::T_OGG:
			return ".ogg";
		case ChanInfo::T_MP3:
			return ".mp3";
		case ChanInfo::T_MOV:
			return ".mov";
		case ChanInfo::T_NSV:
			return ".nsv";
		case ChanInfo::T_WMV:
			return ".wmv";
		case ChanInfo::T_WMA:
			return ".wma";
		default:
			return "";
	}
}
// -----------------------------------
ChanInfo::TYPE ChanInfo::getTypeFromStr(const char *str)
{
	if (stricmp(str,"MP3")==0)
		return T_MP3;
	else if (stricmp(str,"OGG")==0)
		return T_OGG;
	else if (stricmp(str,"OGM")==0)
		return T_OGM;
	else if (stricmp(str,"RAW")==0)
		return T_RAW;
	else if (stricmp(str,"NSV")==0)
		return T_NSV;
	else if (stricmp(str,"WMA")==0)
		return T_WMA;
	else if (stricmp(str,"WMV")==0)
		return T_WMV;
	else if (stricmp(str,"PLS")==0)
		return T_PLS;
	else if (stricmp(str,"M3U")==0)
		return T_PLS;
	else if (stricmp(str,"ASX")==0)
		return T_ASX;
	else 
		return T_UNKNOWN;
}
// -----------------------------------
bool	ChanInfo::matchNameID(ChanInfo &inf)
{
	if (inf.id.isSet())
		if (id.isSame(inf.id))
			return true;

	if (!inf.name.isEmpty())
		if (name.contains(inf.name))
			return true;

	return false;
}
// -----------------------------------
bool	ChanInfo::match(ChanInfo &inf)
{
	bool matchAny=true;

	if (inf.status != S_UNKNOWN)
	{
		if (status != inf.status)
			return false;
	}

	if (inf.bitrate != 0)
	{
		if (bitrate == inf.bitrate)
			return true;
		matchAny = false;
	}

	if (inf.id.isSet())
	{
		if (id.isSame(inf.id))
			return true;
		matchAny = false;
	}

	if (inf.contentType != T_UNKNOWN)
	{
		if (contentType == inf.contentType)
			return true;
		matchAny = false;
	}

	if (!inf.name.isEmpty())
	{
		if (name.contains(inf.name))
			return true;
		matchAny = false;
	}

	if (!inf.genre.isEmpty())
	{
		if (genre.contains(inf.genre))
			return true;
		matchAny = false;
	}

	return matchAny;
}
// -----------------------------------
bool TrackInfo::update(TrackInfo &inf)
{
	bool changed = false;

	if (!contact.isSame(inf.contact))
	{
		contact = inf.contact;
		changed = true;
	}

	if (!title.isSame(inf.title))
	{
		title = inf.title;
		changed = true;
	}

	if (!artist.isSame(inf.artist))
	{
		artist = inf.artist;
		changed = true;
	}

	if (!album.isSame(inf.album))
	{
		album = inf.album;
		changed = true;
	}

	if (!genre.isSame(inf.genre))
	{
		genre = inf.genre;
		changed = true;
	}


	return changed;
}


// -----------------------------------
bool ChanInfo::update(ChanInfo &info)
{
	bool changed = false;



	// check valid id
	if (!info.id.isSet())
		return false;

	// only update from chaninfo that has full name etc..
	if (info.name.isEmpty())
		return false;

	// check valid broadcaster key
	if (bcID.isSet())
	{
		if (!bcID.isSame(info.bcID))
		{
			LOG_ERROR("ChanInfo BC key not valid");
			return false;
		}
	}else
	{
		bcID = info.bcID;
	}



	if (bitrate != info.bitrate)
	{
		bitrate = info.bitrate;
		changed = true;
	}

	if (contentType != info.contentType)
	{
		contentType = info.contentType;
		changed = true;
	}

	if(ppFlags != info.ppFlags) //JP-MOD
	{
		ppFlags = info.ppFlags;
		changed = true;
	}

	if (!desc.isSame(info.desc)) //JP-EX
	{
		desc = info.desc;
		changed = true;
	}

	if (!name.isSame(info.name))
	{
		name = info.name;
		changed = true;
	}

	if (!comment.isSame(info.comment))
	{
		comment = info.comment;
		changed = true;
	}

	if (!genre.isSame(info.genre))
	{
		genre = info.genre;
		changed = true;
	}
	
	if (!url.isSame(info.url))
	{
		url = info.url;
		changed = true;
	}

	if (track.update(info.track))
		changed = true;


	return changed;
}
// -----------------------------------
void ChanInfo::initNameID(const char *n)
{
	init();
	id.fromStr(n);
	if (!id.isSet())
		name.set(n);
}

// -----------------------------------
void ChanInfo::init()
{
	status = S_UNKNOWN;
	name.clear();
	bitrate = 0;
	contentType = T_UNKNOWN;
	srcProtocol = SP_UNKNOWN;
	id.clear();
	url.clear();
	genre.clear();
	comment.clear();
	track.clear();
	lastPlayStart = 0;
	lastPlayEnd = 0;
	numSkips = 0;
	bcID.clear();
	createdTime = 0;
	ppFlags = 0; //JP-MOD
}
// -----------------------------------
void ChanInfo::readTrackXML(XML::Node *n)
{
	track.clear();
	readXMLString(track.title,n,"title");
	readXMLString(track.contact,n,"contact");
	readXMLString(track.artist,n,"artist");
	readXMLString(track.album,n,"album");
	readXMLString(track.genre,n,"genre");
}
// -----------------------------------
unsigned int ChanInfo::getUptime()
{
	// calculate uptime and cap if requested by settings.
	unsigned int upt;
	upt = lastPlayStart?(sys->getTime()-lastPlayStart):0;
	if (chanMgr->maxUptime)
		if (upt > chanMgr->maxUptime)
			upt = chanMgr->maxUptime;
	return upt;
}
// -----------------------------------
unsigned int ChanInfo::getAge()
{
	return sys->getTime()-createdTime;
}

// ------------------------------------------
void ChanInfo::readTrackAtoms(AtomStream &atom,int numc)
{
	for(int i=0; i<numc; i++)
	{
		int c,d;
		ID4 id = atom.read(c,d);
		if (id == PCP_CHAN_TRACK_TITLE)
		{
			atom.readString(track.title.data,sizeof(track.title.data),d);
		}else if (id == PCP_CHAN_TRACK_CREATOR)
		{
			atom.readString(track.artist.data,sizeof(track.artist.data),d);
		}else if (id == PCP_CHAN_TRACK_URL)
		{
			atom.readString(track.contact.data,sizeof(track.contact.data),d);
		}else if (id == PCP_CHAN_TRACK_ALBUM)
		{
			atom.readString(track.album.data,sizeof(track.album.data),d);
		}else
			atom.skip(c,d);
	}
}
// ------------------------------------------
void ChanInfo::readInfoAtoms(AtomStream &atom,int numc)
{
	for(int i=0; i<numc; i++)
	{
		int c,d;
		ID4 id = atom.read(c,d);
		if (id == PCP_CHAN_INFO_NAME)
		{
			atom.readString(name.data,sizeof(name.data),d);
		}else if (id == PCP_CHAN_INFO_BITRATE)
		{
			bitrate = atom.readInt();
		}else if (id == PCP_CHAN_INFO_GENRE)
		{
			atom.readString(genre.data,sizeof(genre.data),d);
		}else if (id == PCP_CHAN_INFO_URL)
		{
			atom.readString(url.data,sizeof(url.data),d);
		}else if (id == PCP_CHAN_INFO_DESC)
		{
			atom.readString(desc.data,sizeof(desc.data),d);
		}else if (id == PCP_CHAN_INFO_COMMENT)
		{
			atom.readString(comment.data,sizeof(comment.data),d);
		}else if (id == PCP_CHAN_INFO_TYPE)
		{
			char type[16];
			atom.readString(type,sizeof(type),d);
			contentType = ChanInfo::getTypeFromStr(type);
		}else if (id == PCP_CHAN_INFO_PPFLAGS) //JP-MOD
		{
			ppFlags = (unsigned int)atom.readInt();
		}else
			atom.skip(c,d);
	}	
}

// -----------------------------------
void ChanInfo::writeInfoAtoms(AtomStream &atom)
{
	atom.writeParent(PCP_CHAN_INFO,7 + (ppFlags ? 1:0/*JP-MOD*/));
		atom.writeString(PCP_CHAN_INFO_NAME,name.cstr());
		atom.writeInt(PCP_CHAN_INFO_BITRATE,bitrate);
		atom.writeString(PCP_CHAN_INFO_GENRE,genre.cstr());
		atom.writeString(PCP_CHAN_INFO_URL,url.cstr());
		atom.writeString(PCP_CHAN_INFO_DESC,desc.cstr());
		atom.writeString(PCP_CHAN_INFO_COMMENT,comment.cstr());
		atom.writeString(PCP_CHAN_INFO_TYPE,getTypeStr(contentType));	
		if(ppFlags)
			atom.writeInt(PCP_CHAN_INFO_PPFLAGS,ppFlags); //JP-MOD

}
// -----------------------------------
void ChanInfo::writeTrackAtoms(AtomStream &atom)
{
	atom.writeParent(PCP_CHAN_TRACK,4);
		atom.writeString(PCP_CHAN_TRACK_TITLE,track.title.cstr());
		atom.writeString(PCP_CHAN_TRACK_CREATOR,track.artist.cstr());
		atom.writeString(PCP_CHAN_TRACK_URL,track.contact.cstr());
		atom.writeString(PCP_CHAN_TRACK_ALBUM,track.album.cstr());
}


// -----------------------------------
XML::Node *ChanInfo::createChannelXML()
{
	char idStr[64];

	String nameUNI = name;
	nameUNI.convertTo(String::T_UNICODESAFE);

	String urlUNI = url;
	urlUNI.convertTo(String::T_UNICODESAFE);

	String genreUNI = genre;
	genreUNI.convertTo(String::T_UNICODESAFE);

	String descUNI = desc;
	descUNI.convertTo(String::T_UNICODESAFE);

	String commentUNI;
	commentUNI = comment;
	commentUNI.convertTo(String::T_UNICODESAFE);


	id.toStr(idStr);


	return new XML::Node("channel name=\"%s\" id=\"%s\" bitrate=\"%d\" type=\"%s\" genre=\"%s\" desc=\"%s\" url=\"%s\" uptime=\"%d\" comment=\"%s\" skips=\"%d\" age=\"%d\" bcflags=\"%d\"",
		nameUNI.cstr(),
		idStr,
		bitrate,
		getTypeStr(contentType),
		genreUNI.cstr(),
		descUNI.cstr(),
		urlUNI.cstr(),
		getUptime(),
		commentUNI.cstr(),
		numSkips,
		getAge(),
		bcID.getFlags()
		);	
}

// -----------------------------------
XML::Node *ChanInfo::createQueryXML()
{
	char buf[512];
	char idStr[64];


	String nameHTML = name;
	nameHTML.convertTo(String::T_HTML);
	String genreHTML = genre;
	genreHTML.convertTo(String::T_HTML);

	buf[0]=0;
	if (!nameHTML.isEmpty())
	{
		strcat(buf," name=\"");
		strcat(buf,nameHTML.cstr());
		strcat(buf,"\"");
	}

	if (!genreHTML.isEmpty())
	{
		strcat(buf," genre=\"");
		strcat(buf,genreHTML.cstr());
		strcat(buf,"\"");
	}

	if (id.isSet())
	{
		id.toStr(idStr);
		strcat(buf," id=\"");
		strcat(buf,idStr);
		strcat(buf,"\"");
	}
		

	return new XML::Node("channel %s",buf);
}

// -----------------------------------
XML::Node *ChanInfo::createRelayChannelXML()
{
	char idStr[64];

	id.toStr(idStr);


	return new XML::Node("channel id=\"%s\" uptime=\"%d\" skips=\"%d\" age=\"%d\"",
		idStr,
		getUptime(),
		numSkips,
		getAge()
		);	
}// -----------------------------------
XML::Node *ChanInfo::createTrackXML()
{
	String titleUNI = track.title;
	titleUNI.convertTo(String::T_UNICODESAFE);

	String artistUNI = track.artist;
	artistUNI.convertTo(String::T_UNICODESAFE);

	String albumUNI = track.album;
	albumUNI.convertTo(String::T_UNICODESAFE);

	String genreUNI = track.genre;
	genreUNI.convertTo(String::T_UNICODESAFE);

	String contactUNI = track.contact;
	contactUNI.convertTo(String::T_UNICODESAFE);
	


	return new XML::Node("track title=\"%s\" artist=\"%s\" album=\"%s\" genre=\"%s\" contact=\"%s\"",
		titleUNI.cstr(),
		artistUNI.cstr(),
		albumUNI.cstr(),
		genreUNI.cstr(),
		contactUNI.cstr()
		);
}

// -----------------------------------
void ChanInfo::init(XML::Node *n)
{
	init();

	updateFromXML(n);
}
// -----------------------------------
void ChanInfo::updateFromXML(XML::Node *n)
{
	String typeStr,idStr;

	readXMLString(name,n,"name");
	readXMLString(genre,n,"genre");
	readXMLString(url,n,"url");
	readXMLString(desc,n,"desc");


	int br = n->findAttrInt("bitrate");
	if (br)
		bitrate = br;

	{ //JP-MOD
		ppFlags = ServMgr::bcstNone;

		if (n->findAttrInt("bcstClap"))
			ppFlags |= ServMgr::bcstClap;
	}

	readXMLString(typeStr,n,"type");
	if (!typeStr.isEmpty())
		contentType = getTypeFromStr(typeStr.cstr());


	readXMLString(idStr,n,"id");
	if (!idStr.isEmpty())
		id.fromStr(idStr.cstr());

	readXMLString(comment,n,"comment");

	XML::Node *tn = n->findNode("track");
	if (tn)
		readTrackXML(tn);

}

// -----------------------------------
void ChanInfo::init(const char *n, GnuID &cid, TYPE tp, int br)
{
	init();

	name.set(n);
	bitrate = br;
	contentType = tp;
	id = cid;
}

// -----------------------------------
void ChanInfo::init(const char *fn)
{
	init();

	if (fn)
		name.set(fn);
}
// -----------------------------------
void PlayList::readASX(Stream &in)
{
	LOG_DEBUG("Reading ASX");
	XML xml;

	try
	{
		xml.read(in);
	}catch(StreamException &) {} // TODO: eof is NOT handled properly in sockets - always get error at end

	if (xml.root)
	{
		XML::Node *n = xml.root->child;
		while (n)
		{
			if (stricmp("entry",n->getName())==0)
			{
				XML::Node *rf = n->findNode("ref");
				if (rf)
				{
					char *hr = rf->findAttr("href");
					if (hr)
					{
						addURL(hr,"","");
						//LOG("asx url %s",hr);
					}

				}
			}
			n=n->sibling;
		}
	}
}
// -----------------------------------
void PlayList::readSCPLS(Stream &in)
{
	char tmp[256];
	while (in.readLine(tmp,sizeof(tmp)))
	{
		if (strnicmp(tmp,"file",4)==0)
		{
			char *p = strstr(tmp,"=");
			if (p)
				addURL(p+1,"","");
		}
	}
}
// -----------------------------------
void PlayList::readPLS(Stream &in)
{
	char tmp[256];
	while (in.readLine(tmp,sizeof(tmp)))
	{
		if (tmp[0] != '#')
			addURL(tmp,"","");
	}
}
// -----------------------------------
void PlayList::writeSCPLS(Stream &out)
{
	out.writeLine("[playlist]");
	out.writeLine("");
	out.writeLineF("NumberOfEntries=%d",numURLs);

	for(int i=0; i<numURLs; i++)
	{
		out.writeLineF("File%d=%s",i+1,urls[i].cstr());
		out.writeLineF("Title%d=%s",i+1,titles[i].cstr());
		out.writeLineF("Length%d=-1",i+1);
	}
	out.writeLine("Version=2");
}
// -----------------------------------
void PlayList::writePLS(Stream &out)
{
	for(int i=0; i<numURLs; i++)
		out.writeLineF("%s",urls[i].cstr());
}
// -----------------------------------
void PlayList::writeRAM(Stream &out)
{
	for(int i=0; i<numURLs; i++)
		out.writeLineF("%s",urls[i].cstr());
}

#define isHTMLSPECIAL(a) ((a == '&') || (a == '\"') || (a == '\'') || (a == '<') || (a == '>'))
static void SJIStoSJISSAFE(char *string, size_t size)
{
	size_t pos;
	for(pos = 0;
		(string[pos] != '\0') && (pos < size);
		++pos)
	{
		if(isHTMLSPECIAL(string[pos]))
			string[pos] = ' ';
	}
}

static void WriteASXInfo(Stream &out, String &title, String &contacturl, String::TYPE tEncoding = String::T_UNICODESAFE) //JP-MOD
{
	if(!title.isEmpty())
	{
		String titleEncode;
		titleEncode = title;
		titleEncode.convertTo(tEncoding);
#ifdef _WIN32
		if(tEncoding == String::T_SJIS)
			SJIStoSJISSAFE(titleEncode.cstr(), String::MAX_LEN);
#endif
		out.writeLineF("<TITLE>%s</TITLE>", titleEncode.cstr());
	}

	if(!contacturl.isEmpty())
	{
		String contacturlEncode;
		contacturlEncode = contacturl;
		contacturlEncode.convertTo(tEncoding);
#ifdef _WIN32
		if(tEncoding == String::T_SJIS)
			SJIStoSJISSAFE(contacturlEncode.cstr(), String::MAX_LEN);
#endif
		out.writeLineF("<MOREINFO HREF = \"%s\" />", contacturlEncode.cstr());
	}
}

// -----------------------------------
void PlayList::writeASX(Stream &out)
{
	out.writeLine("<ASX Version=\"3.0\">");

#ifdef _WIN32
	String::TYPE tEncoding = String::T_SJIS;
#else
	String::TYPE tEncoding = String::T_UNICODESAFE;
#endif
	if(servMgr->asxDetailedMode == 2)
	{
		out.writeLine("<PARAM NAME = \"Encoding\" VALUE = \"utf-8\" />"); //JP-MOD Memo: UTF-8 cannot be used in some recording software.
		tEncoding = String::T_UNICODESAFE;
	}

	if(servMgr->asxDetailedMode)
		WriteASXInfo(out, titles[0], contacturls[0], tEncoding); //JP-MOD

	for(int i=0; i<numURLs; i++)
	{
		out.writeLine("<ENTRY>");
		if(servMgr->asxDetailedMode)
			WriteASXInfo(out, titles[i], contacturls[i], tEncoding); //JP-MOD
		out.writeLineF("<REF href = \"%s\" />",urls[i].cstr());
		out.writeLine("</ENTRY>");
	}
	out.writeLine("</ASX>");
}


// -----------------------------------
void PlayList::addChannel(const char *path, ChanInfo &info)
{
	String url;

	char idStr[64];

	info.id.toStr(idStr);
	char *nid = info.id.isSet()?idStr:info.name.cstr();

	sprintf(url.cstr(),"%s/stream/%s%s",path,nid,ChanInfo::getTypeExt(info.contentType));
	addURL(url.cstr(),info.name,info.url);
}

// -----------------------------------
void ChanHitSearch::init()
{
	matchHost.init();
	waitDelay = 0;
	useFirewalled = false;
	trackersOnly = false;
	useBusyRelays = true;
	useBusyControls = true;
	excludeID.clear();
	numResults = 0;

	//seed = sys->getTime();
	//srand(seed);
}

int ChanHitSearch::getRelayHost(Host host1, Host host2, GnuID exID, ChanHitList *chl)
{
	int cnt = 0;
	int loop = 1;
	int index = 0;
	int prob;
	int rnd;
	int base = 0x400;
	ChanHit tmpHit[MAX_RESULTS];

	//srand(seed);
	//seed += 11;

	unsigned int seq = chl->getSeq();

	ChanHit *hit = chl->hit;

	while(hit){
		if (hit->rhost[0].ip && !hit->dead){
			if (
				(!exID.isSame(hit->sessionID))
//			&&	(hit->relay)
			&&	(!hit->tracker)
			&&	(!hit->firewalled)
			&&	(hit->numHops != 0)
			){
				if (	(hit->rhost[0].ip == host1.ip)
					&&	hit->rhost[1].isValid()
					&&	(host2.ip != hit->rhost[1].ip)
				){
					best[0] = *hit;
					best[0].host = hit->rhost[1];
					index++;
				}
				if ((hit->rhost[0].ip == host2.ip) && hit->rhost[1].isValid()){
					best[0] = *hit;
					best[0].host = hit->rhost[1];
					index++;
				}

				loop = (index / MAX_RESULTS) + 1;
				//prob = (float)1 / (float)loop;
				prob = base / loop;
				//rnd = (float)rand() / (float)RAND_MAX;
				rnd = rand() % base;
				if (hit->numHops == 1){
					if (tmpHit[index % MAX_RESULTS].numHops == 1){
						if (rnd < prob){
							tmpHit[index % MAX_RESULTS] = *hit;
							tmpHit[index % MAX_RESULTS].host = hit->rhost[0];
							index++;
						}
					} else {
						tmpHit[index % MAX_RESULTS] = *hit;
						tmpHit[index % MAX_RESULTS].host = hit->rhost[0];
						index++;
					}
				} else {
					if ((tmpHit[index % MAX_RESULTS].numHops != 1) && (rnd < prob)){
						tmpHit[index % MAX_RESULTS] = *hit;
						tmpHit[index % MAX_RESULTS].host = hit->rhost[0];
						index++;
					}
				}

//				char tmp[50];
//				hit->host.toStr(tmp);
//				LOG_DEBUG("TEST %s: %f %f", tmp, rnd, prob);
			}
		}
		hit = hit->next;
	}
	
	if (index > MAX_RESULTS){
		cnt = MAX_RESULTS;
	} else {
		cnt = index;
	}

/*	int use[MAX_RESULTS];
	memset(use, 0, sizeof(use));
	int i;
	for (i = 0; i < cnt; i++){
		use[i] = -1;
	}
	int r;
	for (i = 0; i < cnt; i++){
		do {
			r = rand();
//			LOG_DEBUG("%d",r);
			r = r % cnt;
			if (use[r] == -1){
				use[r] = i;
				break;
			}
		} while(1);
	}
	for (i = 0; i < cnt; i++){
//		LOG_DEBUG("%d", use[i]);
		best[use[i]] = tmpHit[i];
	}*/

	for (int i = 0; i < cnt; i++){
//		LOG_DEBUG("%d", use[i]);
		best[(i + seq) % cnt] = tmpHit[i];
	}
//	for (i = 0; i < cnt; i++){
//		char tmp[50];
//		best[i].host.toStr(tmp);
//		LOG_DEBUG("Relay info: Hops = %d, %s", best[i].numHops, tmp);
//	}

	return cnt;
}
