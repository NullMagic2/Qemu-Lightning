/*
Module: qemu_lightning.opengl.reactos_condition_variable_test_api
Purpose: Interpose only the adapter's event calls to force real wait races.
Created: 2026-10-22
Architecture: Force-include for the fixture adapter translation unit only;
production builds never include this file or link these functions.
*/
#pragma once
#include <windows.h>
extern "C" HANDLE WINAPI ql_cv_test_create_event(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
extern "C" BOOL WINAPI ql_cv_test_close_handle(HANDLE);
extern "C" BOOL WINAPI ql_cv_test_set_event(HANDLE);
extern "C" DWORD WINAPI ql_cv_test_wait(HANDLE, DWORD);
#define CreateEventW ql_cv_test_create_event
#define CloseHandle ql_cv_test_close_handle
#define SetEvent ql_cv_test_set_event
#define WaitForSingleObject ql_cv_test_wait
