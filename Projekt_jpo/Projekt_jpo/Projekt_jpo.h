// Projekt_jpo.h
#pragma once
#include <windows.h>
#include "Resource.h"

#define MAX_LOADSTRING 100

// Dialogi
#ifndef IDD_DIALOG_STACJE
#define IDD_DIALOG_STACJE      201
#endif
#ifndef IDD_DIALOG_WYKRES
#define IDD_DIALOG_WYKRES      202
#endif
#ifndef IDD_DIALOG_USTAWIENIA
#define IDD_DIALOG_USTAWIENIA  203
#endif
#ifndef IDD_ABOUTBOX
#define IDD_ABOUTBOX           204
#endif

// Kontrolki
#ifndef IDC_LIST_STACJE
#define IDC_LIST_STACJE        1001
#endif
#ifndef IDC_DATETIME_FROM
#define IDC_DATETIME_FROM      1007
#endif
#ifndef IDC_DATETIME_TO
#define IDC_DATETIME_TO        1008
#endif
#ifndef IDC_BUTTON_DRAW
#define IDC_BUTTON_DRAW        1009
#endif
#ifndef IDC_COMBO_STATION
#define IDC_COMBO_STATION      1010
#endif
#ifndef IDC_COMBO_CITY
#define IDC_COMBO_CITY         1011
#endif
#ifndef IDC_COMBO_REGION
#define IDC_COMBO_REGION       1012
#endif
#ifndef IDC_RADIO_LINE
#define IDC_RADIO_LINE         1013
#endif
#ifndef IDC_RADIO_BAR
#define IDC_RADIO_BAR          1014
#endif
#ifndef IDC_MAIN_BTN_STACJE
#define IDC_MAIN_BTN_STACJE    2001
#endif
#ifndef IDC_MAIN_BTN_WYKRES
#define IDC_MAIN_BTN_WYKRES    2002
#endif
#ifndef IDC_MAIN_BTN_USTAW
#define IDC_MAIN_BTN_USTAW     2003
#endif
#ifndef IDC_STATIC_TIMESTAMP
#define IDC_STATIC_TIMESTAMP   2004
#endif

// Główne zmienne
extern HINSTANCE   hInst;
extern WCHAR       szTitle[MAX_LOADSTRING];

// Klasy i funkcje
ATOM                MyRegisterClass(HINSTANCE);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);

// Dialogi
INT_PTR CALLBACK    DlgStacjeProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    DlgWykresProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    DlgUstawienProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    AboutDlgProc(HWND, UINT, WPARAM, LPARAM);
