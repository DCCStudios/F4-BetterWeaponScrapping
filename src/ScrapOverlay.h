#pragma once

#include "PCH.h"

#include "BenchScrapTypes.h"

namespace ScrapOverlay
{
	void Install();
	void QueuePending(PendingWeaponScrap a_pending);
	/** Dismiss the recovery picker if open and release input/pause guards. */
	void ForceDismiss();
	/** True while the ImGui recovery picker is on screen (gamepad suppression). */
	bool IsPopupVisible();
}
