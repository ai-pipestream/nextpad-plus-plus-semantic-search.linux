/*
 * Minimal Scintilla constants and the SCNotification layout used by the
 * plugin. Values and the notification struct match Scintilla's C API as
 * shipped in nextpad-plus-plus-linux src/sci_messages.h at commit
 * 8dec39c9f0f53aefc662520717fb7c0b6107fcdb (Sci_Position is ptrdiff_t).
 *
 * Refresh with tools/fetch-npp-headers.sh after the host updates Scintilla.
 */
#ifndef SEMANTIC_SCINTILLA_MIN_H
#define SEMANTIC_SCINTILLA_MIN_H

#include <stddef.h>
#include <stdint.h>

typedef uintptr_t uptr_t;
typedef intptr_t sptr_t;
typedef ptrdiff_t Sci_Position;

#define SCI_GETLENGTH 2006
#define SCI_MARKERLINEFROMHANDLE 2017
#define SCI_MARKERDELETEHANDLE 2018
#define SCI_MARKERADD 2043
#define SCI_MARKERGET 2046
#define SCI_INDICSETSTYLE 2080
#define SCI_GETCODEPAGE 2137
#define SCI_LINEFROMPOSITION 2166
#define SCI_GETDOCPOINTER 2357
#define SCI_SETINDICATORCURRENT 2500
#define SCI_SETINDICATORVALUE 2502
#define SCI_INDICATORFILLRANGE 2504
#define SCI_INDICATORCLEARRANGE 2505
#define SCI_GETCHARACTERPOINTER 2520
#define SCI_INDICSETFLAGS 2684

#define SC_CP_UTF8 65001
#define INDIC_TEXTFORE 17
#define INDIC_CONTAINER 8
#define SC_INDICVALUEBIT 0x1000000
#define SC_INDICFLAG_VALUEFORE 1

#define SC_MOD_INSERTTEXT 0x1
#define SC_MOD_DELETETEXT 0x2
#define SCN_UPDATEUI 2007
#define SCN_MODIFIED 2008

struct Sci_NotifyHeader {
    void *hwndFrom;
    uptr_t idFrom;
    unsigned int code;
};

struct SCNotification {
    Sci_NotifyHeader nmhdr;
    Sci_Position position;
    int ch;
    int modifiers;
    int modificationType;
    const char *text;
    Sci_Position length;
    Sci_Position linesAdded;
    int message;
    uptr_t wParam;
    sptr_t lParam;
    Sci_Position line;
    int foldLevelNow;
    int foldLevelPrev;
    int margin;
    int listType;
    int x;
    int y;
    int token;
    Sci_Position annotationLinesAdded;
    int updated;
    int listCompletionMethod;
    int characterSource;
};

#endif
