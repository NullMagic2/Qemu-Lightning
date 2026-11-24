/*
Module: qemu_lightning.opengl.reactos_crt
Purpose: Declare bounded C ABI adapters for loader calls absent from NT5 MSVCRT.
Created: 2026-10-21
Architecture: Forced include for the unchanged Khronos C dependency; implementation is C++17.
*/
#pragma once
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif
int ql_nt5_strncpy_s(char* destination, size_t capacity, const char* source, size_t count);
int ql_nt5_strncat_s(char* destination, size_t capacity, const char* source, size_t count);
int ql_nt5_wcsncpy_s(wchar_t* destination, size_t capacity, const wchar_t* source, size_t count);
char* ql_nt5_strtok_s(char* input, const char* delimiters, char** context);
#ifdef __cplusplus
}
#endif
#ifdef QL_REDIRECT_NT5_CRT
#define strncpy_s ql_nt5_strncpy_s
#define strncat_s ql_nt5_strncat_s
#define wcsncpy_s ql_nt5_wcsncpy_s
#define strtok_s ql_nt5_strtok_s
#endif
