#ifndef _CHKMEMORYLEAK_H
#define _CHKMEMORYLEAK_H

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#ifdef _DEBUG
  #define _CRTDBG_MAP_ALLOC

  #define  SET_CRT_DEBUG_FIELD(a)   _CrtSetDbgFlag((a) | _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG))
  #define  CLEAR_CRT_DEBUG_FIELD(a) _CrtSetDbgFlag(~(a) & _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG))

  void*  operator new(size_t size, const char *filename, int linenumber);
  void   operator delete(void * _P, const char *filename, int linenumber);
#else
  #define  SET_CRT_DEBUG_FIELD(a)   ((void) 0)
  #define  CLEAR_CRT_DEBUG_FIELD(a) ((void) 0)
#endif

#include <malloc.h>
#include <crtdbg.h>

#endif
