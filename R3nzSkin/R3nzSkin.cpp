#pragma warning(disable : 6387 4715)

#include <Windows.h>
#include <clocale>
#include <chrono>
#include <thread>

#include "CheatManager.hpp"
#include "Config.hpp"
#include "Hooks.hpp"
#include "Memory.hpp"
#include "SDK/GameState.hpp"

// Anti-detection includes
#include "AntiDetection.hpp"
#include "CompileTimeRandom.hpp"

// Exported hook procedure for SetWindowsHookEx injection
// This function does nothing but allows the DLL to be loaded into the target process
extern "C" __declspec(dllexport) LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Exported hook procedure for GetMessage hook (alternative)
extern "C" __declspec(dllexport) LRESULT CALLBACK GetMsgProc(int nCode, WPARAM wParam, LPARAM lParam) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool WINAPI HideThread(const HANDLE hThread) noexcept
{
        __try {
                // Obfuscated string for "NtSetInformationThread"
                // Using simple stack string construction to avoid static string analysis
                wchar_t ntdllStr[] = { 'n','t','d','l','l','.','d','l','l','\0' };
                char funcStr[] = { 'N','t','S','e','t','I','n','f','o','r','m','a','t','i','o','n','T','h','r','e','a','d','\0' };

                using FnSetInformationThread = NTSTATUS(NTAPI*)(HANDLE ThreadHandle, UINT ThreadInformationClass, PVOID ThreadInformation, ULONG ThreadInformationLength);
                const auto NtSetInformationThread{ reinterpret_cast<FnSetInformationThread>(::GetProcAddress(::GetModuleHandleW(ntdllStr), funcStr)) };

                if (!NtSetInformationThread)
                        return false;

                // Use random thread info class offset (0x11 is ThreadHideFromDebugger)
                // We use 0x11 directly but could randomize the call timing
                const auto status{ NtSetInformationThread(hThread, 0x11u, nullptr, 0ul) };
                return status == 0x00000000;
        } __except (TRUE) {
                return false;
        }
        return false;
}

__declspec(safebuffers) static void WINAPI DllAttach([[maybe_unused]] LPVOID lp) noexcept
{
        using namespace std::chrono_literals;

        // Anti-sandbox delay
        RANDOM_DELAY();

        cheatManager.start();

        if (HideThread(::GetCurrentThread()))
                cheatManager.logger->addLog("Thread Hidden!\n");

        // Wait for game to be ready
        cheatManager.memory->Search(true);
        while (true) {
                std::this_thread::sleep_for(1s);

                if (!cheatManager.memory->client)
                        cheatManager.memory->Search(true);
                else if (cheatManager.memory->client->game_state == GGameState_s::Running)
                        break;
        }
        cheatManager.logger->addLog("GameClient found!\n");

        std::this_thread::sleep_for(500ms);
        cheatManager.memory->Search(false);
        cheatManager.logger->addLog("All offsets found!\n");
        std::this_thread::sleep_for(500ms);

        cheatManager.config->init();
        cheatManager.config->load();
        cheatManager.logger->addLog("CFG loaded!\n");

        cheatManager.hooks->install();

        // 注入成功提示
        ::MessageBoxW(nullptr, L"R3nzSkin 注入成功！\n请使用 Insert 键呼出或隐藏菜单。", L"R3nzSkin", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);

        // Auto-hide GUI when game starts running (stealth mode)
        // Set after hooks->install() to ensure GUI is initialized
        cheatManager.gui->is_open = false;
        cheatManager.logger->addLog("GUI hidden for stealth mode.\n");

        // Monitor game state and auto-show GUI when game ends
        auto lastGameState = GGameState_s::Running;
        while (cheatManager.cheatState) {
                std::this_thread::sleep_for(250ms);

                // Check if game state changed to finished/exiting
                if (cheatManager.memory->client) {
                        const auto currentState = cheatManager.memory->client->game_state;
                        if (lastGameState == GGameState_s::Running && 
                                (currentState == GGameState_s::Finished || currentState == GGameState_s::Exiting)) {
                                // Game ended, show GUI automatically
                                cheatManager.gui->is_open = true;
                                cheatManager.logger->addLog("Game ended, GUI shown.\n");
                        }
                        lastGameState = currentState;
                }
        }

        ::ExitProcess(0u);
}

__declspec(safebuffers) BOOL APIENTRY DllMain(const HMODULE hModule, const DWORD reason, [[maybe_unused]] LPVOID reserved)
{
        if (reason == DLL_PROCESS_ATTACH) {
                DisableThreadLibraryCalls(hModule);

                // Hook-based injector loads this DLL in injector process first to resolve exports.
                // Only run cheat initialization inside actual League game process.
                wchar_t processPath[MAX_PATH]{};
                if (::GetModuleFileNameW(nullptr, processPath, MAX_PATH)) {
                        if (::wcsstr(processPath, L"League of Legends.exe") == nullptr)
                                return TRUE;
                }

                // Environment Check
                if (AntiDetection::IsUnderMonitoring()) {
                        ::MessageBoxW(nullptr,
                                L"R3nzSkin 注入被拦截。\n检测到反作弊/监控环境，DLL 已停止加载。",
                                L"R3nzSkin",
                                MB_OK | MB_ICONWARNING | MB_TOPMOST);
                        return FALSE;
                }

                HideThread(::GetCurrentThread());
                std::setlocale(LC_ALL, ".utf8");

                // Create a named event to signal successful injection
                // This allows the injector to detect if the DLL is loaded
                // The event name is based on the current process ID
                wchar_t eventName[64];
                swprintf(eventName, 64, L"Global\\MM_%08X", ::GetCurrentProcessId());
                HANDLE hEvent = ::CreateEventW(nullptr, TRUE, TRUE, eventName);
                // Keep the event handle open - it will be closed when the process exits

                ::_beginthreadex(nullptr, 0u, reinterpret_cast<_beginthreadex_proc_type>(DllAttach), nullptr, 0u, nullptr);
                return TRUE;
        }

        return TRUE;
}
