// --------------------------------------------------------------------------
// File : main.h
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

#ifndef MAINH
#define MAINH

#include <queue>

typedef struct
{
	int type;
	QString name;
	QString msg;
}tNotifyInfo;

extern bool g_bChangeSettings;
extern std::queue<QString> g_qLog;
extern std::queue<tNotifyInfo> g_qNotify;

#endif
