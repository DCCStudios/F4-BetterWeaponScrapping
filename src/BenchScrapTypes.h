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
