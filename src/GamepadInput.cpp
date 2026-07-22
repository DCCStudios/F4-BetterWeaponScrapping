#include "PCH.h"
#include "GamepadInput.h"
#include "ScrapModManager.h"
#include "ScrapOverlay.h"
#include "Settings.h"

#include "F4SE/API.h"
#include "F4SE/Interfaces.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <Xinput.h>
#include <tlhelp32.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <filesystem>

// ===========================================================================
// XInput suppression + gamepad "open picker" button.
//
// Adapted from the verified-working F4SE Menu Framework 3 implementation
// (F4SE-Menu-Framework-3/src/GamepadInput.cpp). Why IAT hooks, not inline:
//  * Fallout4.exe statically imports xinput1_3.dll — patching that import
//    slot is what actually blinds the game engine. Other modules (ENB, other
//    plugins) may import other variants, so all variants are covered.
//  * Steam Input often inline-hooks XInputGetState inside the DLL to
//    translate DualShock/DualSense pads; IAT patching composes cleanly with
//    that (we forward to the export address, which runs Steam's chain).
//  * ImGui's Win32 backend resolves XInputGetState with LoadLibrary +
//    GetProcAddress — that bypasses import tables, so ImGui keeps seeing the
//    real controller and its gamepad navigation works while the game sees
//    a zeroed pad.
// ===========================================================================
namespace
{
	using XInputGetStateFn = DWORD(WINAPI*)(DWORD a_userIndex, XINPUT_STATE* a_state);

	struct XInputDllSlot
	{
		const wchar_t*   name;              // DLL file name
		HMODULE          module{ nullptr }; // handle if loaded in this process
		XInputGetStateFn real{ nullptr };   // export address (may be Steam-hooked — fine)
		// What our hook forwards to. Starts as the export, but is replaced by
		// whatever pointer the patched IAT slot held at patch time. That way,
		// if another plugin (F4SE Menu Framework 3 does exactly this) already
		// IAT-hooked XInputGetState, we CHAIN through its hook instead of
		// silently cutting it out of the call path — both plugins' gamepad
		// suppression keeps working. (MF3 forwards straight to the export, so
		// it must stay downstream of us; see Install() timing note.)
		XInputGetStateFn chain{ nullptr };
		// Export ordinal of XInputGetState in THIS dll, resolved at runtime
		// from its export directory. Needed because Fallout4.exe imports
		// xinput1_3.dll BY ORDINAL ONLY (verified: import thunks are ordinals
		// 4, 2, 3 — no name table) — and ordinal imports cannot be matched by
		// comparing the IAT slot against the export address once another
		// plugin (MF3) has already patched the slot. 0 = unresolved.
		WORD getStateOrdinal{ 0 };
	};

	// Order matters only for logging; every loaded variant gets patched.
	XInputDllSlot g_xinputDlls[] = {
		{ L"xinput1_3.dll" },    // the game's own static import (DirectX SDK era)
		{ L"xinput1_4.dll" },    // Windows 8+ (ImGui backend prefers this one)
		{ L"xinput9_1_0.dll" },  // Windows Vista+ generic
		{ L"xinputuap.dll" },    // UWP shim on some Win10/11 setups
	};
	constexpr std::size_t kXInputDllCount = std::size(g_xinputDlls);

	std::atomic<bool> g_suppressing{ false };

	// Per-user-index release linger: the button that CLOSED a BWS menu is
	// physically still held on the game's very next poll — without this the
	// game would treat it as a fresh press (e.g. B backing out of the whole
	// workbench). Buttons/triggers stay hidden until the pad reports fully
	// released once; sticks pass through so movement resumes instantly.
	std::atomic<bool> g_lingerSuppress[4] = {};

	// Rising-edge tracker for the "open scrap-mod picker" pad button, per
	// user index, read/written only inside the hook.
	WORD g_prevButtons[4] = {};

	// True while an open-picker task is queued, so a held button cannot
	// enqueue the task once per poll.
	std::atomic<bool> g_openQueued{ false };

	[[nodiscard]] bool AnyBwsMenuOpen()
	{
		return ScrapOverlay::IsPopupVisible() || BWS::ScrapModManager::BlocksGameInput();
	}

	DWORD WINAPI HookedCommon(std::size_t a_dllIndex, DWORD a_userIndex, XINPUT_STATE* a_state)
	{
		XInputGetStateFn next = g_xinputDlls[a_dllIndex].chain;
		if (!next) {
			next = g_xinputDlls[a_dllIndex].real;
		}
		if (!next) {
			return ERROR_DEVICE_NOT_CONNECTED;
		}

		const DWORD       result = next(a_userIndex, a_state);
		const std::size_t idx = (a_userIndex < 4) ? a_userIndex : 0;

		if (result != ERROR_SUCCESS || !a_state) {
			g_lingerSuppress[idx].store(false, std::memory_order_relaxed);
			g_prevButtons[idx] = 0;
			return result;
		}

		// --------------------------------------------------------- open key
		// While the workbench ExamineMenu is up and none of our menus are,
		// a rising edge on the configured button opens the scrap-mod picker
		// (gamepad equivalent of the keyboard hotkey). The press is also
		// masked out of what the game sees so the ExamineMenu underneath
		// doesn't react to it. Everything the picker touches must run on the
		// game's main thread, so the actual open is deferred to an F4SE task.
		const WORD buttons = a_state->Gamepad.wButtons;
		const WORD openMask = static_cast<WORD>(BWS::Settings::Get().scrapModGamepadButton.load());
		if (openMask != 0 && !AnyBwsMenuOpen() && BWS::ScrapModManager::IsExamineMenuOpen()) {
			const bool down = (buttons & openMask) != 0;
			const bool was = (g_prevButtons[idx] & openMask) != 0;
			if (down) {
				// Consume the button even while held.
				a_state->Gamepad.wButtons &= static_cast<WORD>(~openMask);
			}
			if (down && !was && !g_openQueued.exchange(true)) {
				if (const auto* tasks = F4SE::GetTaskInterface()) {
					tasks->AddTask([]() {
						BWS::ScrapModManager::OpenPickerFromExternal();
						g_openQueued.store(false, std::memory_order_release);
					});
				} else {
					g_openQueued.store(false, std::memory_order_release);
				}
			}
		}
		g_prevButtons[idx] = buttons;

		// ------------------------------------------------------ suppression
		if (AnyBwsMenuOpen()) {
			g_suppressing.store(true, std::memory_order_relaxed);
			g_lingerSuppress[idx].store(true, std::memory_order_relaxed);
			// Keep connection status + packet number, hide all input.
			const DWORD packet = a_state->dwPacketNumber;
			std::memset(a_state, 0, sizeof(XINPUT_STATE));
			a_state->dwPacketNumber = packet;
		} else {
			g_suppressing.store(false, std::memory_order_relaxed);
			if (g_lingerSuppress[idx].load(std::memory_order_relaxed)) {
				const bool anyHeld =
					a_state->Gamepad.wButtons != 0 ||
					a_state->Gamepad.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
					a_state->Gamepad.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
				if (anyHeld) {
					// Still holding the press that closed the menu — hide
					// buttons/triggers, let sticks through.
					a_state->Gamepad.wButtons = 0;
					a_state->Gamepad.bLeftTrigger = 0;
					a_state->Gamepad.bRightTrigger = 0;
				} else {
					g_lingerSuppress[idx].store(false, std::memory_order_relaxed);
				}
			}
		}
		return result;
	}

	// An IAT slot can only point at a plain function, so one concrete hook
	// per DLL variant with the index baked in.
	DWORD WINAPI Hooked0(DWORD i, XINPUT_STATE* s) { return HookedCommon(0, i, s); }
	DWORD WINAPI Hooked1(DWORD i, XINPUT_STATE* s) { return HookedCommon(1, i, s); }
	DWORD WINAPI Hooked2(DWORD i, XINPUT_STATE* s) { return HookedCommon(2, i, s); }
	DWORD WINAPI Hooked3(DWORD i, XINPUT_STATE* s) { return HookedCommon(3, i, s); }
	XInputGetStateFn const g_hookFns[kXInputDllCount] = { &Hooked0, &Hooked1, &Hooked2, &Hooked3 };

	// Resolves the export ordinal of a named function from a loaded module's
	// in-memory export directory. Returns 0 if not found. Needed to identify
	// ordinal-only imports (Fallout4.exe imports xinput1_3 purely by ordinal).
	WORD FindExportOrdinal(HMODULE a_module, const char* a_fnName)
	{
		auto* base = reinterpret_cast<std::uint8_t*>(a_module);
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return 0;
		}
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			return 0;
		}
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (dir.VirtualAddress == 0 || dir.Size == 0) {
			return 0;
		}
		auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
		auto* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
		auto* ords = reinterpret_cast<WORD*>(base + exp->AddressOfNameOrdinals);
		for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
			const char* nm = reinterpret_cast<const char*>(base + names[i]);
			if (std::strcmp(nm, a_fnName) == 0) {
				return static_cast<WORD>(ords[i] + exp->Base);
			}
		}
		return 0;
	}

	// Matches an import descriptor's ANSI DLL name against our variant table
	// (case-insensitive, ASCII-only names). SIZE_MAX = not an XInput DLL.
	std::size_t MatchXInputDll(const char* a_importName)
	{
		for (std::size_t idx = 0; idx < kXInputDllCount; ++idx) {
			const wchar_t* w = g_xinputDlls[idx].name;
			const char*    a = a_importName;
			bool           match = true;
			while (*w && *a) {
				if (towlower(*w) != static_cast<wint_t>(std::tolower(static_cast<unsigned char>(*a)))) {
					match = false;
					break;
				}
				++w;
				++a;
			}
			if (match && *w == 0 && *a == 0) {
				return idx;
			}
		}
		return SIZE_MAX;
	}

	// Patches XInputGetState import entries in one module's IAT. Returns the
	// number of entries patched.
	int PatchModuleIAT(HMODULE a_module, const wchar_t* a_moduleName)
	{
		auto* base = reinterpret_cast<std::uint8_t*>(a_module);

		// Validate PE headers defensively — some (packed/hooked) modules
		// have odd layouts.
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return 0;
		}
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			return 0;
		}

		const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
			return 0;
		}

		int   patched = 0;
		auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
		for (; desc->Name != 0; ++desc) {
			const char*       dllName = reinterpret_cast<const char*>(base + desc->Name);
			const std::size_t dllIdx = MatchXInputDll(dllName);
			if (dllIdx == SIZE_MAX || !g_xinputDlls[dllIdx].real) {
				continue;
			}

			// ILT (names/ordinals) and IAT (resolved addresses) run in
			// parallel. Some linkers omit the ILT; fall back to matching the
			// IAT entry against the real export address.
			auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
			auto* ilt = desc->OriginalFirstThunk
			                ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk)
			                : nullptr;

			for (std::size_t n = 0; iat[n].u1.Function != 0; ++n) {
				bool isTarget = false;
				if (ilt && ilt[n].u1.AddressOfData != 0) {
					if (ilt[n].u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
						// Ordinal import — Fallout4.exe imports xinput1_3
						// exclusively this way (ordinals 4, 2, 3; GetState
						// is ordinal 2). Match against the ordinal resolved
						// from the DLL's own export directory. Address
						// matching does NOT work here whenever another
						// plugin (F4SE Menu Framework 3) patched the slot
						// first — that was exactly the bug that let the
						// workbench menu keep moving under our picker.
						const WORD ord = static_cast<WORD>(IMAGE_ORDINAL64(ilt[n].u1.Ordinal));
						isTarget = (g_xinputDlls[dllIdx].getStateOrdinal != 0 &&
									ord == g_xinputDlls[dllIdx].getStateOrdinal);
					} else {
						auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + ilt[n].u1.AddressOfData);
						isTarget = (std::strcmp(byName->Name, "XInputGetState") == 0);
					}
				} else {
					// Missing ILT — last resort: match by resolved address
					// (only works if nothing else has patched the slot yet).
					isTarget = (iat[n].u1.Function ==
								reinterpret_cast<ULONGLONG>(g_xinputDlls[dllIdx].real));
				}
				if (!isTarget) {
					continue;
				}
				// Already ours (re-run of the install pass).
				if (iat[n].u1.Function == reinterpret_cast<ULONGLONG>(g_hookFns[dllIdx])) {
					continue;
				}

				// Chain capture: if this IAT slot already points at some OTHER
				// hook (F4SE Menu Framework 3 patches XInputGetState the same
				// way), forward to that hook rather than the raw export so
				// both plugins' suppression stays live. Prefer a hooked value
				// over the plain export once one is seen.
				const auto current = reinterpret_cast<XInputGetStateFn>(iat[n].u1.Function);
				if (current != g_xinputDlls[dllIdx].real) {
					g_xinputDlls[dllIdx].chain = current;
				} else if (!g_xinputDlls[dllIdx].chain) {
					g_xinputDlls[dllIdx].chain = g_xinputDlls[dllIdx].real;
				}

				DWORD oldProtect;
				if (!::VirtualProtect(&iat[n], sizeof(IMAGE_THUNK_DATA64), PAGE_READWRITE, &oldProtect)) {
					continue;
				}
				iat[n].u1.Function = reinterpret_cast<ULONGLONG>(g_hookFns[dllIdx]);
				::VirtualProtect(&iat[n], sizeof(IMAGE_THUNK_DATA64), oldProtect, &oldProtect);
				++patched;

				logger::info("[BWS] gamepad IAT hook: {} imports {} — XInputGetState patched"sv,
					std::filesystem::path(a_moduleName).filename().string(), dllName);
			}
		}
		return patched;
	}
}

namespace BWS::GamepadInput
{
	// TIMING: must run AFTER every other plugin's XInput IAT pass. F4SE Menu
	// Framework 3 patches at its F4SEPlugin_Load and again at kGameDataReady,
	// and it forwards to the raw export — so if it patched after us, our hook
	// would be cut out of the call path entirely. We therefore install from
	// the first HUD render frame (TryInitImGuiFromRenderer), which is after
	// all plugins' kGameDataReady handlers have completed; we capture the
	// then-current IAT value as our forward target, so MF3 stays chained.
	void Install()
	{
		// Resolve the real export for every loaded XInput variant. If Steam
		// has inline-hooked the export, calling it runs Steam's chain —
		// exactly what we want (translated PlayStation-pad data included).
		for (auto& slot : g_xinputDlls) {
			if (!slot.module) {
				slot.module = ::GetModuleHandleW(slot.name);
				if (slot.module) {
					slot.real = reinterpret_cast<XInputGetStateFn>(
						::GetProcAddress(slot.module, "XInputGetState"));
					// Ordinal for matching ordinal-only imports (Fallout4.exe).
					slot.getStateOrdinal = FindExportOrdinal(slot.module, "XInputGetState");
				}
			}
		}

		bool anyLoaded = false;
		for (const auto& slot : g_xinputDlls) {
			if (slot.real) {
				anyLoaded = true;
			}
		}
		if (!anyLoaded) {
			logger::warn("[BWS] no XInput DLL loaded — gamepad suppression unavailable"sv);
			return;
		}

		// Never patch our own module (ImGui's backend must always read the
		// real pad; it resolves dynamically anyway, but belt-and-braces).
		HMODULE selfModule = nullptr;
		::GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&Install), &selfModule);

		int    totalPatched = 0;
		HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
		if (snapshot != INVALID_HANDLE_VALUE) {
			MODULEENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			if (::Module32FirstW(snapshot, &entry)) {
				do {
					if (entry.hModule == selfModule) {
						continue;
					}
					// Don't patch the XInput DLLs themselves (forwarder stubs).
					bool isXInputDll = false;
					for (const auto& slot : g_xinputDlls) {
						if (slot.module == entry.hModule) {
							isXInputDll = true;
							break;
						}
					}
					if (isXInputDll) {
						continue;
					}
					totalPatched += PatchModuleIAT(entry.hModule, entry.szModule);
				} while (::Module32NextW(snapshot, &entry));
			}
			::CloseHandle(snapshot);
		}

		logger::info("[BWS] gamepad XInput IAT hook pass complete — {} import entries patched"sv, totalPatched);
		spdlog::default_logger()->flush();
	}

	bool IsSuppressing()
	{
		return g_suppressing.load(std::memory_order_relaxed);
	}
}
