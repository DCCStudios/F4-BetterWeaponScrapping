#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RE/Bethesda/BGSInventoryItem.h"

namespace RE
{
	class ExamineMenu;
}

// Snapshot by FormID: raw TESForm / Mod* from ExamineMenu can dangle after the scrap callback returns.
struct PendingModPick
{
	std::uint32_t formID{ 0 };
	std::string   label;
};

struct RecipeMaterialLine
{
	std::uint32_t ingredientFormID{ 0 };
	std::uint32_t count{ 0 };
	std::string   displayName;
};

struct PendingWeaponScrap
{
	std::string                    weaponDisplayName;
	std::string                    matchedCobjEditorID;
	std::vector<PendingModPick>    mods;
	std::vector<RecipeMaterialLine> recipeMaterials;
	// True when this snapshot was captured BEFORE the vanilla scrap ran (the
	// SWF-injected ScrapItem wrapper intercepted the request). The picker then
	// stages selections into the native scrappingArray and re-invokes the
	// vanilla scrap, instead of granting items itself afterwards.
	bool preScrap{ false };
};

// One item the native scrap pipeline should grant: staged by the pre-scrap
// picker, appended to ExamineMenu::scrappingArray by the
// BuildWeaponScrappingArray detour, displayed by the vanilla confirm dialog,
// and granted by the vanilla ScrapItemCallback::OnAccept.
struct RecoveryGrantItem
{
	std::uint32_t formID{ 0 };
	std::uint32_t count{ 0 };
};

// Layout from CommonLibF4 fork IMenu.h (ScrapItemCallback / ExamineMenu offsets).
// Used only for workbench ExamineMenu scrap confirmation.
struct ScrapItemCallbackLayout
{
	void*              vtbl{ nullptr };
	RE::ExamineMenu*   examineMenu{ nullptr };
	std::uint32_t      itemIndex{ 0 };
	std::uint32_t      pad{ 0 };
};
static_assert(sizeof(ScrapItemCallbackLayout) == 0x18);

// offsetof(RE::ExamineMenu, moddedInventoryItem) == 0x548 (PostNG / current CommonLibF4 layout).
inline constexpr std::uintptr_t kExamineMenu_ModdedInventoryItem = 0x548;

// offsetof(RE::ExamineMenu, scrappingArray) == 0x420 — from the same verified
// layout (PluginTemplate/GunMover/lib/commonlibf4/include/RE/E/ExamineMenu.h)
// that gives moddedInventoryItem == 0x548, which is confirmed working on this
// runtime. BSTArray<BSTTuple<TESBoundObject*, uint32_t>> of (component, count)
// pairs granted by the vanilla scrap-accept path.
inline constexpr std::uintptr_t kExamineMenu_ScrappingArray = 0x420;
