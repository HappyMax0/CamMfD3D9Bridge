#include "pch.h"
#include "CamMfD3D9Bridge.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d9.h>
#include <vector>

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d9.lib")

static IMFSourceReader* g_reader = nullptr;
static IMFMediaSource* g_source = nullptr;
static IDirect3D9Ex* g_d3d9ex = nullptr;
static IDirect3DDevice9Ex* g_dev9ex = nullptr;
static IDirect3DSurface9* g_surface = nullptr;
static IDirect3DSurface9* g_sysMem = nullptr;  // ✅ 新增一個 sysmem surface
static int g_w = 0, g_h = 0;
static std::vector<IMFActivate*> g_vidDevices;
static bool g_devicesEnumerated = false;

static void SafeRelease(IUnknown* p)
{
    if (p) p->Release();
}

static void ClearVideoDevices()
{
    for (auto* a : g_vidDevices) SafeRelease(a);
    g_vidDevices.clear();
    g_devicesEnumerated = false;
}

static HRESULT InitD3D9Ex(int width, int height)
{
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d9ex);
    if (FAILED(hr)) return hr;

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = GetDesktopWindow();
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;

    hr = g_d3d9ex->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, pp.hDeviceWindow,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
        &pp, nullptr, &g_dev9ex);

    // ✅ 建立 CPU 可寫的 surface
    hr = g_dev9ex->CreateOffscreenPlainSurface(
        g_w,
        g_h,
        D3DFMT_A8R8G8B8,    // 必須跟 g_surface 一樣
        D3DPOOL_SYSTEMMEM,  // ✅ 關鍵
        &g_sysMem,
        nullptr);

    if (FAILED(hr)) return hr;

    /*    // 這裡用 offscreen plain surface：簡單、可 LockRect 寫入
        hr = g_dev9ex->CreateOffscreenPlainSurface(
            width, height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_surface, nullptr);*/

            // 取代 CreateOffscreenPlainSurface
    hr = g_dev9ex->CreateRenderTarget(
        width,
        height,
        D3DFMT_A8R8G8B8,          // 或 D3DFMT_X8R8G8B8
        D3DMULTISAMPLE_NONE,
        0,
        FALSE,                    // lockable 通常設 FALSE（預設）
        &g_surface,
        nullptr
    );

    return hr;
}

static HRESULT InitMF(int width, int height)
{
    // SourceReader 前置：CoInitializeEx + MFStartup 【3-a8dd5d】
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return hr;

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return hr;

    IMFAttributes* attrs = nullptr;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;

    hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) return hr;

    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) { SafeRelease(attrs); return hr; }

    hr = MFEnumDeviceSources(attrs, &devices, &count);
    SafeRelease(attrs);
    if (FAILED(hr)) return hr;
    if (count == 0) return E_FAIL;

    // 取第一個攝像頭（最小範例）
    hr = devices[0]->ActivateObject(IID_PPV_ARGS(&g_source));
    for (UINT32 i = 0; i < count; i++) devices[i]->Release();
    CoTaskMemFree(devices);
    if (FAILED(hr)) return hr;

    // 需要時可開 advanced video processing 讓 SourceReader 轉格式 【5-0e9a22】
    IMFAttributes* readerAttrs = nullptr;
    hr = MFCreateAttributes(&readerAttrs, 1);
    if (SUCCEEDED(hr))
    {
        readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, 1);
    }

    // 裝置用 MFCreateSourceReaderFromMediaSource 【3-a8dd5d】
    hr = MFCreateSourceReaderFromMediaSource(g_source, readerAttrs, &g_reader);
    SafeRelease(readerAttrs);
    if (FAILED(hr)) return hr;

    // 設定輸出格式：RGB32 + 指定尺寸（最小）
    IMFMediaType* mt = nullptr;
    hr = MFCreateMediaType(&mt);
    if (FAILED(hr)) return hr;

    hr = mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) { SafeRelease(mt); return hr; }

    hr = mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr)) { SafeRelease(mt); return hr; }

    hr = MFSetAttributeSize(mt, MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    if (FAILED(hr)) { SafeRelease(mt); return hr; }

    hr = g_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt);
    SafeRelease(mt);
    return hr;
}

HRESULT EnumVideoDevices(std::vector<IMFActivate*>& out)
{
    if (g_devicesEnumerated) return S_OK;

    IMFAttributes* attrs = nullptr;
    IMFActivate** devs = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) return hr;

    // 指定要列舉「視訊擷取裝置」【1-197311】
    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) { SafeRelease(attrs); return hr; }

    // 枚舉裝置，devs 由 MF 分配；需要釋放【1-197311】
    hr = MFEnumDeviceSources(attrs, &devs, &count);
    SafeRelease(attrs);
    if (FAILED(hr)) return hr;

    g_vidDevices.reserve(count);
    for (UINT32 i = 0; i < count; ++i)
    {
        // 保存指標，稍後 ClearVideoDevices() 會 Release
        g_vidDevices.push_back(devs[i]);
    }

    CoTaskMemFree(devs); // 釋放陣列記憶體【1-197311】
    g_devicesEnumerated = true;
    return S_OK;
}

HRESULT CreateReaderFromIndex(UINT32 index, IMFSourceReader** outReader)
{
    std::vector<IMFActivate*> devices;
    HRESULT hr = EnumVideoDevices(devices);
    if (FAILED(hr)) return hr;
    if (index >= devices.size()) return E_INVALIDARG;

    IMFMediaSource* source = nullptr;
    hr = devices[index]->ActivateObject(IID_PPV_ARGS(&source)); // 【1-198f16】
    if (FAILED(hr)) return hr;

    hr = MFCreateSourceReaderFromMediaSource(source, nullptr, outReader);
    source->Release();

    // 釋放 IMFActivate
    for (auto* a : devices) a->Release();
    return hr;
}

static HRESULT EnsureVideoDevices()
{
    if (g_devicesEnumerated) return S_OK;

    IMFAttributes* attrs = nullptr;
    IMFActivate** devs = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) return hr;

    // 指定要列舉「視訊擷取裝置」【1-197311】
    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) { SafeRelease(attrs); return hr; }

    // 枚舉裝置，devs 由 MF 分配；需要釋放【1-197311】
    hr = MFEnumDeviceSources(attrs, &devs, &count);
    SafeRelease(attrs);
    if (FAILED(hr)) return hr;

    g_vidDevices.reserve(count);
    for (UINT32 i = 0; i < count; ++i)
    {
        // 保存指標，稍後 ClearVideoDevices() 會 Release
        g_vidDevices.push_back(devs[i]);
    }

    CoTaskMemFree(devs); // 釋放陣列記憶體【1-197311】
    g_devicesEnumerated = true;
    return S_OK;
}

int __stdcall GetDeviceCount()
{
    HRESULT hr = EnsureVideoDevices();
    if (FAILED(hr)) return (int)hr;
    return (int)g_vidDevices.size();
}

int  __stdcall GetDeviceName(int index, wchar_t* buf, int cch)
{
    if (!buf || cch <= 0) return (int)E_INVALIDARG;

    // 預設輸出空字串，避免上層忘記清
    buf[0] = L'\0';

    HRESULT hr = EnsureVideoDevices();
    if (FAILED(hr)) return (int)hr;

    if (index < 0 || index >= (int)g_vidDevices.size())
        return (int)E_INVALIDARG;

    WCHAR* name = nullptr;
    UINT32 nameLen = 0;

    // FriendlyName：設備顯示名稱【1-197311】
    hr = g_vidDevices[index]->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen);

    if (FAILED(hr))
        return (int)hr;

    // nameLen 不含結尾 '\0'（文件說明）【2-4d6513】
    // 我們要複製到使用者 buffer，包含結尾 '\0'
    const UINT32 need = nameLen + 1;
    if ((UINT32)cch < need)
    {
        CoTaskMemFree(name); // GetAllocatedString 配置的記憶體要釋放【2-4d6513】
        return (int)HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    // 安全拷貝
    wmemcpy(buf, name, nameLen);
    buf[nameLen] = L'\0';

    CoTaskMemFree(name); // 【2-4d6513】
    return 0;
}

int __stdcall SelectDevice(int index)
{
    return CreateReaderFromIndex(index, &g_reader);
}

void __stdcall RefreshDevices()
{
    ClearVideoDevices();
    // 下次呼叫 GetDeviceCount/GetDeviceName 時會重新枚舉
}

int __stdcall Init(int width, int height)
{
    g_w = width; g_h = height;

    HRESULT hr = InitD3D9Ex(width, height);
    if (FAILED(hr)) return (int)hr;

    hr = InitMF(width, height);
    if (FAILED(hr)) return (int)hr;

    return 0;
}

void* __stdcall GetSurface()
{
    return (void*)g_surface;
}

int __stdcall GrabFrame()
{
    // ✅ 同時檢查兩個 surface
    if (!g_reader || !g_surface || !g_sysMem)
        return (int)E_FAIL;

    DWORD streamIndex = 0, flags = 0;
    LONGLONG ts = 0;
    IMFSample* sample = nullptr;

    HRESULT hr = g_reader->ReadSample(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        &streamIndex, &flags, &ts, &sample);

    if (FAILED(hr)) return (int)hr;
    if (!sample) return 0;

    IMFMediaBuffer* buffer = nullptr;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    SafeRelease(sample);
    if (FAILED(hr)) return (int)hr;

    BYTE* src = nullptr;
    DWORD maxLen = 0, curLen = 0;
    hr = buffer->Lock(&src, &maxLen, &curLen);
    if (FAILED(hr)) {
        SafeRelease(buffer);
        return (int)hr;
    }

    // ========= ✅ 修正核心開始 =========

    D3DLOCKED_RECT lr{};
    hr = g_sysMem->LockRect(&lr, nullptr, 0);   // ✅ 改成 g_sysMem

    if (SUCCEEDED(hr))
    {
        const int srcStride = g_w * 4;  // RGB32

        BYTE* dst = (BYTE*)lr.pBits;

        for (int y = 0; y < g_h; y++)
        {
            memcpy(dst + y * lr.Pitch,
                src + y * srcStride,
                srcStride);
        }

        g_sysMem->UnlockRect();   // ✅ 對 sysMem unlock

        // ✅ 把 systemmem 推到 render target
        hr = g_dev9ex->UpdateSurface(
            g_sysMem, nullptr,
            g_surface, nullptr);

        if (FAILED(hr))
        {
            buffer->Unlock();
            SafeRelease(buffer);
            return (int)hr;
        }
    }

    // ========= ✅ 修正核心結束 =========

    buffer->Unlock();
    SafeRelease(buffer);

    return (int)hr;
}

int __stdcall GetFrameInfo(int* w, int* h, int* stride)
{
    if (!w || !h || !stride) return (int)E_INVALIDARG;
    *w = g_w;
    *h = g_h;
    *stride = g_w * 4; // BGRA32
    return (int)S_OK;
}

// 將 g_sysMem 的內容拷到呼叫端提供的 buffer
int __stdcall CopyLastFrameBGRA(unsigned char* dst, int dstStride)
{
    if (!g_sysMem || !dst) return (int)E_FAIL;
    if (dstStride < g_w * 4) return (int)E_INVALIDARG;

    D3DLOCKED_RECT lr{};
    HRESULT hr = g_sysMem->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) return (int)hr;

    const int rowBytes = g_w * 4;
    for (int y = 0; y < g_h; ++y)
    {
        memcpy(dst + y * dstStride,
            (unsigned char*)lr.pBits + y * lr.Pitch,
            rowBytes);
    }

    g_sysMem->UnlockRect();
    return (int)S_OK;
}

void __stdcall Shutdown()
{
    SafeRelease(g_reader); g_reader = nullptr;
    SafeRelease(g_source); g_source = nullptr;

    SafeRelease(g_surface); g_surface = nullptr;
    SafeRelease(g_dev9ex);  g_dev9ex = nullptr;
    SafeRelease(g_d3d9ex);  g_d3d9ex = nullptr;

    MFShutdown();
    CoUninitialize();
}