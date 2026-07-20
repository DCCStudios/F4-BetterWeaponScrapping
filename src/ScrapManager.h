#pragma once

#include "BenchScrapTypes.h"

struct PendingWeaponScrap;

namespace RE
{
	class ExamineMenu;
}

namespace ScrapManager
{
	bool Install();

	/** Fill pending scrap snapshot from workbench ExamineMenu (weapon + mods + recipe). */
	bool BuildPendingFromExamineMenu(RE::ExamineMenu* a_menu, PendingWeaponScrap& a_out);
}
