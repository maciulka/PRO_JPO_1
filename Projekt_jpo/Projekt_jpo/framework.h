// header.h: plik dołączany dla standardowych systemowych plików dołączanych,
// lub pliki dołączane specyficzne dla projektu
//

#pragma once

#include "targetver.h" // Wyklucz rzadko używane rzeczy z nagłówków systemu Windows
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
// Pliki nagłówkowe środowiska uruchomieniowego języka C
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>
#include "Resource.h"