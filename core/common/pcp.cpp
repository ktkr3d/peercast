// ------------------------------------------------
// File : pcp.cpp
// Date: 1-mar-2004
// Author: giles
//
// (c) 2002-4 peercast.org
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

#include "atom.h"
#include "pcp.h"
#include "peercast.h"
#include "version2.h"
#ifdef _DEBUG
#include "chkMemoryLeak.h"
#define DEBUG_NEW new(__FILE__, __LINE__)
#define new DEBUG_NEW
#endif

// ------------------------------------------
void PCPStream::init(GnuID &rid)
{
	remoteID = rid;
	routeList.clear();

	lastPacketTime = 0;
	nextRootPacket = 0;	 // 0 seconds (never)

	inData.init();
	inData.accept = ChanPacket::T_PCP;

	outData.init();
	outData.accept = ChanPacket::T_PCP;
}
// ------------------------------------------
void PCPStream::readVersion(Stream &in)
{
	int len = in.readInt();

	if (len != 4)
		throw StreamException("Invalid PCP");

	int ver = in.readInt();

	LOG_DEBUG("PCP ver: %d",ver);
}
// ------------------------------------------
void PCPStream::readHeader(Stream &in,Channel *)
{
//	AtomStream atom(in);

//	if (in.readInt() != PCP_CONNECT)
//		throw StreamException("Not PCP");

//	readVersion(in);
}
// ------------------------------------------
bool PCPStream::sendPacket(ChanPacket &pack,GnuID &destID)
{
	if (destID.isSet())
		if (!destID.isSame(remoteID))
			if (!routeList.contains(destID))
				return false;

	return outData.writePacket(pack);
}
// ------------------------------------------
void PCPStream::flush(Stream &in)
{
	ChanPacket pack;
	// send outward packets
	while (outData.numPending())
	{
		outData.readPacket(pack);
		pack.writeRaw(in);
	}
}
// ------------------------------------------
unsigned int PCPStream::flushUb(Stream &in, unsigned int size)
{
	ChanPacket pack;
	unsigned int len = 0, skip = 0;

	while (outData.numPending())
	{
		outData.readPacketPri(pack);

		if (size >= len + pack.len) {
			len += pack.len;
			pack.writeRaw(in);
		} else {
			skip++;
		}
	}
	if (skip > 0)
		LOG_DEBUG("PCPStream::flushUb: skip %d packets", skip);

	return len;
}
// ------------------------------------------
int PCPStream::readPacket(Stream &in,Channel *)
{
	BroadcastState bcs;
	bcs.ttl = 1;
	return readPacket(in,bcs);
}
// ------------------------------------------
int PCPStream::readPacket(Stream &in,BroadcastState &bcs)
{
	int error = PCP_ERROR_GENERAL;
	try
	{
		AtomStream atom(in);

		ChanPacket pack;
		MemoryStream mem(pack.data,sizeof(pack.data));
		AtomStream patom(mem);


		// send outward packets
		error = PCP_ERROR_WRITE;
		if (outData.numPending())
		{
			outData.readPacket(pack);
			pack.writeRaw(in);
		}
		error = PCP_ERROR_GENERAL;

		if (outData.willSkip())
		{
			error = PCP_ERROR_WRITE+PCP_ERROR_SKIP;
			throw StreamException("Send too slow");
		}


		error = PCP_ERROR_READ;
		// poll for new downward packet
		if (in.readReady())
		{
			int numc,numd;
			ID4 id;

			id = atom.read(numc,numd);

			mem.rewind();
			pack.len = patom.writeAtoms(id, in, numc, numd);
			pack.type = ChanPacket::T_PCP;

			//inData.writePacket(pack);
		//}
		error = PCP_ERROR_GENERAL;

		// process downward packets
		//if (inData.numPending())
		//{
			//inData.readPacket(pack);

			mem.rewind();

			//int numc,numd;
			id = patom.read(numc,numd);

			error = PCPStream::procAtom(patom,id,numc,numd,bcs);
			
			if (error){
				throw StreamException("PCP exception");
			}
		}

		error = 0;

	}catch(StreamException &e)
	{
		LOG_ERROR("PCP readPacket: %s (%d)",e.msg,error);
	}

	return error;
}

// ------------------------------------------
void PCPStream::readEnd(Stream &,Channel *)
{
}


// ------------------------------------------
void PCPStream::readPushAtoms(AtomStream &atom, int numc,BroadcastState &bcs)
{
	Host host;
	GnuID	chanID;

	chanID.clear();

	for(int i=0; i<numc; i++)
	{
		int c,d;
		ID4 id = atom.read(c,d);

		if (id == PCP_PUSH_IP)
			host.ip = atom.readInt();
		else if (id == PCP_PUSH_PORT)
			host.port = atom.readShort();
		else if (id == PCP_PUSH_CHANID)
			atom.readBytes(chanID.id,16);
		else
		{
			LOG_DEBUG("PCP skip: %s,%d,%d",id.getString().str(),c,d);
			atom.skip(c,d);
		}
	}


	if (bcs.forMe)
	{
		char ipstr[64];
		host.toStr(ipstr);

		Servent *s = NULL;

		if (chanID.isSet())
		{
			Channel *ch = chanMgr->findChannelByID(chanID);
			if (ch)
				if (ch->isBroadcasting() || !ch->isFull() && !servMgr->relaysFull() && ch->info.id.isSame(chanID))
					s = servMgr->allocServent();
		}else{
			s = servMgr->allocServent();
		}

		if (s)
		{
			LOG_DEBUG("GIVing to %s",ipstr);
			s->initGIV(host,chanID);
		}
	}

}
// ------------------------------------------
void PCPStream::readRootAtoms(AtomStream &atom, int numc,BroadcastState &bcs)
{
	String url;

	for(int i=0; i<numc; i++)
	{
		int c,d;
		ID4 id = atom.read(c,d);

		if (id == PCP_ROOT_UPDINT)
		{
			int si = atom.readInt();

			chanMgr->setUpdateInterval(si);
			LOG_DEBUG("PCP got new host update interval: %ds",si);
		}else if (id == PCP_ROOT_URL)
		{
			url = "http://www.peercast.org/";
			String loc;
			atom.readString(loc.data,sizeof(loc.data),d);
			url.append(loc);

		}else if (id == PCP_ROOT_CHECKVER)
		{
			unsigned int newVer = atom.readInt();
			if (newVer > PCP_CLIENT_VERSION)
			{
				strcpy(servMgr->downloadURL,url.cstr());
				peercastApp->notifyMessage(ServMgr::NT_UPGRADE,"There is a new version of Peercast available, please click here to upgrade your client.");
			}
			LOG_DEBUG("PCP got version check: %d / %d",newVer,PCP_CLIENT_VERSION);

		}else if (id == PCP_ROOT_NEXT)
		{
			unsigned int time = atom.readInt();

			if (time)
			{
				unsigned int ctime = sys->getTime();
				nextRootPacket = ctime+time;
				LOG_DEBUG("PCP expecting next root packet in %ds",time);
			}else
			{
				nextRootPacket = 0;
			}

		}else if (id == PCP_ROOT_UPDATE)
		{
			atom.skip(c,d);

			chanMgr->broadcastTrackerUpdate(remoteID,true);

		}else if ((id == PCP_MESG_ASCII) || (id == PCP_MESG))			// PCP_MESG_ASCII to be depreciated 
		{
			String newMsg;

			atom.readString(newMsg.data,sizeof(newMsg.data),d);
			if (!newMsg.isSame(servMgr->rootMsg.cstr()))
			{
				servMgr->rootMsg = newMsg;
				LOG_DEBUG("PCP got new root mesg: %s",servMgr->rootMsg.cstr());
				peercastApp->notifyMessage(ServMgr::NT_PEERCAST,servMgr->rootMsg.cstr());
			}
		}else
		{
			LOG_DEBUG("PCP skip: %s,%d,%d",id.getString().str(),c,d);
			atom.skip(c,d);
		}
	}
}

// ------------------------------------------
void PCPStream::readPktAtoms(Channel *ch,AtomStream &atom,int numc,BroadcastState &bcs)
{
	ChanPacket pack;
	ID4 type;


	for(int i=0; i<numc; i++)
	{
		int c,d;
		ID4 id = atom.read(c,d);

		if (id == PCP_CHAN_PKT_TYPE)
		{
			type = atom.readID4();

			if (type == PCP_CHAN_PKT_HEAD)
				pack.type = ChanPacket::T_HEAD;
			else if (type == PCP_CHAN_PKT_DATA)
				pack.type = ChanPacket::T_DATA;
			else
				pack.type = ChanPacket::T_UNKNOWN;

		}else if (id == PCP_CHAN_PKT_POS)
		{
			pack.pos = atom.readInt();


		}else if (id == PCP_CHAN_PKT_DATA)
		{
			pack.len = d;
			atom.readBytes(pack.data,pack.len);
		}
		else
		{
			LOG_DEBUG("PCP skip: %s,%d,%d",id.getString().str(),c,d);
			atom.skip(c,d);
		}
	}

	if (ch)
	{

		int diff = pack.pos - ch->streamPos;
		if (diff)
		{
			LOG_DEBUG("PCP skipping %s%8d (%10d -> %10d) count=%2d",(diff>0)?"+":"",diff,ch->streamPos,pack.pos, ch->skipCount);
			if (ch->lastSkipTime + 120 < sys->getTime()){
				ch->skipCount = 0;
			}
			ch->lastSkipTime = sys->getTime();
			ch->skipCount++; //JP-EX
			pack.skip = true;
		}

		if (servMgr->autoBumpSkipCount) //JP-EX
		{
			if ((ch->skipCount > servMgr->autoBumpSkipCount) && !(servMgr->disableAutoBumpIfDirect && ch->sourceHost.tracker)) //JP-MOD
			{
				LOG_DEBUG("Auto bump");
				ch->bump = true;
			}
		}

		if (pack.type == ChanPacket::T_HEAD)
		{
			LOG_DEBUG("New head packet at %d",pack.pos);
			bool renewhead;
			if (servMgr->keepDownstreams)
				renewhead = (memcmp(ch->headPack.data, pack.data, pack.len) != 0);
			else
				renewhead = true;

			/*
			// check for stream restart
			if (pack.pos == 0)		
			{
				LOG_CHANNEL("PCP resetting stream");
				ch->streamIndex++;
				ch->rawData.init();
			}
			*/
			if (renewhead || ch->lastStopTime + 30 < sys->getTime()) {
				// check for stream restart
				if (pack.pos == 0)
				{
					LOG_CHANNEL("PCP resetting stream");
					ch->streamIndex++;
					ch->rawData.init();
				}

				ch->headPack = pack;

				ch->rawData.writePacket(pack,true);
				ch->streamPos = pack.pos+pack.len;
			}

		}else if (pack.type == ChanPacket::T_DATA)
		{
			ch->rawData.writePacket(pack,true);
			ch->streamPos = pack.pos+pack.len;
		}

	}

	// update this parent packet stream position
	if ((pack.pos) && (!bcs.streamPos || (pack.pos < bcs.streamPos)))
		bcs.streamPos = pack.pos;

}
// -----------------------------------
void PCPStream::readHostAtoms(AtomStream &atom, int numc, BroadcastState &bcs, ChanHit &hit, bool flg)
{
//	ChanHit hit;
	hit.init();
	GnuID chanID = bcs.chanID;	//use default

	bool busy=false;

	unsigned int ipNum=0;

	for(int i=0; i<numc; i++)
	{
		int c,d;
		ID4 id = atom.read(c,d);

		if (id == PCP_HOST_IP)
		{
			unsigned int ip = atom.readInt();
			hit.rhost[ipNum].ip = ip;
		}else if (id == PCP_HOST_PORT)
		{
			int port = atom.readShort();
			hit.rhost[ipNum++].port = port;

			if (ipNum > 1)
				ipNum = 1;
		}
		else if (id == PCP_HOST_NUML)
		{
			hit.numListeners = atom.readInt();
			if (hit.numListeners > 10) hit.numListeners = 10;
		}
		else if (id == PCP_HOST_NUMR)
		{
			hit.numRelays = atom.readInt();
			if (hit.numRelays > 100) hit.numRelays = 100;
		}
		else if (id == PCP_HOST_UPTIME)
			hit.upTime = atom.readInt();
		else if (id == PCP_HOST_OLDPOS)
			hit.oldestPos = atom.readInt();
		else if (id == PCP_HOST_NEWPOS)
			hit.newestPos = atom.readInt();
		else if (id == PCP_HOST_VERSION)
			hit.version = atom.readInt();
		else if (id == PCP_HOST_VERSION_VP)
			hit.version_vp = atom.readInt();
		else if (id == PCP_HOST_VERSION_EX_PREFIX)
			atom.readBytes(hit.version_ex_prefix,2);
		else if (id == PCP_HOST_VERSION_EX_NUMBER){
			hit.version_ex_number = atom.readShort();
		}
		else if (id == PCP_HOST_FLAGS1)
		{
			int fl1 = atom.readChar();

			hit.recv = (fl1 & PCP_HOST_FLAGS1_RECV) !=0;
			hit.relay = (fl1 & PCP_HOST_FLAGS1_RELAY) !=0;
			hit.direct = (fl1 & PCP_HOST_FLAGS1_DIRECT) !=0;
			hit.cin = (fl1 & PCP_HOST_FLAGS1_CIN) !=0;
			hit.tracker = (fl1 & PCP_HOST_FLAGS1_TRACKER) !=0;
			hit.firewalled = (fl1 & PCP_HOST_FLAGS1_PUSH) !=0;


		}else if (id == PCP_HOST_ID)
			atom.readBytes(hit.sessionID.id,16);
		else if (id == PCP_HOST_CHANID)
			atom.readBytes(chanID.id,16);
		else if (id == PCP_HOST_UPHOST_IP)
			hit.uphost.ip = atom.readInt();
		else if (id == PCP_HOST_UPHOST_PORT)
			hit.uphost.port = atom.readInt();
		else if (id == PCP_HOST_UPHOST_HOPS)
			hit.uphostHops = atom.readInt();
		else if (id == PCP_HOST_CLAP_PP){ //JP-MOD
			hit.clap_pp = atom.readInt();
			if (hit.clap_pp & 1){
				Channel *c = chanMgr->findChannelByID(chanID);
				if(c && c->isBroadcasting()){
					String sjis;
					sjis = c->info.name;
#ifdef _WIN32
					sjis.convertTo(String::T_SJIS);
#endif
					peercastApp->notifyMessage(ServMgr::NT_APPLAUSE, sjis);
				}
			}
		}else
		{
			LOG_DEBUG("PCP skip: %s,%d,%d",id.getString().str(),c,d);
			atom.skip(c,d);
		}
	}

	hit.host = hit.rhost[0];
	hit.chanID = chanID;

	hit.numHops = bcs.numHops;

	hit.servent_id = bcs.servent_id;

	if (flg && (bcs.ttl != 0)){
//		LOG_DEBUG("readHostAtoms HITLISTLOCK ON-------------");
		chanMgr->hitlistlock.on();
		if (hit.recv)
			chanMgr->addHit(hit);
		else
			chanMgr->delHit(hit);
//		LOG_DEBUG("readHostAtoms HITLISTLOCK OFF-------------");
		chanMgr->hitlistlock.off();
	}

	if (hit.numHops == 1){
		Servent *sv = servMgr->findServentByServentID(hit.servent_id);
		if (sv && sv->getHost().ip == hit.host.ip){
//			LOG_DEBUG("set servent's waitPort = %d", hit.host.port);
 			sv->waitPort = hit.host.port;
//			hit.lastSendSeq = sv->serventHit.lastSendSeq;
			sv->serventHit = hit;
		}
	}
}

// ------------------------------------------
void PCPStream::readChanAtoms(AtomStream &atom,int numc,BroadcastState &bcs)
{
/*	Channel *ch=NULL;
	ChanHitList *chl=NULL;
	ChanInfo newInfo;

	ch = chanMgr->findChannelByID(bcs.chanID);
	chl = chanMgr->findHitListByID(bcs.chanID);

	if (ch)
		newInfo = ch->info;
	else if (chl)
		newInfo = chl->info;*/

	Channel *ch=NULL;
	ChanHitList *chl=NULL;
	ChanInfo newInfo, chaInfo;

	ch = this->parent;
	if (ch){
		newInfo = ch->info;
		chaInfo = ch->info;
	}

	for(int i=0; i<numc; i++)
	{

		int c,d;
		ID4 id = atom.read(c,d);

		if ((id == PCP_CHAN_PKT) && (ch))
		{
			readPktAtoms(ch,atom,c,bcs);
		}else if (id == PCP_CHAN_INFO)
		{
			newInfo.readInfoAtoms(atom,c);

		}else if (id == PCP_CHAN_TRACK)
		{
			newInfo.readTrackAtoms(atom,c);

		}else if (id == PCP_CHAN_BCID)
		{
			atom.readBytes(newInfo.bcID.id,16);

		}else if (id == PCP_CHAN_KEY)			// depreciated
		{
			atom.readBytes(newInfo.bcID.id,16);
			newInfo.bcID.id[0] = 0;				// clear flags

		}else if (id == PCP_CHAN_ID)
		{
			atom.readBytes(newInfo.id.id,16);

			ch = chanMgr->findChannelByID(newInfo.id);
			chl = chanMgr->findHitListByID(newInfo.id);

		}else
		{
			LOG_DEBUG("PCP skip: %s,%d,%d",id.getString().str(),c,d);
			atom.skip(c,d);
		}
	}

	chl = chanMgr->findHitList(newInfo);

	if (!chl)
		chl = chanMgr->addHitList(newInfo);

	if (chl)
	{
		chl->info.update(newInfo);
	
		if (!servMgr->chanLog.isEmpty())
		{
			//if (chl->numListeners())
			{
				try
				{
					FileStream file;
					file.openWriteAppend(servMgr->chanLog.cstr());

        				XML::Node *rn = new XML::Node("update time=\"%d\"",sys->getTime());
       					XML::Node *n = chl->info.createChannelXML();
        				n->add(chl->createXML(false));
        				n->add(chl->info.createTrackXML());
					rn->add(n);	
	
					rn->write(file,0);
					delete rn;
					file.close();
				}catch(StreamException &e)
				{
					LOG_ERROR("Unable to update channel log: %s",e.msg);
				}
			}
		}

	}

	if (ch && !ch->isBroadcasting())
		ch->updateInfo(newInfo);


}
// ------------------------------------------
int PCPStream::readBroadcastAtoms(AtomStream &atom,int numc,BroadcastState &bcs)
{
	ChanPacket pack;
//	int ttl=0;		
	int ver=0;
	int ver_vp=0;
	GnuID fromID,destID;
	int r=0;
	char ver_ex_prefix[2];
	int ver_ex_number = 0;

	fromID.clear();
	destID.clear();

	bcs.initPacketSettings();

	MemoryStream pmem(pack.data,sizeof(pack.data));
	AtomStream patom(pmem);

	patom.writeParent(PCP_BCST,numc);

	for(int i=0; i<numc; i++)
	{
		int c,d;
		ID4 id = atom.read(c,d);
		
		if (id == PCP_BCST_TTL)
		{
			bcs.ttl = atom.readChar()-1;
			patom.writeChar(id,bcs.ttl);

		}else if (id == PCP_BCST_HOPS)
		{
			bcs.numHops = atom.readChar()+1;
			patom.writeChar(id,bcs.numHops);

		}else if (id == PCP_BCST_FROM)
		{
			atom.readBytes(fromID.id,16);
			patom.writeBytes(id,fromID.id,16);

			routeList.add(fromID);
		}else if (id == PCP_BCST_GROUP)
		{
			bcs.group = atom.readChar();
			patom.writeChar(id,bcs.group);
		}else if (id == PCP_BCST_DEST)
		{
			atom.readBytes(destID.id,16);
			patom.writeBytes(id,destID.id,16);
			bcs.forMe = destID.isSame(servMgr->sessionID);

			char idstr1[64];
			char idstr2[64];

			destID.toStr(idstr1);
			servMgr->sessionID.toStr(idstr2);

		}else if (id == PCP_BCST_CHANID)
		{
			atom.readBytes(bcs.chanID.id,16);
			patom.writeBytes(id,bcs.chanID.id,16);
		}else if (id == PCP_BCST_VERSION)
		{
			ver = atom.readInt();
			patom.writeInt(id,ver);
		}else if (id == PCP_BCST_VERSION_VP)
		{
			ver_vp = atom.readInt();
			patom.writeInt(id,ver_vp);
		}else if (id == PCP_BCST_VERSION_EX_PREFIX)
		{
			atom.readBytes(ver_ex_prefix,2);
			patom.writeBytes(id,ver_ex_prefix,2);
		}else if (id == PCP_BCST_VERSION_EX_NUMBER)
		{
			ver_ex_number = atom.readShort();
			patom.writeShort(id,ver_ex_number);
		}else if (id == PCP_HOST)
		{
			ChanHit hit;
			readHostAtoms(atom,c,bcs,hit,false);
			Servent *sv = servMgr->findServentByServentID(bcs.servent_id);

			if (hit.uphost.ip == 0){
//				LOG_DEBUG("bcs servent_id = %d", bcs.servent_id);
				if (bcs.numHops == 1){
					hit.uphost.ip = servMgr->serverHost.ip;
					hit.uphost.port = servMgr->serverHost.port;
					hit.uphostHops = 1;
				} else {
//					Servent *sv = servMgr->findServentByServentID(bcs.servent_id);
					if (sv){
						hit.uphost.ip = sv->getHost().ip;
						hit.uphost.port = sv->waitPort;
						hit.uphostHops = bcs.numHops - 1;
					}
				}
			}
			if (sv &&
				((hit.numHops == 1 && (hit.rhost[0].ip == sv->getHost().ip
				 && hit.uphost.ip == servMgr->serverHost.ip && hit.uphost.port == servMgr->serverHost.port)
				 || (hit.rhost[1].localIP() && hit.rhost[1].ip == sv->getHost().ip))
				|| (hit.numHops != 1 && chanMgr->findParentHit(hit))))
			{
				int oldPos = pmem.pos;
				hit.writeAtoms(patom, hit.chanID);
				pmem.pos = oldPos;
				r = readAtom(patom,bcs);
 			} else {
				char tmp[80], tmp2[80], tmp3[80];
				hit.uphost.toStr(tmp);
				hit.host.toStr(tmp2);
				sv->getHost().toStr(tmp3);
				LOG_DEBUG("### Invalid bcst: hops=%d, l/r = %d/%d, ver=%d(VP%04d), ttl=%d",
					bcs.numHops,hit.numListeners, hit.numRelays, ver,ver_vp,bcs.ttl);
				LOG_DEBUG("### %s <- %s <- sv(%s)",
					tmp2, tmp, tmp3);
				bcs.ttl = 0;
			}
		} else {
			// copy and process atoms
			int oldPos = pmem.pos;
			patom.writeAtoms(id,atom.io,c,d);
			pmem.pos = oldPos;
			r = readAtom(patom,bcs);
		}
	}

	char fromStr[64];
	fromStr[0] = 0;
	if (fromID.isSet())
		fromID.toStr(fromStr);
	char destStr[64];
	destStr[0] = 0;
	if (destID.isSet())
		destID.toStr(destStr);
	char tmp[64];
	bcs.chanID.toStr(tmp);

//	LOG_DEBUG(tmp);

	if (ver_ex_number){
		LOG_DEBUG("PCP bcst: group=%d, hops=%d, ver=%d(%c%c%04d), from=%s, dest=%s ttl=%d",
			bcs.group,bcs.numHops,ver,ver_ex_prefix[0],ver_ex_prefix[1],ver_ex_number,fromStr,destStr,bcs.ttl);
	} else if (ver_vp){
		LOG_DEBUG("PCP bcst: group=%d, hops=%d, ver=%d(VP%04d), from=%s, dest=%s ttl=%d",bcs.group,bcs.numHops,ver,ver_vp,fromStr,destStr,bcs.ttl);
	} else {
		LOG_DEBUG("PCP bcst: group=%d, hops=%d, ver=%d, from=%s, dest=%s ttl=%d",bcs.group,bcs.numHops,ver,fromStr,destStr,bcs.ttl);
	}

	if (fromID.isSet())
		if (fromID.isSame(servMgr->sessionID))
		{
			LOG_ERROR("BCST loopback"); 
			return PCP_ERROR_BCST+PCP_ERROR_LOOPBACK;
		}

	// broadcast back out if ttl > 0 
	if ((bcs.ttl>0) && (!bcs.forMe))
	{
		pack.len = pmem.pos;
		pack.type = ChanPacket::T_PCP;

		if (bcs.group & (/*PCP_BCST_GROUP_ROOT|*/PCP_BCST_GROUP_TRACKERS|PCP_BCST_GROUP_RELAYS))
		{
			pack.priority = 11 - bcs.numHops;
			chanMgr->broadcastPacketUp(pack,bcs.chanID,remoteID,destID);
		}

		if (bcs.group & (/*PCP_BCST_GROUP_ROOT|*/PCP_BCST_GROUP_TRACKERS|PCP_BCST_GROUP_RELAYS))
		{
			servMgr->broadcastPacket(pack,bcs.chanID,remoteID,destID,Servent::T_COUT);
		}

		if (bcs.group & (PCP_BCST_GROUP_RELAYS|PCP_BCST_GROUP_TRACKERS))
		{
			servMgr->broadcastPacket(pack,bcs.chanID,remoteID,destID,Servent::T_CIN);
		}

		if (bcs.group & (PCP_BCST_GROUP_RELAYS))
		{
			servMgr->broadcastPacket(pack,bcs.chanID,remoteID,destID,Servent::T_RELAY);
		}


//		LOG_DEBUG("ttl=%d",ttl);

	} else {
//		LOG_DEBUG("ttl=%d",ttl);
	}
	return r;
}


// ------------------------------------------
int PCPStream::procAtom(AtomStream &atom,ID4 id,int numc, int dlen,BroadcastState &bcs)
{
	int r=0;
	ChanHit hit;
	int rBan = 0;

	if (id == PCP_CHAN)
	{
		readChanAtoms(atom,numc,bcs);
	}else if (id == PCP_ROOT)
	{
		if (servMgr->isRoot)
			throw StreamException("Unauthorized root message");				
		else
			readRootAtoms(atom,numc,bcs);

	}else if (id == PCP_HOST)
	{
		readHostAtoms(atom,numc,bcs,hit);
		Channel *ch = chanMgr->findChannelByID(hit.chanID);
		if (ch && (ch->isBroadcasting() || servMgr->vpDebug)){
			if (servMgr->autoPort0Kick && (hit.numHops == 1) && (hit.firewalled || (!hit.relay && !hit.numRelays))){
				char tmp[32];
				hit.host.IPtoStr(tmp);
				LOG_DEBUG("host that can't relay is disconnect: %s", tmp);
				rBan = PCP_ERROR_BANNED;
			}
			if (servMgr->allowOnlyVP && (hit.numHops == 1) && !hit.version_vp){
				char tmp[32];
				hit.host.IPtoStr(tmp);
				LOG_DEBUG("host that is not VP is disconnect: %s", tmp);
				rBan = PCP_ERROR_BANNED;
			}
		}

	}else if ((id == PCP_MESG_ASCII) || (id == PCP_MESG))		// PCP_MESG_ASCII to be depreciated
	{
		String msg;
		atom.readString(msg.data,sizeof(msg.data),dlen);
		LOG_DEBUG("PCP got text: %s",msg.cstr());
	}else if (id == PCP_BCST)
	{
		r = readBroadcastAtoms(atom,numc,bcs);
	}else if (id == PCP_HELO)
	{
		atom.skip(numc,dlen);
		atom.writeParent(PCP_OLEH,1);
			atom.writeBytes(PCP_HELO_SESSIONID,servMgr->sessionID.id,16);
	}else if (id == PCP_PUSH)
	{

		readPushAtoms(atom,numc,bcs);
	}else if (id == PCP_OK)
	{
		atom.readInt();

	}else if (id == PCP_QUIT)
	{
		r = atom.readInt();
		if (!r)
			r = PCP_ERROR_QUIT;

	}else if (id == PCP_ATOM)
	{
		for(int i=0; i<numc; i++)
		{
			int nc,nd;
			ID4 aid = atom.read(nc,nd);
			int ar = procAtom(atom,aid,nc,nd,bcs);
			if (ar)
				r = ar;
		}

	}else
	{
		LOG_CHANNEL("PCP skip: %s",id.getString().str());
		atom.skip(numc,dlen);
	}

	if (!r)
		r = rBan;

	return r;

}

// ------------------------------------------
int PCPStream::readAtom(AtomStream &atom,BroadcastState &bcs)
{
	int numc,dlen;
	ID4 id = atom.read(numc,dlen);

	return	procAtom(atom,id,numc,dlen,bcs);
}


