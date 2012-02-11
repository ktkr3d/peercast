// --------------------------------------------------------------------------
// File : listitem.cpp
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

#include "listitem.h"

int selectedItem(Q3ListBox *lb)
{
	int ret;
	
	ret = lb->currentItem();
	if(!lb->isSelected(ret))
		ret = -1;
		
	return ret;
}

unsigned int get_relay_color(tServentInfo &info)
{
    if(info.available)
    {
		if(!info.firewalled)
		{
		    if(info.relay)
		    {
				return _RGB(0,255,0);
		    }
		    else
		    {
				if(info.relays)
				    return _RGB(0,160,255);
				else
				    return _RGB(255,0,255);
		    }
		}
		else
		{
			if(info.relays)
				return _RGB(255,128,0);
			else
				return _RGB(255,0,0);
		}
    }
    else
    {
		return _RGB(0,0,0);
    }
}

LogListBoxItem::LogListBoxItem(QString str, Q3ListBox *lb) : Q3ListBoxItem()
{
	out = str;
	line_width = lb->fontMetrics().width(out) + 2;
	line_height = lb->fontMetrics().height();
}
	
void LogListBoxItem::paint(QPainter *p)
{
	p->drawText(1, 1, line_width + 1, line_height, Qt::AlignVCenter, out);
}

ChannelListBoxItem::ChannelListBoxItem(Channel *ch, Q3ListBox *lb) : Q3ListBoxItem()
{
	info.available = true;
	info.relay = ch->chDisp.relay;
	info.relays = ch->localRelays();
	info.firewalled = servMgr->getFirewall() == ServMgr::FW_ON;
	
	receive = ch->status == Channel::S_RECEIVING || ch->status == Channel::S_BROADCASTING;
	broadcast = ch->status == Channel::S_BROADCASTING;
	tracker = ch->sourceHost.tracker;
	
	status.sprintf(" - %d kbps - %s - %d/%d - [%d/%d] - %s",
		ch->info.bitrate,
		ch->getStatusStr(),
		ch->totalListeners(),
		ch->totalRelays(),
		ch->localListeners(),
		ch->localRelays(),
		ch->stayConnected ? "YES" : "NO"
		);
	name = QString::fromUtf8(ch->info.name.data);	
			
	id = ch->info.id;
	
	line_height = lb->fontMetrics().height();
	offset = line_height + 2;
	line_width = lb->width() - lb->lineWidth()*2 - (lb->verticalScrollBar()->isVisible() ? lb->verticalScrollBar()->width() : 0) - 2;
	status_width = lb->fontMetrics().width(status) + 2;
}
		
void ChannelListBoxItem::paint(QPainter *p)
{
	QFontMetrics fm = p->fontMetrics();	
	
	if(receive)
	{
		if(broadcast)
		{
			if(isSelected())
				p->setPen(_RGB(255,255,0));
			else
				p->setPen(_RGB(128,128,0));
		}
		else if(tracker)
		{
			if(isSelected())
				p->setPen(_RGB(0,255,0));
			else
				p->setPen(_RGB(0,160,0));
		}
	}
	
	p->drawText(offset, 1, line_width - status_width - offset, line_height, Qt::AlignVCenter, name);
	p->drawText(line_width - status_width, 1, line_width, line_height, Qt::AlignVCenter, status);
	
	QBrush brush;
	
	if(receive)
	{
		brush.setStyle(Qt::SolidPattern);
		brush.setColor(get_relay_color(info));
	}
	else
	{
		brush.setColor(_RGB(255,255,255));
	}
	
	p->setBrush(brush);
	p->drawRect(1, 1, line_height-3, line_height-3);
}

ConnectionListBoxItem::ConnectionListBoxItem(Servent *sv, tServentInfo &si, Q3ListBox *lb) : Q3ListBoxItem()
{
	int t;
	char vp_ver[32], host_name[32];
	
	servent_id = sv->servent_id;
	info = si;
	
	if(info.vp_ver)
		sprintf(vp_ver, "(VP%04d)", info.vp_ver);
	else
		strcpy(vp_ver, "");
	
	sv->getHost().toStr(host_name);
	
	t = (sv->lastConnect) ? (sys->getTime()-sv->lastConnect) : 0;
	
	relaying = sv->type == Servent::T_RELAY;
	
	if(sv->type == Servent::T_RELAY)
	{
		if(sv->status == Servent::S_CONNECTED)
		{
			out.sprintf("RELAYING - %ds - %d/%d - %s - %s%s",
				t, 
				info.totalListeners,
				info.totalRelays,
				host_name, 
				sv->agent.cstr(),
				vp_ver
				);
		}
		else
		{
			out.sprintf("%s-%s - %ds - %d/%d - %s - %s%s",
				sv->getTypeStr(),
				sv->getStatusStr(),
				t, 
				info.totalListeners,
				info.totalRelays,
				host_name, 
				sv->agent.cstr(),
				vp_ver
				);
		}
	}
	else if(sv->type == Servent::T_DIRECT)
	{
		out.sprintf("%s-%s - %ds - %s - %s", 
			sv->getTypeStr(),
			sv->getStatusStr(),
			t, 
			host_name, 
			sv->agent.cstr() 
			); 
	}
	else
	{
		if(sv->status == Servent::S_CONNECTED)
		{
			out.sprintf("%s-%s - %ds - %s - %s", 
				sv->getTypeStr(),
				sv->getStatusStr(),
				t, 
				host_name, 
				sv->agent.cstr() 
				); 
		}
		else
		{
			out.sprintf("%s-%s - %ds - %s", 
				sv->getTypeStr(),
				sv->getStatusStr(),
				t, 
				host_name 
				); 
		}
	}
	
	line_height = lb->fontMetrics().height();
	offset = line_height + 2;	
	line_width = lb->fontMetrics().width(out) + offset + 2; 
}

void ConnectionListBoxItem::paint(QPainter *p)
{
	QFontMetrics fm = p->fontMetrics();	
	
	p->drawText(offset, 1, offset + line_width, line_height, Qt::AlignVCenter, out);
	
	QBrush brush;
	
	if(relaying)
	{
		brush.setStyle(Qt::SolidPattern);
		brush.setColor(get_relay_color(info));
	}
	else
	{
		brush.setColor(_RGB(255,255,255));
	}
	
	p->setBrush(brush);
	p->drawRect(1, 1, line_height-3, line_height-3);
}
