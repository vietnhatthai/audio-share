/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifdef _WINDOWS

#include "audio_manager.hpp"
#include "client.pb.h"
#include "network_manager.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <functional>
#include <iostream>
#include <vector>

#include <atlbase.h>
#include <combaseapi.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Audioclient.h>
#include <Audiopolicy.h>

using namespace io::github::mkckr0::audio_share_app::pb;

void exit_on_failed(HRESULT hr, const char* message = "", const char* func = "");
void print_endpoints(CComPtr<IMMDeviceCollection> pColletion);
void set_format(std::shared_ptr<AudioFormat> _format, WAVEFORMATEX* format);

namespace detail {

audio_manager_impl::audio_manager_impl()
{
    (void)CoInitializeEx(nullptr, COINIT::COINIT_MULTITHREADED);
}

audio_manager_impl::~audio_manager_impl()
{
    CoUninitialize();
}

} // namespace detail

void audio_manager::do_loopback_recording(std::shared_ptr<network_manager> network_manager, const std::string& endpoint_id)
{
    spdlog::info("endpoint_id: {}", endpoint_id);

    HRESULT hr {};

    CComPtr<IMMDeviceEnumerator> pEnumerator;
    hr = pEnumerator.CoCreateInstance(__uuidof(MMDeviceEnumerator));
    exit_on_failed(hr, "Failed to create IMMDeviceEnumerator");

    CComPtr<IMMDevice> pEndpoint;
    hr = pEnumerator->GetDevice(mbs_to_wchars(endpoint_id).c_str(), &pEndpoint);
    exit_on_failed(hr, "Failed to get audio endpoint device");

    CComPtr<IPropertyStore> pProps;
    hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
    exit_on_failed(hr, "Failed to open property store");

    PROPVARIANT varName;
    PropVariantInit(&varName);
    hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
    exit_on_failed(hr, "Failed to get device friendly name");
    spdlog::info("Selected audio endpoint: {}", wchars_to_utf8(varName.pwszVal));
    PropVariantClear(&varName);

    CComPtr<IAudioClient> pAudioClient;
    hr = pEndpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
    exit_on_failed(hr, "Failed to activate IAudioClient");

    CComHeapPtr<WAVEFORMATEX> pCaptureFormat;
    hr = pAudioClient->GetMixFormat(&pCaptureFormat);
    exit_on_failed(hr, "Failed to get mix format");

    // Log detailed audio format information
    spdlog::info("WAVEFORMATEX: wFormatTag: {}, nBlockAlign: {}", pCaptureFormat->wFormatTag, pCaptureFormat->nBlockAlign);
    spdlog::info("AudioFormat: format_tag: {}, channels: {}, sample_rate: {}, bits_per_sample: {}",
                 pCaptureFormat->wFormatTag,
                 pCaptureFormat->nChannels,
                 pCaptureFormat->nSamplesPerSec,
                 pCaptureFormat->wBitsPerSample);

    // Handle WAVE_FORMAT_EXTENSIBLE
    if (pCaptureFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* pExtensibleFormat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pCaptureFormat.m_pData);
        if (pExtensibleFormat->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            pCaptureFormat->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        }
    }

    set_format(_format, pCaptureFormat);

    constexpr static int REFTIMES_PER_SEC = 10000000; // 1 reference_time = 100ns
    constexpr static int REFTIMES_PER_MILLISEC = 10000;

    REFERENCE_TIME hnsMinimumDevicePeriod = 0;
    hr = pAudioClient->GetDevicePeriod(nullptr, &hnsMinimumDevicePeriod);
    exit_on_failed(hr, "Failed to get device period");

    // Adjust buffer duration (try smaller)
    REFERENCE_TIME hnsRequestedDuration = 5 * REFTIMES_PER_SEC;  // Reduce requested duration to 5 seconds

    // Try initializing with loopback mode
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, hnsRequestedDuration, 0, pCaptureFormat, nullptr);
    if (FAILED(hr)) {
        spdlog::warn("Loopback initialization failed, trying with EVENTCALLBACK flag");

        // Try different flags if loopback fails
        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsRequestedDuration, 0, pCaptureFormat, nullptr);
        exit_on_failed(hr, "Failed to initialize audio client for loopback");
    }

    UINT32 bufferFrameCount = 0;
    hr = pAudioClient->GetBufferSize(&bufferFrameCount);
    exit_on_failed(hr, "Failed to get buffer size");
    spdlog::info("Buffer size: {}", bufferFrameCount);

    CComPtr<IAudioCaptureClient> pCaptureClient;
    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
    exit_on_failed(hr, "Failed to get IAudioCaptureClient");

    hr = pAudioClient->Start();
    exit_on_failed(hr, "Failed to start audio client");

    const std::chrono::milliseconds duration { hnsMinimumDevicePeriod / REFTIMES_PER_MILLISEC };
    spdlog::info("Device period: {}ms", duration.count());

    UINT32 frame_count = 0;
    int seconds = 0;

    using namespace std::chrono_literals;
    asio::steady_timer timer(*network_manager->_ioc);
    std::error_code ec;

    timer.expires_at(std::chrono::steady_clock::now());

    do {
        timer.expires_at(timer.expiry() + duration);
        timer.wait(ec);
        if (ec) {
            break;
        }

        UINT32 next_packet_size = 0;
        hr = pCaptureClient->GetNextPacketSize(&next_packet_size);
        exit_on_failed(hr, "pCaptureClient->GetNextPacketSize");

        if (next_packet_size == 0) {
            continue;
        }

        BYTE* pData {};
        UINT32 numFramesAvailable {};
        DWORD dwFlags {};

        hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &dwFlags, NULL, NULL);
        exit_on_failed(hr, "pCaptureClient->GetBuffer");

        int bytes_per_frame = pCaptureFormat->nBlockAlign;
        int count = numFramesAvailable * bytes_per_frame;

        network_manager->broadcast_audio_data((const char*)pData, count, pCaptureFormat->nBlockAlign);

#ifdef DEBUG
        frame_count += numFramesAvailable;
        seconds = frame_count / pCaptureFormat->nSamplesPerSec;
        // spdlog::trace("numFramesAvailable: {}, seconds: {}", numFramesAvailable, seconds);
#endif // DEBUG

        hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
        exit_on_failed(hr, "pCaptureClient->ReleaseBuffer");

    } while (!_stoppped);

    // Stop the audio client after the loop ends
    pAudioClient->Stop();
}

int audio_manager::get_endpoint_list(endpoint_list_t& endpoint_list)
{
    HRESULT hr {};

    // Enumerate both render (playback) and capture (recording) devices
    EDataFlow data_flows[] = { eRender, eCapture };  // eRender for playback, eCapture for recording
    int default_index = -1;
    std::string default_id = get_default_endpoint();

    // Loop through both playback and recording devices
    for (EDataFlow data_flow : data_flows)
    {
        CComPtr<IMMDeviceEnumerator> pEnumerator;
        hr = pEnumerator.CoCreateInstance(__uuidof(MMDeviceEnumerator));
        exit_on_failed(hr);

        CComPtr<IMMDeviceCollection> pCollection;
        hr = pEnumerator->EnumAudioEndpoints(data_flow, DEVICE_STATE_ACTIVE, &pCollection);
        exit_on_failed(hr);

        UINT count {};
        hr = pCollection->GetCount(&count);
        exit_on_failed(hr);

        default_index = -1;
        default_id = get_default_endpoint();

        for (UINT i = 0; i < count; ++i)
        {
            CComPtr<IMMDevice> pEndpoint;
            hr = pCollection->Item(i, &pEndpoint);
            exit_on_failed(hr);

            CComHeapPtr<WCHAR> pwszID;
            hr = pEndpoint->GetId(&pwszID);
            exit_on_failed(hr);

            CComPtr<IPropertyStore> pProps;
            hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
            exit_on_failed(hr);

            PROPVARIANT varName;
            PropVariantInit(&varName);
            hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
            exit_on_failed(hr);

            std::string endpoint_id = wchars_to_mbs((LPWSTR)pwszID);
            std::string endpoint_name = wchars_to_mbs(varName.pwszVal);

            // Add device info to endpoint_list, label them as playback or recording
            std::string label = (data_flow == eRender) ? "[Playback]" : "[Recording]";
            endpoint_list.push_back(std::make_pair(endpoint_id, label + " " + endpoint_name));

            if (endpoint_id == default_id) {
                default_index = i;
            }

            PropVariantClear(&varName);
        }
    }

    return default_index;
}

std::string audio_manager::get_default_endpoint()
{
    HRESULT hr {};

    CComPtr<IMMDeviceEnumerator> pEnumerator;
    hr = pEnumerator.CoCreateInstance(__uuidof(MMDeviceEnumerator));
    exit_on_failed(hr);

    CComPtr<IMMDevice> pEndpoint;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pEndpoint);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        return {};
    }
    exit_on_failed(hr);

    CComHeapPtr<WCHAR> pwszID;
    hr = pEndpoint->GetId(&pwszID);
    exit_on_failed(hr);

    return wchars_to_mbs((LPWSTR)pwszID);
}

void set_format(std::shared_ptr<AudioFormat> _format, WAVEFORMATEX* format)
{
    if (format->wFormatTag == WAVE_FORMAT_PCM || format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        _format->set_format_tag(format->wFormatTag);
    } else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto p = (WAVEFORMATEXTENSIBLE*)(format);
        if (IsEqualGUID(p->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            _format->set_format_tag(WAVE_FORMAT_PCM);
        } else if (IsEqualGUID(p->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            _format->set_format_tag(WAVE_FORMAT_IEEE_FLOAT);
        }
    }
    _format->set_channels(format->nChannels);
    _format->set_sample_rate(format->nSamplesPerSec);
    _format->set_bits_per_sample(format->wBitsPerSample);

    spdlog::info("WAVEFORMATEX: wFormatTag: {}, nBlockAlign: {}", format->wFormatTag, format->nBlockAlign);
    spdlog::info("AudioFormat:\n{}", _format->DebugString());
}

void print_endpoints(CComPtr<IMMDeviceCollection> pColletion)
{
    HRESULT hr {};

    UINT count {};
    hr = pColletion->GetCount(&count);
    exit_on_failed(hr);

    for (UINT i = 0; i < count; ++i) {
        CComPtr<IMMDevice> pEndpoint;
        hr = pColletion->Item(i, &pEndpoint);
        exit_on_failed(hr);

        CComHeapPtr<WCHAR> pwszID;
        hr = pEndpoint->GetId(&pwszID);
        exit_on_failed(hr);

        CComPtr<IPropertyStore> pProps;
        hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
        exit_on_failed(hr);

        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
        exit_on_failed(hr);

        spdlog::info("{}", wchars_to_utf8(varName.pwszVal));

        PropVariantClear(&varName);
    }
}

void exit_on_failed(HRESULT hr, const char* message, const char* func)
{
    if (FAILED(hr)) {
        spdlog::error("exit_on_failed {} {} {}", func, message, wchars_to_utf8(wstr_win_err(HRESULT_CODE(hr))));
        exit(-1);
    }
}

std::string wchars_to_mbs(const std::wstring& src)
{
    UINT cp = GetACP();
    int n = WideCharToMultiByte(cp, 0, src.data(), (int)src.size(), nullptr, 0, 0, 0);

    std::vector<char> buf(n);
    WideCharToMultiByte(cp, 0, src.data(), (int)src.size(), buf.data(), (int)buf.size(), 0, 0);
    std::string dst(buf.data(), buf.size());
    return dst;
}

std::string wchars_to_utf8(const std::wstring& src)
{
    UINT cp = CP_UTF8;
    int n = WideCharToMultiByte(cp, 0, src.data(), (int)src.size(), nullptr, 0, 0, 0);

    std::vector<char> buf(n);
    WideCharToMultiByte(cp, 0, src.data(), (int)src.size(), buf.data(), (int)buf.size(), 0, 0);
    std::string dst(buf.data(), buf.size());
    return dst;
}

std::wstring mbs_to_wchars(const std::string& src)
{
    UINT cp = GetACP();
    int n = MultiByteToWideChar(cp, 0, src.data(), (int)src.size(), nullptr, 0);

    std::vector<wchar_t> buf(n);
    MultiByteToWideChar(cp, 0, src.data(), (int)src.size(), buf.data(), (int)buf.size());
    std::wstring dst(buf.data(), buf.size());
    return dst;
}

std::string str_win_err(int err)
{
    LPSTR buf = nullptr;
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
        nullptr,
        err,
        0,
        (LPSTR)&buf,
        0,
        nullptr);
    std::string msg;
    if (buf) {
        msg.assign(buf);
        LocalFree(buf);
    }
    return msg;
}

std::wstring wstr_win_err(int err)
{
    LPWSTR buf = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
        nullptr,
        err,
        0,
        (LPWSTR)&buf,
        0,
        nullptr);
    std::wstring msg;
    if (buf) {
        msg.assign(buf);
        LocalFree(buf);
    }
    return msg;
}

#endif // _WINDOWS