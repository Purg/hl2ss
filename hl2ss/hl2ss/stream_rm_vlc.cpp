
#include <mfapi.h>
#include "research_mode.h"
#include "server.h"
#include "locator.h"
#include "utilities.h"
#include "timestamps.h"
#include "types.h"

#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Perception.h>
#include <winrt/Windows.Perception.Spatial.h>

using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::Perception;
using namespace winrt::Windows::Perception::Spatial;

//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

// OK
template<bool ENABLE_LOCATION>
void RM_VLC_SendSampleToSocket(IMFSample* pSample, void* param)
{
    IMFMediaBuffer* pBuffer; // Release
    LONGLONG sampletime;
    BYTE* pBytes;
    DWORD cbData;
    WSABUF wsaBuf[ENABLE_LOCATION ? 4 : 3];
    float4x4 pose;
    HookCallbackSocket* user;
    bool ok;

    user = (HookCallbackSocket*)param;

    pSample->GetSampleTime(&sampletime);
    pSample->ConvertToContiguousBuffer(&pBuffer);

    pBuffer->Lock(&pBytes, NULL, &cbData);

    wsaBuf[0].buf = (char*)&sampletime;
    wsaBuf[0].len = sizeof(sampletime);
    
    wsaBuf[1].buf = (char*)&cbData;
    wsaBuf[1].len = sizeof(cbData);
    
    wsaBuf[2].buf = (char*)pBytes;
    wsaBuf[2].len = cbData;

    if constexpr(ENABLE_LOCATION)
    {
    pSample->GetBlob(MF_USER_DATA_PAYLOAD, (UINT8*)&pose, sizeof(pose), NULL);
    
    wsaBuf[3].buf = (char*)&pose;
    wsaBuf[3].len = sizeof(pose);
    }

    ok = send_multiple(user->clientsocket, wsaBuf, sizeof(wsaBuf) / sizeof(WSABUF));
    if (!ok) { SetEvent(user->clientevent); }

    pBuffer->Unlock();

    pBuffer->Release();
}

// OK
template <bool ENABLE_LOCATION>
void RM_VLC_Stream(IResearchModeSensor* sensor, SOCKET clientsocket, SpatialLocator const& locator)
{
    uint32_t const width      = RM_VLC_WIDTH;
    uint32_t const height     = RM_VLC_HEIGHT;
    uint32_t const framerate  = RM_VLC_FPS;
    uint32_t const lumasize   = width * height;
    uint32_t const chromasize = lumasize / 2;
    uint32_t const framebytes = lumasize + chromasize;
    LONGLONG const duration   = HNS_BASE / framerate;
    uint8_t  const zerochroma = 0x80;

    PerceptionTimestamp ts = nullptr;
    bool ok = true;
    IResearchModeSensorFrame* pSensorFrame; // Release
    IResearchModeSensorVLCFrame* pVLCFrame; // Release
    IMFSinkWriter* pSinkWriter; // Release
    IMFMediaBuffer* pBuffer; // Release
    IMFSample* pSample; // Release
    HANDLE clientevent; // CloseHandle
    H26xFormat format;    
    HookCallbackSocket user;
    ResearchModeSensorTimestamp timestamp;
    DWORD dwVideoIndex;
    BYTE const* pImage;
    size_t length;
    BYTE* pDst;
    float4x4 pose;
    HRESULT hr;

    ok = ReceiveVideoFormatH26x(clientsocket, format);
    if (!ok) { return; }

    format.width     = width;
    format.height    = height;
    format.framerate = framerate;

    clientevent = CreateEvent(NULL, TRUE, FALSE, NULL);

    user.clientsocket = clientsocket;
    user.clientevent  = clientevent;

    CreateSinkWriterNV12ToH26x(&pSinkWriter, &dwVideoIndex, format, RM_VLC_SendSampleToSocket<ENABLE_LOCATION>, &user);

    sensor->OpenStream();

    do
    {
    hr = sensor->GetNextBuffer(&pSensorFrame); // block
    if (FAILED(hr)) { continue; }

    pSensorFrame->GetTimeStamp(&timestamp);
    pSensorFrame->QueryInterface(IID_PPV_ARGS(&pVLCFrame));

    pVLCFrame->GetBuffer(&pImage, &length);

    MFCreateMemoryBuffer(framebytes, &pBuffer);

    pBuffer->Lock(&pDst, NULL, NULL);
    memcpy(pDst, pImage, lumasize);
    memset(pDst + lumasize, zerochroma, chromasize);
    pBuffer->Unlock();
    pBuffer->SetCurrentLength(framebytes);

    MFCreateSample(&pSample);

    pSample->AddBuffer(pBuffer);
    pSample->SetSampleDuration(duration);
    pSample->SetSampleTime(timestamp.HostTicks);

    if constexpr(ENABLE_LOCATION)
    {
    ts = QPCTimestampToPerceptionTimestamp(timestamp.HostTicks);
    pose = Locator_Locate(ts, locator, Locator_GetWorldCoordinateSystem(ts));
    pSample->SetBlob(MF_USER_DATA_PAYLOAD, (UINT8*)&pose, sizeof(float4x4));
    }

    pSinkWriter->WriteSample(dwVideoIndex, pSample);

    pVLCFrame->Release();
    pSensorFrame->Release();
    pSample->Release();
    pBuffer->Release();
    }
    while (WaitForSingleObject(clientevent, 0) == WAIT_TIMEOUT);

    sensor->CloseStream();

    pSinkWriter->Flush(dwVideoIndex);
    pSinkWriter->Release();

    CloseHandle(clientevent);
}

// OK
void RM_VLC_Stream_Mode0(IResearchModeSensor* sensor, SOCKET clientsocket)
{
    RM_VLC_Stream<false>(sensor, clientsocket, nullptr);
}

// OK
void RM_VLC_Stream_Mode1(IResearchModeSensor* sensor, SOCKET clientsocket, SpatialLocator const& locator)
{
    RM_VLC_Stream<true>(sensor, clientsocket, locator);
}

// OK
void RM_VLC_Stream_Mode2(IResearchModeSensor* sensor, SOCKET clientsocket)
{
    std::vector<float> uv2x;
    std::vector<float> uv2y;
    std::vector<float> mapx;
    std::vector<float> mapy;
    float K[4];
    DirectX::XMFLOAT4X4 extrinsics;
    WSABUF wsaBuf[6];

    ResearchMode_GetIntrinsics(sensor, uv2x, uv2y, mapx, mapy, K);
    ResearchMode_GetExtrinsics(sensor, extrinsics);

    wsaBuf[0].buf = (char*)uv2x.data();
    wsaBuf[0].len = (ULONG)(uv2x.size() * sizeof(float));
    
    wsaBuf[1].buf = (char*)uv2y.data();
    wsaBuf[1].len = (ULONG)(uv2y.size() * sizeof(float));
    
    wsaBuf[2].buf = (char*)extrinsics.m;
    wsaBuf[2].len = sizeof(extrinsics.m);

    wsaBuf[3].buf = (char*)mapx.data();
    wsaBuf[3].len = (ULONG)(mapx.size() * sizeof(float));

    wsaBuf[4].buf = (char*)mapy.data();
    wsaBuf[4].len = (ULONG)(mapy.size() * sizeof(float));

    wsaBuf[5].buf = (char*)K;
    wsaBuf[5].len = sizeof(K);

    send_multiple(clientsocket, wsaBuf, sizeof(wsaBuf) / sizeof(WSABUF));
}
