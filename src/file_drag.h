#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <objidl.h>

bool FileDrag_Register(HWND window, HWND notify);
void FileDrag_Revoke(HWND window);
bool FileDrag_IsActive();
void FileDrag_Start(HWND owner, const std::vector<std::wstring>& paths);
HRESULT FileDrag_DataObject(const std::vector<std::wstring>& paths, IDataObject** data);
