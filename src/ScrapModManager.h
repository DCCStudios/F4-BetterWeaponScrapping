#pragma once

#include "PCH.h"

#include <atomic>
#include <optional>

namespace RE
{
	class TESBoundObject;
	class BGSObjectInstanceExtra;
}

namespace BWS::ScrapModManager
{
	void Install();

	/**
	 * Index of the stack, within the PLAYER's real BGSInventoryItem for
	 * a_base, whose BGSObjectInstanceExtra content (the OID mod array)
	 * exactly matches a_snapshot — the instance ExamineMenu is displaying.
	 * ExamineMenu::moddedInventoryItem is a snapshot and ExamineMenu::modStack
	 * proved unreliable as a plain stack index (detach wrote to the wrong
	 * weapon when two of the same base form were carried), so the stack is
	 * located by comparing actual instance data instead.
	 */
	std::optional<std::uint32_t> FindPlayerStackIndexForInstance(
		RE::TESBoundObject* a_base, const RE::BGSObjectInstanceExtra* a_snapshot);

	/** Called from HUDMenu PostDisplay after ImGui NewFrame; draws hotkey hint and scrap-mod flow. */
	void Draw();

	/** True while scrap-mod ImGui flow is active (blocks game input like recovery overlay). */
	bool BlocksGameInput();

	/** If ExamineMenu is open and hotkey matches, opens the scrap-mod picker. Returns true if consumed. */
	bool TryHotkey(std::uintptr_t a_vk);

	/** Immediately closes the scrap-mod flow, releasing input and menuMode. */
	void ForceClose();

	/** True while ExamineMenu is open (weapons/armor/cooking workbench). */
	bool IsExamineMenuOpen();

	/**
	 * Force-clear ControlMap ignore + any menuMode layers we added.
	 * Safe to call when leaving the workbench or recovering from a stranded UI.
	 */
	void ForceReleaseInputGuards();

	/**
	 * True when the native "SCRAP MODS" button-bar hint should be visible:
	 * feature enabled, ExamineMenu open on a modded weapon, no confirm dialog
	 * and no picker already active. Polled per-frame by the injected SWF
	 * (BWSExamineMenu.swf) via root.bws.IsHintVisible().
	 */
	bool ShouldShowNativeHint();

	/** Opens the scrap-mod picker (button-bar hint click / external callers). */
	void OpenPickerFromExternal();

	/**
	 * Per-frame heal after leaving ExamineMenu. Must NOT clear
	 * PlayerControls::blockPlayerInput inside the close event — that races
	 * ButtonBarMenu teardown and leaves activate/interaction prompts hidden
	 * for the rest of the session. Call from HUD PostDisplay instead.
	 */
	void TickPostExamineInputHeal();
}
