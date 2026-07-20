#pragma once

#include "PCH.h"

#include <atomic>

namespace BWS::ScrapModManager
{
	void Install();

	/** Called from HUDMenu PostDisplay after ImGui NewFrame; draws hotkey hint and scrap-mod flow. */
	void Draw();

	/** True while scrap-mod ImGui flow is active (blocks game input like recovery overlay). */
	bool BlocksGameInput();

	/** If ExamineMenu is open and hotkey matches, opens the scrap-mod picker. Returns true if consumed. */
	bool TryHotkey(std::uintptr_t a_vk);

	/** Immediately closes the scrap-mod flow, releasing input and menuMode. */
	void ForceClose();
}
