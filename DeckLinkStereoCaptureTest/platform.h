/* -LICENSE-START-
 ** Copyright (c) 2013 Blackmagic Design
 **  
 ** Permission is hereby granted, free of charge, to any person or organization 
 ** obtaining a copy of the software and accompanying documentation (the 
 ** "Software") to use, reproduce, display, distribute, sub-license, execute, 
 ** and transmit the Software, and to prepare derivative works of the Software, 
 ** and to permit third-parties to whom the Software is furnished to do so, in 
 ** accordance with:
 ** 
 ** (1) if the Software is obtained from Blackmagic Design, the End User License 
 ** Agreement for the Software Development Kit (“EULA”) available at 
 ** https://www.blackmagicdesign.com/EULA/DeckLinkSDK; or
 ** 
 ** (2) if the Software is obtained from any third party, such licensing terms 
 ** as notified by that third party,
 ** 
 ** and all subject to the following:
 ** 
 ** (3) the copyright notices in the Software and this entire statement, 
 ** including the above license grant, this restriction and the following 
 ** disclaimer, must be included in all copies of the Software, in whole or in 
 ** part, and all derivative works of the Software, unless such copies or 
 ** derivative works are solely in the form of machine-executable object code 
 ** generated by a source language processor.
 ** 
 ** (4) THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
 ** OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 ** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT 
 ** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE 
 ** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, 
 ** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 ** DEALINGS IN THE SOFTWARE.
 ** 
 ** A copy of the Software is available free of charge at 
 ** https://www.blackmagicdesign.com/desktopvideo_sdk under the EULA.
 ** 
 ** -LICENSE-END-
 */

// Windows definitions

#pragma once

#include "comdef.h"

#include <conio.h>
#include <objbase.h>		// Necessary for COM
#include <comutil.h>
#include "DeckLinkAPI_h.h"
#include <stdio.h>
#include <Windows.h>
#include <tchar.h>
#include <string>
#include <list>
#include <algorithm>

#define	INT8_UNSIGNED			unsigned char
#define	INT32_UNSIGNED			unsigned int
#define INT32_SIGNED			int
#define INT64_UNSIGNED			ULONGLONG
#define INT32_SIGNED			int
#define INT64_SIGNED			LONGLONG

#define STRINGOBJ               BSTR
#define STRINGPTR               TCHAR *
#define STRINGPREFIX(x)         _T(x)
#define STRINGLITERAL(x)		OLESTR(x)
#define STRINGCOPY(x)           SysAllocString(x)
#define STRINGFREE(x)           SysFreeString(x)

#define MUTEX					HANDLE //HANDLE

HRESULT		Initialize();
HRESULT     GetDeckLinkIterator(IDeckLinkIterator **deckLinkIterator);
HRESULT		GetDeckLinkVideoConversion(IDeckLinkVideoConversion** deckLinkVideoConversion);
HRESULT		GetDeckLinkDiscoveryInstance(IDeckLinkDiscovery **deckLinkDiscovery);

void MutexInit(MUTEX* mutex);
void MutexLock(MUTEX* mutex);
void MutexUnlock(MUTEX* mutex);
void MutexDestroy(MUTEX* mutex);

// string helpers
void        StringFromCharArray(STRINGOBJ* newStr, const char* charPtr);
void        StringToCharArray(STRINGOBJ bmdStr, char* charArray, unsigned int arrayLength);
void        StringToStdString(STRINGOBJ bmdStr, std::string& stdStr);

// atomic operators
INT32_SIGNED AtomicIncrement(volatile INT32_SIGNED* value);
INT32_SIGNED AtomicDecrement(volatile INT32_SIGNED* value);