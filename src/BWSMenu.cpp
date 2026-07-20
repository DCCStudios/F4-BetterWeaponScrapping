#include "PCH.h"

#include <atomic>

#pragma warning(push, 0)
#include "F4SEMenuFramework.h"
#pragma warning(pop)

#include "BWSMenu.h"
#include "Settings.h"

namespace
{
	using namespace ImGuiMCP;

	void __stdcall RenderSettings()
	{
		ImGuiMCP::SeparatorText("Better Weapon Scrapping");
		ImGuiMCP::TextWrapped(
			"Workbench Examine-menu scrap recovery: after you confirm scrapping a weapon, you can recover "
			"object mods as loose items and optionally add ingredients from the first matching constructible "
			"(COBJ) for that weapon. Requires F4SE Menu Framework for this settings panel; the scrap picker uses "
			"the plugin's own overlay.");
		ImGuiMCP::Spacing();

		auto& s = BWS::Settings::Get();

		bool master = s.masterEnabled.load();
		if (ImGuiMCP::Checkbox("Enable scrap recovery", &master)) {
			s.masterEnabled.store(master);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip(
				"When off, confirming a weapon scrap behaves like vanilla (no recovery picker and no extras).");
		}

		ImGuiMCP::Separator();
		ImGuiMCP::TextUnformatted("While the recovery window is open");
		bool pause = s.pauseWhilePicker.load();
		if (ImGuiMCP::Checkbox("Pause the game world", &pause)) {
			s.pauseWhilePicker.store(pause);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip(
				"Uses UI freeze-frame pause so time does not advance during the picker. "
				"Off by default: on some setups it can leave a stale frame behind the overlay.");
		}

		bool solidBg = s.solidBackground.load();
		if (ImGuiMCP::Checkbox("Solid popup backgrounds (recovery + scrap mod)", &solidBg)) {
			s.solidBackground.store(solidBg);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip(
				"When on, ImGui pickers use a fully opaque HUD background color. "
				"The bottom hotkey hint stays transparent.");
		}
		bool block = s.blockInputWhilePicker.load();
		if (ImGuiMCP::Checkbox("Block keyboard / mouse to the game", &block)) {
			s.blockInputWhilePicker.store(block);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip(
				"When on, movement and menu input go to the picker instead of the world. Turn off if you need "
				"to interact with the game behind the window (not recommended).");
		}

		ImGuiMCP::Separator();
		ImGuiMCP::TextUnformatted("Defaults when the picker opens");
		bool defMods = s.defaultSelectAllMods.load();
		if (ImGuiMCP::Checkbox("Pre-select all object mods", &defMods)) {
			s.defaultSelectAllMods.store(defMods);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip("If off, every mod checkbox starts unchecked.");
		}

		bool defRec = s.defaultSelectRecipeMaterials.load();
		if (ImGuiMCP::Checkbox("Pre-select recipe (COBJ) materials", &defRec)) {
			s.defaultSelectRecipeMaterials.store(defRec);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip(
				"When a COBJ match exists, this sets the initial state of the recipe-ingredients checkbox.");
		}

		ImGuiMCP::Separator();
		ImGuiMCP::TextUnformatted("UI mode");
		bool nativeGrant = s.useNativeGrant.load();
		if (ImGuiMCP::Checkbox("Native scrap pipeline (picker before confirm)", &nativeGrant)) {
			s.useNativeGrant.store(nativeGrant);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip(
				"When on, pressing Scrap on a weapon opens the recovery picker FIRST; your selections are added "
				"to the game's own scrap confirmation and granted by the game itself. "
				"When off, the picker appears after the vanilla scrap and items are added directly (legacy).");
		}
		bool native = s.nativeUIOnly.load();
		if (ImGuiMCP::Checkbox("Native UI Only", &native)) {
			s.nativeUIOnly.store(native);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip(
				"When on, uses only the game's built-in message boxes for scrap recovery. "
				"\"Select Per Mod\" walks through each mod one at a time with Keep / Scrap / Skip buttons. "
				"When off, uses the ImGui overlay with per-mod checkboxes.");
		}

		ImGuiMCP::Separator();
		ImGuiMCP::TextUnformatted("Scrap mod (workbench Examine menu)");
		bool scrapMod = s.enableScrapMod.load();
		if (ImGuiMCP::Checkbox("Enable selective mod scrapping (hotkey bar + picker)", &scrapMod)) {
			s.enableScrapMod.store(scrapMod);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip(
				"While examining a weapon at a workbench, press the hotkey to remove one object mod at a time "
				"and receive matching constructible (COBJ) components when available.");
		}

		int vkHot = s.scrapModHotkey.load();
		if (ImGuiMCP::InputInt("Scrap mod hotkey (virtual-key code)", &vkHot)) {
			if (vkHot < 1) {
				vkHot = 1;
			} else if (vkHot > 255) {
				vkHot = 255;
			}
			s.scrapModHotkey.store(vkHot);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip("Default 71 (0x47) = letter G. See WinUser.h VK_* constants.");
		}

		// The SCRAP MODS prompt now lives in the game's own workbench button
		// bar (injected SWF), so the old ImGui prompt position/scale sliders
		// are gone — the bar lays the hint out natively.

		ImGuiMCP::Spacing();
		ImGuiMCP::TextUnformatted("Custom font");

		static char fontPathBuf[512]{};
		static bool fontPathBufInit = false;
		if (!fontPathBufInit) {
			const auto fp = s.GetCustomFontPath();
			std::strncpy(fontPathBuf, fp.c_str(), sizeof(fontPathBuf) - 1);
			fontPathBufInit = true;
		}
		if (ImGuiMCP::InputText("Custom font path (TTF)", fontPathBuf, sizeof(fontPathBuf))) {
			s.SetCustomFontPath(fontPathBuf);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip(
				"Absolute or relative path to a .ttf file. Leave empty for default.\n"
				"Requires game restart to apply.\n"
				"Tip: use JPEXS FFDec to extract TTF from fonts_en.swf.");
		}

		ImGuiMCP::Separator();
		bool hud = s.showApplyHudMessage.load();
		if (ImGuiMCP::Checkbox("HUD message after applying recovery", &hud)) {
			s.showApplyHudMessage.store(hud);
			s.Save();
		}

		bool dbg = s.debugLogging.load();
		if (ImGuiMCP::Checkbox("Extra debug logging", &dbg)) {
			s.debugLogging.store(dbg);
			s.Save();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip("Writes additional lines to BetterWeaponScrappingF4SE.log when scrapping.");
		}

		ImGuiMCP::Separator();
		if (const auto p = s.IniPath()) {
			ImGuiMCP::TextWrapped("Settings file: %s", p->string().c_str());
		} else {
			ImGuiMCP::TextColored(ImGuiMCP::ImVec4{ 1.0f, 0.4f, 0.3f, 1.0f },
				"Could not resolve F4SE log folder; INI load/save may be unavailable.");
		}

		if (ImGuiMCP::Button("Reload settings from disk")) {
			BWS::Settings::Get().Load();
		}
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip("Re-read BetterWeaponScrappingF4SE.ini without restarting.");
		}
		ImGuiMCP::SameLine();
		if (ImGuiMCP::Button("Save settings to disk now")) {
			BWS::Settings::Get().Save();
		}
	}

	void __stdcall RenderAbout()
	{
		ImGuiMCP::SeparatorText("About");
		ImGuiMCP::BulletText("Plugin: BetterWeaponScrappingF4SE");
		ImGuiMCP::BulletText("Scrap flow: workbench Examine menu, confirm scrap on a weapon.");
		ImGuiMCP::BulletText("Menu toggle key: %s", F4SEMenuFramework::GetToggleKeyName());
		ImGuiMCP::Spacing();
		ImGuiMCP::TextWrapped(
			"Install F4SE Menu Framework 3 (F4SEMenuFramework.dll in Data/F4SE/Plugins) to use this panel. "
			"Other settings are read from BetterWeaponScrappingF4SE.ini next to this plugin DLL. "
			"The framework is not required for scrap recovery itself, only for in-game configuration.");
	}
}

namespace BWS
{
	namespace
	{
		std::atomic<bool> g_menuFrameworkRegistered{ false };
		std::atomic<bool> g_loggedMissingFramework{ false };
	}

	void RegisterMenuFrameworkPages()
	{
		if (g_menuFrameworkRegistered.load()) {
			return;
		}

		if (!F4SEMenuFramework::IsInstalled()) {
			if (!g_loggedMissingFramework.exchange(true)) {
				logger::warn(
					"Better Weapon Scrapping: F4SE Menu Framework not in process yet or not installed — "
					"settings panel will register on kPostLoad if F4SEMenuFramework.dll is present. "
					"Ensure F4SEMenuFramework.dll is under Data/F4SE/Plugins/. "
					"You can still edit BetterWeaponScrappingF4SE.ini next to this plugin DLL."sv);
			}
			return;
		}

		F4SEMenuFramework::SetSection("Better Weapon Scrapping");
		F4SEMenuFramework::AddSectionItem("Settings", RenderSettings);
		F4SEMenuFramework::AddSectionItem("About", RenderAbout);

		g_menuFrameworkRegistered.store(true);
		logger::info("Better Weapon Scrapping: registered with F4SE Menu Framework"sv);
		spdlog::default_logger()->flush();
	}
}
