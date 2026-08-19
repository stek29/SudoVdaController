#include "../pch.h"

#include "VirtualDisplayService.h"
#include "driver/SudovdaDriver.h"

#include "../utils/JsonUtils.h"
#include "../utils/GuidUtils.h"
#include "../utils/DisplayConfigUtils.h"
#include "../utils/HdrUtils.h"
#include "../utils/StringUtils.h"
#include "../utils/ContainerUtils.h"
#include "../utils/ConfigStore.h"

#include "../utils/Logger.h"

#include <iostream>
#include <locale>
#include <codecvt>
#include <thread>
#include <unordered_set>
#include <regex>

using namespace vdc;
using namespace vdisplay;

static std::wstring MakeUniqueDeviceName(const std::wstring& requested, const std::map<GUID, std::shared_ptr<VirtualDisplay>>& virtualDisplays);

VirtualDisplayService::VirtualDisplayService() {
    m_sudoVdaDriver = new SudovdaDriver();
    configStore_ = std::make_unique<ConfigStore>();
}

VirtualDisplayService::~VirtualDisplayService() = default;

bool VirtualDisplayService::CreateVirtualDisplay(const VirtualDisplay& cfg, const std::optional<GUID>& guidOpt) {

    try {

        VirtualDisplay virtualDisplay = cfg;
        TrimWhitespace(virtualDisplay.deviceName);

        DisplayConfig displayConfig = FindExistingDisplayConfigOrGenerate(virtualDisplay, guidOpt);
	    GUID displayId = StringToGuid(displayConfig.displayId).value();
        float fpsHz = static_cast<float>(virtualDisplay.refreshRateMilliHz) / 1000.0f;
        bool isDefaultDisplay = virtualDisplay.deviceName == DEFAULT_VIRTUAL_DISPLAY_DEVICE_NAME;

        if (virtualDisplay.deviceName.empty())
        {
            throw std::exception("Device name cannot be empty");
        }

        std::wstring newName = MakeUniqueDeviceName(virtualDisplay.deviceName, virtualDisplays_);
        if (newName != virtualDisplay.deviceName) {
            std::string oldNameStr = WStringToString(virtualDisplay.deviceName);
            std::string newNameStr = WStringToString(newName);
            LOG_WARN("A virtual display with the name '%s' already exists. Renamed to '%s'", oldNameStr.c_str(), newNameStr.c_str());
            virtualDisplay.deviceName = newName;
        }

        auto gdiName = m_sudoVdaDriver->CreateVirtualDisplay(displayConfig.displayId.c_str(), 
                                                             WStringToString(virtualDisplay.deviceName).c_str(),
                                                             virtualDisplay.width, virtualDisplay.height, fpsHz, 
                                                             displayId);

        if (!gdiName) {
            LOG_ERROR("Failed to add virtual display");
            return false;
        }

        virtualDisplay.gdiName = *gdiName;
        auto gdiNameResult = virtualDisplay.gdiName;
        auto session = std::make_shared<VirtualDisplay>(virtualDisplay);
        virtualDisplays_.emplace(displayId, session);

        if (cfg.hdr) {

            std::string devNameCopy = WStringToString(virtualDisplay.deviceName);
            auto virtualDisplayShared = session;

            std::thread([this, gdiNameResult, devNameCopy, virtualDisplayShared]() {
                auto res = SetHdr(gdiNameResult, true);
                if (res) {
                    LOG_INFO("Succesfully enabled HDR on device '%s'", devNameCopy);
                } else {
                    LOG_ERROR("Failed to enable HDR on device '%s'", devNameCopy);
                }
            }).detach();
        }

        if (cfg.primary) {

            std::string devNameCopy = WStringToString(virtualDisplay.deviceName);
            auto virtualDisplayShared = session;

            std::thread([this, gdiNameResult, devNameCopy, virtualDisplayShared]() {

                auto res = SetPrimary(gdiNameResult);
                if (res) {
                    LOG_INFO("Succesfully set '%s' as primary device", devNameCopy);
                } else {
                    LOG_ERROR("Failed to set '%s' as primary device", devNameCopy);
                }
            }).detach();
        }

        if (isDefaultDisplay) LOG_WARN("Default display detected. No configuration will be stored.");

        if (configStore_ && !isDefaultDisplay) {

            DisplayConfig cfgCopy = displayConfig;
            std::string devNameCopy = WStringToString(virtualDisplay.deviceName);
            auto virtualDisplayShared = session;

            std::thread([this, displayId, virtualDisplayShared, cfgCopy, devNameCopy]() mutable {
                Sleep(5000);
                auto res = AddNewDisplayToConfigStore(displayId, *virtualDisplayShared, cfgCopy);
            }).detach();
        }

        std::string createdName = WStringToString(newName);
        std::string createdGdi = WStringToString(gdiNameResult);
        std::string createdId = displayConfig.displayId;
        std::string successMsg = "Created '" + createdName + "' (" + createdGdi + ") with id: " + createdId;
        LOG_SUCCESS(successMsg.c_str());
        return true;
    }
    catch (const std::exception& ex) {
        LOG_ERROR("Failed to create virtual display: %s", ex.what());
        return false;
    }
}

bool VirtualDisplayService::RemoveVirtualDisplay(const GUID& guid) {

    try {
        if (guid == GUID{}) {
            throw std::exception("Invalid GUID");
        }

        auto virtualDisplay = virtualDisplays_.find(guid);
        if (virtualDisplay == virtualDisplays_.end()) {
            throw std::exception("The virtual display could not be found");
        }

        if (configStore_ && virtualDisplay->second->deviceName.find(DEFAULT_VIRTUAL_DISPLAY_DEVICE_NAME) == std::wstring::npos) {
            auto displayConfigOpt = configStore_->GetByDisplayId(GuidToString(guid));
            if (displayConfigOpt.has_value()) {
                auto displayConfig = UpdateConfigTopogology(displayConfigOpt.value(), true);
                configStore_->SaveDisplayConfig(WStringToString(virtualDisplay->second->deviceName), displayConfig);
            }
            else {
                auto guidString = GuidToString(guid);
                LOG_WARN("No virtual display config was found in the store for removed display: %s", guidString.c_str());
            }
        }

        if (!m_sudoVdaDriver->RemoveVirtualDisplay(guid)) {
            throw std::exception("Driver failed to remove virtual display");
        }

        virtualDisplays_.erase(virtualDisplay);
        
        return true;
    } catch (const std::exception& ex) {
		std::string guidStr = GuidToString(guid);
		LOG_ERROR("Failed to remove virtual display with id (%s): %s", guidStr.c_str(), ex.what());
        return false;
	}
}

bool VirtualDisplayService::SetMode(const std::wstring gdiName, int w, int h, int refreshMilliHz, bool isolatedLayout) {
    try {
        if (gdiName.empty()) {
            throw std::exception("GdiName was empty");
        }

        if (gdiName.rfind('\\.\\\\', 0) == 0) {
            throw std::exception("Invalid gdiName");
        }

        if (!DisplayConfigUtils::ApplyModeForDevice(gdiName, w, h, refreshMilliHz, isolatedLayout)) {
            throw std::exception("Failed to set mode on display");
		}

        return true;
    } catch (const std::exception& ex) {
		std::string gdiNameStr = WStringToString(gdiName);
        LOG_ERROR("Failed to set mode with gdiName (%s) : %s", gdiNameStr, ex.what());
        return false;
    }
}

bool VirtualDisplayService::SetPrimary(const std::wstring gdiName) {
    try {
        if (gdiName.empty()) {
            throw std::exception("GdiName was empty");
        }

        if (gdiName.rfind('\\.\\\\', 0) == 0) {
            throw std::exception("Invalid gdiName");
        }

        if (!DisplayConfigUtils::MakeDevicePrimary(gdiName)) {
			throw std::exception("Failed to set display as primary");
        }

        return true;
    } catch (const std::exception& ex) {
        std::string gdiNameStr = WStringToString(gdiName);
        LOG_ERROR("Failed to set primary with gdiName (%s) : %s", gdiNameStr, ex.what());
        return false;
    }
}

bool VirtualDisplayService::DisplaySupportsHdr(const std::wstring gdiName) {
    try {
        if (gdiName.empty()) {
            throw std::exception("GdiName was empty");
        }

        if (gdiName.rfind('\\.\\\\', 0) == 0) {
            throw std::exception("Invalid gdiName");
        }

        return HdrUtils::DisplaySupportsHDR(gdiName);
    }
    catch (const std::exception& ex) {
        std::string gdiNameStr = WStringToString(gdiName);
        LOG_ERROR("Failed to see HDR support for display with gdiName (%s) : %s", gdiName, ex.what());
        return false;
    }
}

bool VirtualDisplayService::IsHdrEnabled(const std::wstring gdiName) {
    try {
        if (gdiName.empty()) {
            throw std::exception("GdiName was empty");
        }

        if (gdiName.rfind('\\.\\\\', 0) == 0) {
            throw std::exception("Invalid gdiName");
        }

        return HdrUtils::IsHdrEnabled(gdiName);
    }
    catch (const std::exception& ex) {
        std::string gdiNameStr = WStringToString(gdiName);
        LOG_ERROR("Failed to get HDR state for display with gdiName (%s) : %s", gdiName, ex.what());
        return false;
    }
}

bool VirtualDisplayService::SetHdr(const std::wstring gdiName, bool enable) {
    try {
        if (gdiName.empty()) {
            throw std::exception("GdiName was empty");
        }

        if (gdiName.rfind('\\.\\\\', 0) == 0) {
            throw std::exception("Invalid gdiName");
        }
        
        if (!HdrUtils::SetHdrState(gdiName, enable)) {
			throw std::exception("Failed to set HDR state on display");
        }

        return true;
    } catch (const std::exception& ex) {
        std::string gdiNameStr = WStringToString(gdiName);
		std::string colorSpace = enable ? "HDR" : "SDR";
        LOG_ERROR("Failed to set %s with gdiName (%s) : %s", colorSpace, gdiName, ex.what());
        return false;
    }
}

bool VirtualDisplayService::IsDisplayEnabled(const std::string edid) {
    try {
        if (edid.empty()) {
            throw std::exception("EDID was empty");
        }

        return DisplayConfigUtils::IsDisplayEnabled(edid);
    }
    catch (const std::exception& ex) {
        LOG_ERROR("Failed to get enabled state for display with EDID (%s) : %s", edid, ex.what());
        return false;
    }
}

bool VirtualDisplayService::SetDisplayEnabled(const std::string edid, bool enable) {
    try {
        if (edid.empty()) {
            throw std::exception("EDID was empty");
        }

        if (!vdc::DisplayConfigUtils().SetDisplayEnabled(edid, enable)) {
            LOG_ERROR("Failed to apply merged topology from config store");
            return false;
        }

        return true;
    }
    catch (const std::exception& ex) {
        LOG_ERROR("Failed to set enabled state with EDID (%s) : %s", edid, ex.what());
        return false;
    }
}

bool VirtualDisplayService::Query(const std::wstring gdiName) {
    try {
        if (gdiName.empty()) {
            throw std::exception("GdiName was empty");
        }

        if (gdiName.rfind('\\.\\\\', 0) == 0) {
            throw std::exception("Invalid gdiName");
        }

        auto mode = DisplayConfigUtils::GetCurrentModeForDevice(gdiName);
        if (!mode) {
            LOG_WARN("Mode returned 'unknown'");
            return false;
        }

        std::string gdiNameStr = WStringToString(gdiName);
	    auto colorSpace = HdrUtils::IsHdrEnabled(gdiName) ? "HDR" : "SDR";
        LOG_INFO("Mode Found:");
        LOG_INFO("%s %sx%s@%s %s", gdiNameStr, std::to_string(mode->width), std::to_string(mode->height), std::to_string(mode->refreshRateMilliHz), colorSpace);

        return true;
    }
    catch (const std::exception& ex) {
        std::string gdiNameStr = WStringToString(gdiName);
        LOG_ERROR("Query failed with gdiName (%s) : %s", gdiNameStr, ex.what());
        return false;
    }
}

size_t VirtualDisplayService::CountDisplays() const {
    return virtualDisplays_.size();
}

const std::map < GUID, std::shared_ptr<vdc::VirtualDisplay>>& VirtualDisplayService::GetVirtualDisplays() const {
    return virtualDisplays_;
}

bool VirtualDisplayService::AddNewDisplayToConfigStore(GUID displayId,
                                                       VirtualDisplay virtualDisplay,
                                                       DisplayConfig displayConfig)
{
    // Verify mode
    auto mode = DisplayConfigUtils::GetCurrentModeForDevice(virtualDisplay.gdiName);
    if (mode) {
        displayConfig.displayId = vdc::GuidToString(displayId);
        displayConfig.width = mode->width;
        displayConfig.height = mode->height;
        displayConfig.refreshRateHz = mode->refreshRateMilliHz;
    }

    // Merge topology of display with global topology
    displayConfig = UpdateConfigTopogology(displayConfig, false);
    auto requestedTopology = configStore_->GetCombinedTopology(&displayConfig,
                                                               configStore_->GetTopologyMergePolicyDisabledWins());

    LOG_INFO("Applying topology:");
    for (const auto& t : requestedTopology) {
        std::string message = "  (" + std::string(t.enabled ? "+" : " ") + ") " + t.displayName;
        LOG_INFO(message.c_str());
    }

    if (!vdc::DisplayConfigUtils().ApplyDisplayConfig(requestedTopology)) {
        LOG_ERROR("Failed to apply merged topology from config store");
        return false;
    }

    LOG_INFO("Succesfully applied merged topology from config store");
    displayConfig.topology = requestedTopology;
    
    if (!configStore_->SaveDisplayConfig(WStringToString(virtualDisplay.deviceName), displayConfig)) {
        LOG_ERROR("Failed to save display config to store");
    }
        
    LOG_INFO("Succesfully saved display config to store");
    return true;
}


DisplayConfig VirtualDisplayService::FindExistingDisplayConfigOrGenerate(const VirtualDisplay& cfg, const std::optional<GUID>& guidOpt) {

    if (auto mappedOpt = configStore_->GetByNameAndMode(cfg.deviceName, cfg.width, cfg.height, cfg.refreshRateMilliHz); 
        mappedOpt.has_value() && !mappedOpt->displayId.empty()) {
        // A caller-supplied GUID is the display's lifecycle identity and must
        // take precedence over an ID retained in a matching stored topology.
        // Keep the rest of the stored configuration, but bind this instance to
        // the requested GUID so a later remove command can address it reliably.
        if (guidOpt) {
            mappedOpt->displayId = vdc::GuidToString(*guidOpt);
        }
        return mappedOpt.value();
    }

    GUID g = guidOpt.value_or(GenerateGuid());

    DisplayConfig dCfg;
    dCfg.displayId = vdc::GuidToString(g);
    dCfg.width = cfg.width;
    dCfg.height = cfg.height;
    dCfg.refreshRateHz = cfg.refreshRateMilliHz;

    return dCfg;
}

DisplayConfig VirtualDisplayService::UpdateConfigTopogology(DisplayConfig displayConfig, bool overrideEnabled)
{
    auto physicalTopologies = GetCurrentTopology();
    for (const auto& phys : physicalTopologies) {
        auto match = vdc::FindByKey(displayConfig.topology, phys.edid, [](const Topology& t) { return t.edid; });
        if (match != displayConfig.topology.end()) {
            if (overrideEnabled) {
                match->enabled = phys.enabled;
            }
        }
        else {
            displayConfig.topology.push_back(phys);
        }
    }
    return displayConfig;
}

std::vector<vdc::Topology> VirtualDisplayService::GetCurrentTopology()
{
    auto utils = DisplayConfigUtils();
    std::vector<std::pair<GUID, std::wstring>> vds;
    vds.reserve(virtualDisplays_.size());
    for (const auto& kv : virtualDisplays_) vds.push_back({ kv.first, kv.second->gdiName });
    return utils.GetActiveDisplayTopology(vds);
}

static std::wstring MakeUniqueDeviceName(const std::wstring& requested,
                                         const std::map<GUID, std::shared_ptr<VirtualDisplay>>& virtualDisplays)
{
    auto nameExists = [&](const std::wstring& name) {
        for (const auto& vd : virtualDisplays) {
            if (vd.second->deviceName == name) return true;
        }
        return false;
        };

    std::wstring originalName = requested;
    std::wstring candidateName = originalName;

    std::wregex reSuffix(L"^(.*)_(\\d+)$");
    std::wstring baseName = originalName;
    int suffix = 1;
    std::wsmatch match;
    if (std::regex_match(originalName, match, reSuffix) && match.size() == 3) {
        baseName = match[1].str();
        try {
            suffix = std::stoi(match[2].str()) + 1;
        }
        catch (...) {
            suffix = 1;
        }
    }

    while (nameExists(candidateName)) {
        candidateName = baseName + L"_" + std::to_wstring(suffix);
        ++suffix;
    }

    return candidateName;
}
