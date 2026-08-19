#include "../../pch.h"
#include "SudovdaDriver.h"

#include "../../utils/Logger.h"
#include "../../utils/GuidUtils.h"

#include "../../third_party/sudovda/sudovda.h"
#include "../../third_party/sudovda/sudovda-ioctl.h"

#include <thread>
#include <chrono>
#include <cmath>
#include <codecvt>
#include <locale>

namespace vdc {

    using namespace SUDOVDA;

    // -----------------------------------------------------------------------------
    // Construction / Destruction
    // -----------------------------------------------------------------------------

    SudovdaDriver::SudovdaDriver() = default;

    SudovdaDriver::~SudovdaDriver() {
        CloseInternal();
    }

    SudovdaDriver::SudovdaDriver(SudovdaDriver&& other) noexcept {
        m_handle = other.m_handle;
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    SudovdaDriver& SudovdaDriver::operator=(SudovdaDriver&& other) noexcept {
        if (this != &other) {
            CloseInternal();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    // -----------------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------------

    SudovdaDriver::Status SudovdaDriver::Open() {
        if (m_handle != INVALID_HANDLE_VALUE)
            return Status::Ok;

        return OpenInternal();
    }

    bool SudovdaDriver::IsOpen() const noexcept {
        return m_handle != INVALID_HANDLE_VALUE;
    }

    void SudovdaDriver::Close() {
        CloseInternal();
    }

    bool SudovdaDriver::StartWatchdog(std::function<void()> failCb) {
        if (!IsOpen())
            return false;

        return StartWatchdogInternal(std::move(failCb));
    }

    bool SudovdaDriver::SetRenderAdapterByName(const std::wstring& adapterName) {
        if (!IsOpen())
            return false;

        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        UINT i = 0;

        while (factory->EnumAdapters(i, &adapter) == S_OK) {
            i++;

            if (FAILED(adapter->GetDesc(&desc)))
                continue;

            if (std::wstring_view(desc.Description) != adapterName)
                continue;

            if (SetRenderAdapter(m_handle, desc.AdapterLuid))
                return true;
        }

        return false;
    }

    std::optional<std::wstring> SudovdaDriver::CreateVirtualDisplay(
        const char* clientUid,
        const char* clientName,
        uint32_t width,
        uint32_t height,
        float fps,
        const GUID& guid
    ) {
        if (Open() != Status::Ok)
            return std::nullopt;

        if (!IsOpen())
            return std::nullopt;

        // Convert Hz (possibly fractional) to milliHz and round
        UINT refresh_millihz = static_cast<UINT>(std::lroundf(fps * 1000.0f));

        float dbgHz = static_cast<float>(refresh_millihz) / 1000.0f;
        LOG_INFO("Creating virtual display %s %ux%u @ %.2fHz", clientName, width, height, dbgHz);

        VIRTUAL_DISPLAY_ADD_OUT output{};
        if (!AddVirtualDisplay(
            m_handle,
            width,
            height,
            refresh_millihz,
            guid,
            clientName,
            clientUid,
            output))
        {
            LOG_ERROR("Failed to add virtual display.");
            return std::nullopt;
        }

        // Poll for GDI device name
        wchar_t deviceName[CCHDEVICENAME]{};
        constexpr uint32_t pollIntervalMs = 100;
        constexpr uint32_t discoveryTimeoutMs = 5000;

        uint32_t elapsedMs = 0;
        while (!GetAddedDisplayName(output, deviceName)) {
            if (elapsedMs >= discoveryTimeoutMs) {
                LOG_WARN("Cannot get name for newly added virtual display after %u ms",
                    discoveryTimeoutMs);
                return std::nullopt;
            }

            // Keep the driver's watchdog alive while Windows publishes the new
            // display topology. The regular watchdog thread starts once the GDI
            // device name has been discovered.
            if (elapsedMs > 0 && elapsedMs % 1000 == 0 && !PingDriver(m_handle)) {
                LOG_WARN("Failed to ping SudoVDA while waiting for the display name");
            }

            Sleep(pollIntervalMs);
            elapsedMs += pollIntervalMs;
        }

        std::wstring dev(deviceName);
        if (dev.empty()) {
            LOG_ERROR("Virtual display created but name retrieval failed");
            return std::nullopt;
        }

        LOG_INFO("Virtual display added: %ls", dev.c_str());

        // Start watchdog
        StartWatchdog([dev]() {
            LOG_ERROR("Watchdog detected driver failure for device: %ls", dev.c_str());
            });

        return dev;
    }

    bool SudovdaDriver::RemoveVirtualDisplay(const GUID& guid) {
        if (Open() != Status::Ok)
            return false;

        if (!IsOpen())
            return false;

        if (SUDOVDA::RemoveVirtualDisplay(m_handle, guid)) {
            LOG_SUCCESS("Virtual display removed.");
            return true;
        }

        LOG_ERROR("Failed to remove virtual display.");
        return false;
    }

    // -----------------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------------

    SudovdaDriver::Status SudovdaDriver::OpenInternal() {
        uint32_t retry = 20;

        while (true) {
            m_handle = OpenDevice(&SUVDA_INTERFACE_GUID);

            if (m_handle != INVALID_HANDLE_VALUE)
                break;

            if (retry > 320) {
                LOG_ERROR("OpenDevice failed");
                return Status::Failed;
            }

            Sleep(retry);
            retry *= 2;
        }

        if (!CheckProtocolCompatibleInternal()) {
            LOG_ERROR("SUDOVDA protocol not compatible");
            CloseInternal();
            return Status::VersionIncompatible;
        }

        return Status::Ok;
    }

    void SudovdaDriver::CloseInternal() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    bool SudovdaDriver::CheckProtocolCompatibleInternal() {
        return CheckProtocolCompatible(m_handle);
    }

    bool SudovdaDriver::StartWatchdogInternal(std::function<void()> failCb) {
        VIRTUAL_DISPLAY_GET_WATCHDOG_OUT wd{};
        if (!GetWatchdogTimeout(m_handle, wd)) {
            LOG_ERROR("Watchdog fetch failed");
            return false;
        }

        if (!wd.Timeout)
            return true;

        uint32_t sleepInterval = wd.Timeout * 1000 / 3;

        std::thread([this, sleepInterval, failCb = std::move(failCb)]() mutable {
            uint8_t failCount = 0;

            while (true) {
                if (!sleepInterval)
                    return;

                if (!PingDriver(m_handle)) {
                    failCount++;
                    if (failCount > 3) {
                        failCb();
                        return;
                    }
                }

                Sleep(sleepInterval);
            }
            }).detach();

        return true;
    }

} // namespace vdc
