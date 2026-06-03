#pragma once
#include <cstdint>

extern "C" {
    __declspec(dllexport) int __stdcall GetDeviceCount();
    __declspec(dllexport) int __stdcall GetDeviceName(int index, wchar_t* name, int nameSize);
    __declspec(dllexport) int __stdcall SelectDevice(int index);
    __declspec(dllexport) void __stdcall RefreshDevices();
    __declspec(dllexport) int __stdcall Init(int width, int height);
    __declspec(dllexport) void* __stdcall GetSurface();   // IDirect3DSurface9*
    __declspec(dllexport) int __stdcall GrabFrame();
    __declspec(dllexport) int __stdcall GetFrameInfo(int* w, int* h, int* stride);
    __declspec(dllexport) int __stdcall CopyLastFrameBGRA(unsigned char* dst, int dstStride);
    __declspec(dllexport) void __stdcall Shutdown();
}