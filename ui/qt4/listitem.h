// --------------------------------------------------------------------------
// File : listitem.h
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

#ifndef LISTITEMH
#define LISTITEMH

#include "peercast.h"

#include <qpainter.h>
#include <Qt/q3listbox.h>

#define _RGB(r,g,b) ((unsigned int)r<<16 | (unsigned int)g<<8 | (unsigned int)b)

typedef struct
{
    bool available;
    bool relay;
    bool firewalled;
    int relays;
    int vp_ver;
    int totalRelays, totalListeners;
}tServentInfo;

int selectedItem(Q3ListBox *lb);
unsigned int get_relay_color(tServentInfo &info);

class LogListBoxItem : public Q3ListBoxItem
{
public:
	QString out;
	int line_height, line_width;
		
	LogListBoxItem(QString str, Q3ListBox *lb);
protected:
	virtual void paint(QPainter *p);
	
	virtual int height(const Q3ListBox *lb) const
	{
		return line_height;
	}
	
	virtual int width(const Q3ListBox *lb) const
	{
		return line_width;
	}
};

class ChannelListBoxItem : public Q3ListBoxItem
{
public:
	int offset;
	QString name, status;
	GnuID id;
	tServentInfo info;
	int line_height, line_width, status_width;
	bool receive, tracker, broadcast;
		
	ChannelListBoxItem(Channel *ch, Q3ListBox *lb);
		
protected:
	virtual void paint(QPainter *p);
		
	virtual int height(const Q3ListBox *lb) const
	{
		return line_height;
	}
		
	virtual int width(const Q3ListBox *lb) const
	{
		return line_width;
	}
};

class ConnectionListBoxItem : public Q3ListBoxItem
{
public:
	int offset;
	QString out;
	int servent_id;
	tServentInfo info;
	int line_height, line_width;
	bool relaying;
		
	ConnectionListBoxItem(Servent *sv, tServentInfo &si, Q3ListBox *lb);
		
protected:
	virtual void paint(QPainter *p);
		
	virtual int height(const Q3ListBox *lb) const
	{
		return line_height;
	}
		
	virtual int width(const Q3ListBox *lb) const
	{
		return line_width;
	}
};

#endif
