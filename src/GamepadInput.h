#pragma once

// Gamepad support for the BWS ImGui menus.
//
// Two jobs:
//  1. SUPPRESSION — while any BWS ImGui menu is open (recovery picker or the
//     scrap-mod flow), the game engine must not see gamepad input, otherwise
//     the ExamineMenu underneath keeps navigating / activating. This is done
//     with IAT hooks on XInputGetState across every loaded module and every
//     XInput DLL variant (xinput1_3 = the game's static import). The hook
//     zeroes buttons/sticks/triggers for callers while a menu is open.
//     ImGui itself is NOT affected: the Win32 backend resolves XInputGetState
//     at runtime via LoadLibrary + GetProcAddress, which bypasses import
//     tables entirely — so ImGui keeps reading the real pad and its built-in
//     gamepad navigation (ImGuiConfigFlags_NavEnableGamepad) works while the
//     game is blind. Pattern proven by F4SE Menu Framework 3 (GamepadInput.cpp).
//  2. OPEN BUTTON — while the workbench ExamineMenu is open and no BWS menu
//     is up, a rising edge on the configured pad button (INI
//     ScrapModGamepadButton, XINPUT_GAMEPAD_* mask, default 0x0040 = L3)
//     opens the scrap-mod picker, mirroring the keyboard hotkey. The press
//     is consumed (masked out of the state the game sees) until released.
namespace BWS::GamepadInput
{
	/**
	 * Installs the XInputGetState IAT hooks in all currently loaded modules.
	 * Idempotent — call again later (e.g. kGameDataReady) to cover modules
	 * loaded after plugin load.
	 */
	void Install();

	/** True while the hook is currently hiding gamepad input from the game. */
	bool IsSuppressing();
}
