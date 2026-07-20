#pragma once

#include "PCH.h"

#include "BenchScrapTypes.h"

#include <vector>

namespace BWS::ExamineMenuBridge
{
	/**
	 * Installs the workbench (ExamineMenu) Scaleform integration:
	 *  - F4SE Scaleform callback that, when Interface/ExamineMenu.swf loads,
	 *    registers the native code object `root.bws`, pre-creates a
	 *    Shared.AS3.BSButtonHintData as `root.bws_hintData`, and injects
	 *    BWSExamineMenu.swf via a flash.display.Loader (same pattern as the
	 *    F4SE Menu Framework pause-menu button).
	 *  - DetourXS hook on ExamineMenu::BuildWeaponScrappingArray that appends
	 *    staged recovery items to the native scrappingArray, so the vanilla
	 *    confirm dialog lists them and the vanilla accept path grants them.
	 *
	 * Call from F4SEPlugin_Load (after F4SE::Init).
	 */
	void Install();

	/**
	 * Stages recovery items (loose mods / COBJ components) chosen in the
	 * pre-scrap picker. Consumed (one-shot) by the BuildWeaponScrappingArray
	 * detour on the next vanilla scrap.
	 */
	void StageRecoveryItems(std::vector<RecoveryGrantItem> a_items);

	/** Discards staged items and the in-flight flag (confirm cancelled / menu closed). */
	void ClearStagedItems();

	/**
	 * True between "picker re-invoked the vanilla scrap" and "confirm menu
	 * closed". While set, the ScrapItemCallback::OnAccept hook must NOT queue
	 * the legacy post-scrap picker (the native pipeline already granted
	 * everything) and OnScrapRequested must not re-intercept.
	 */
	bool IsNativeGrantScrapInFlight();

	/**
	 * Re-invokes the vanilla scrap on the UI thread by calling the original
	 * BGSCodeObj.ScrapItem the SWF wrapper saved as `bws_origScrapItem`.
	 * Marks the native-grant flow as in flight.
	 */
	void InvokeVanillaScrap();
}
