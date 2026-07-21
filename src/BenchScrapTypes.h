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

// offsetof(RE::ExamineMenu, scrappingArray) == 0x428 on runtime 1.10.163.
//
// IMPORTANT: the CommonLibF4 header annotates this as 0x420, but that is WRONG
// for old-gen and was the cause of a hard CTD. Re-derived from the actual
// (Steamless-unpacked) Fallout4.exe by disassembling BuildWeaponScrappingArray:
//   - scrappableItemsMap is a BSTHashMap<u32,u32> at 0x3F8 (size 0x30 ->
//     ends 0x428). The build loop reads its _capacity at [this+0x404] and its
//     _allocator._entries at [this+0x420]; so 0x420 is INSIDE the hashmap.
//   - scrappingArray (BSTArray<BSTTuple<TESBoundObject*, uint32_t>>) is the
//     next member at 0x428: the function clears it via `lea rsi,[this+0x428];
//     mov dword [rsi+0x10], 0` (BSTArray _size at +0x10 -> 0x438) and it ends
//     at 0x440, exactly where the header places `slotObjects`.
// Writing to 0x420 (the old value) corrupted the hashmap's entry pointer, so
// the *next* BuildWeaponScrappingArray iterated garbage form IDs and
// GetFormByID returned null -> `mov edx,[rax+0x30]` on null -> crash.
inline constexpr std::uintptr_t kExamineMenu_ScrappingArray = 0x428;
