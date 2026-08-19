#include "../pch.h"
#include "PipeProtocol.h"

#include "../utils/Logger.h"
#include "../utils/GuidUtils.h"
#include "../models/VirtualDisplay.h"
#include "../controller/VirtualDisplayService.h"
#include "TrayMenuBuilder.h"

#include <windows.h>
#include <string>
#include <sstream>
#include <map>
#include <thread>
#include <chrono>
#include "../utils/StringUtils.h"

namespace vdc {

    namespace {

        // -----------------------------------------------------------------------------
        // Helpers
        // -----------------------------------------------------------------------------

        static int hexVal(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return 0;
        }

    } // anonymous namespace

    // -----------------------------------------------------------------------------
    // Parse key=value&key2=value2
    // -----------------------------------------------------------------------------
    std::map<std::string, std::string> ParseKvForm(const std::string& s) {
        std::map<std::string, std::string> m;

        size_t pos = 0;
        while (pos < s.size()) {
            auto amp = s.find('&', pos);
            std::string tok = s.substr(pos, (amp == std::string::npos) ? std::string::npos : amp - pos);

            auto eq = tok.find('=');
            if (eq != std::string::npos) {
                m[tok.substr(0, eq)] = tok.substr(eq + 1);
            }

            if (amp == std::string::npos)
                break;

            pos = amp + 1;
        }

        return m;
    }

    // -----------------------------------------------------------------------------
    // Percent decode
    // -----------------------------------------------------------------------------
    std::string PercentDecode(const std::string& s) {
        std::string out;

        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                unsigned char v = (unsigned char)((hexVal(s[i + 1]) << 4) | hexVal(s[i + 2]));
                out.push_back((char)v);
                i += 2;
            }
            else if (s[i] == '+') {
                out.push_back(' ');
            }
            else {
                out.push_back(s[i]);
            }
        }

        return out;
    }

    // -----------------------------------------------------------------------------
    // Read entire message from pipe
    // -----------------------------------------------------------------------------
    bool ReadAllFromPipe(HANDLE hPipe, std::string& out) {
        out.clear();
        char buf[4096];
        DWORD read = 0;

        for (;;) {
            BOOL ok = ReadFile(hPipe, buf, sizeof(buf) - 1, &read, NULL);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_MORE_DATA && read > 0) {
                    buf[read] = 0;
                    out.append(buf, read);
                    continue;
                }
                return !out.empty();
            }

            if (read == 0)
                break;

            buf[read] = 0;
            out.append(buf, read);

            if (read < sizeof(buf) - 1)
                break;
        }

        return true;
    }

    // -----------------------------------------------------------------------------
    // Write entire message to pipe
    // -----------------------------------------------------------------------------
    bool WriteAllToPipe(HANDLE hPipe, const std::string& msg) {
        DWORD written = 0;
        return WriteFile(hPipe, msg.c_str(), (DWORD)msg.size(), &written, NULL)
            && written == msg.size();
    }

    // -----------------------------------------------------------------------------
    // Pipe server loop
    // -----------------------------------------------------------------------------
    void RunPipeServerLoop(const std::wstring& pipeName,
        VirtualDisplayService* service,
        std::mutex* serviceMutex)
    {
        while (true) {
            HANDLE hPipe = CreateNamedPipeW(
                (L"\\\\.\\pipe\\" + pipeName).c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                8192, 8192,
                0,
                NULL
            );

            if (hPipe == INVALID_HANDLE_VALUE) {
                LOG_ERROR("CreateNamedPipeW failed: %d", GetLastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            BOOL ok = ConnectNamedPipe(hPipe, NULL) ?
                TRUE :
                (GetLastError() == ERROR_PIPE_CONNECTED);

            if (!ok) {
                CloseHandle(hPipe);
                continue;
            }

            // Read verb line
            std::string verbLine;
            if (!ReadAllFromPipe(hPipe, verbLine)) {
                CloseHandle(hPipe);
                continue;
            }

            // Split verb from KV
            std::istringstream iss(verbLine);
            std::string verb;
            std::getline(iss, verb);

            std::string kvLine;
            std::getline(iss, kvLine);

            auto kv = ParseKvForm(kvLine);

            // ---------------------------------------------------------------------
            // Execute verb
            // ---------------------------------------------------------------------
            std::string response;

            {
                std::lock_guard<std::mutex> lk(*serviceMutex);

                if (verb == "create") {
                    VirtualDisplay cfg;

                    if (kv.count("deviceName"))
                        cfg.deviceName = StringToWString(PercentDecode(kv["deviceName"]));

                    if (kv.count("width"))
                        cfg.width = std::stoi(kv["width"]);

                    if (kv.count("height"))
                        cfg.height = std::stoi(kv["height"]);

                    if (kv.count("refresh"))
                        cfg.refreshRateMilliHz = std::stoi(kv["refresh"]);

                    if (kv.count("hdr"))
                        cfg.hdr = (kv["hdr"] == "1");

                    if (kv.count("primary"))
                        cfg.primary = (kv["primary"] == "1");

                    if (kv.count("adapter"))
                        cfg.adapterLuid = std::stoull(kv["adapter"]);

                    if (kv.count("guid")) {
                        auto guid = StringToGuid(kv["guid"]);
                        if (guid) {
                            response = service->CreateVirtualDisplay(cfg, guid)
                                ? "Successfully created virtual display"
                                : "Failed to create virtual display";
                        }
                        else {
                            response = "Invalid virtual display GUID";
                        }
                    }
                    else {
                        response = service->CreateVirtualDisplay(cfg)
                            ? "Successfully created virtual display"
                            : "Failed to create virtual display";
                    }
                }

                else if (verb == "remove") {
                    if (kv.count("guid")) {
                        auto g = StringToGuid(kv["guid"]);
                        if (g) {
                            response = service->RemoveVirtualDisplay(*g)
                                ? "Successfully removed virtual display"
                                : "Failed to remove virtual display";
                        }
                    }
                }

                else if (verb == "primary") {
                    if (kv.count("guid")) {
                        auto g = StringToGuid(kv["guid"]);
                        if (g) {
                            // response = service->SetPrimary(*g)
                            //    ? "Successfully set display as primary"
                            //    : "Failed to set display as primary";
                        }
                    }
                }

                else if (verb == "hdr") {
                    if (kv.count("guid") && kv.count("enable")) {
                        auto g = StringToGuid(kv["guid"]);
                        if (g) {
                            bool enable = (kv["enable"] == "1");
                            // response = service->SetHdr(*g, enable)
                            //    ? "Successfully set HDR on display"
                            //    : "Failed to set HDR on display";
                        }
                    }
                }

                else if (verb == "mode") {
                    if (kv.count("guid") && kv.count("w") && kv.count("h") &&
                        kv.count("refresh") && kv.count("iso"))
                    {
                        auto g = StringToGuid(kv["guid"]);
                        if (g) {
                            //bool ok = service->SetMode(
                            //    *g,
                            //    std::stoi(kv["w"]),
                            //    std::stoi(kv["h"]),
                            //    std::stoi(kv["refresh"]),
                            //    std::stoi(kv["iso"]) != 0
                            //);
                            // 
                            //response = ok
                            //    ? "Successfully set mode on display"
                            //    : "Failed to set mode on display";
                        }
                    }
                }

                else if (verb == "query") {
                    if (kv.count("guid")) {
                        /*auto g = StringToGuid(kv["guid"]);
                        if (g) {
                            //response = service->Query(*g)
                            //    ? "QUERY RESULTS"
                            //    : "Failed to query display";
                        }*/
                    }
                }

                else if (verb == "exit") {
                    response = "Closing...";
                    WriteAllToPipe(hPipe, response);
                    CloseHandle(hPipe);
                    extern TrayContext g_ctx;
                    if (g_ctx.hwnd) {
                        PostMessageW(g_ctx.hwnd, WM_CLOSE, 0, 0);
                    }
                    else {
                        PostQuitMessage(0);
                    }
                    return;
                }
            }

            WriteAllToPipe(hPipe, response);
            CloseHandle(hPipe);
        }
    }

} // namespace vdc
