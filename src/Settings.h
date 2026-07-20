#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace BWS
{
	class Settings
	{
	public:
		static Settings& Get();

		void Load();
		void Save();

		[[nodiscard]] std::optional<std::filesystem::path> IniPath() const;

		std::atomic<bool> masterEnabled{ true };
		// Default off: freezeFramePause can leave the swap chain showing a black frame with a custom Present hook.
		std::atomic<bool> pauseWhilePicker{ false };
		std::atomic<bool> blockInputWhilePicker{ true };
		std::atomic<bool> defaultSelectAllMods{ true };
		std::atomic<bool> defaultSelectRecipeMaterials{ false };
		std::atomic<bool> showApplyHudMessage{ true };
		std::atomic<bool> nativeUIOnly{ false };
		std::atomic<bool> debugLogging{ false };

		/** Selective mod removal at workbench Examine menu (ImGui flow + hotkey). */
		std::atomic<bool> enableScrapMod{ true };
		/** Virtual-key code (e.g. 'G' = 0x47). See WinUser.h VK_* constants. */
		std::atomic<int> scrapModHotkey{ 0x47 };
		/** When true, recovery / scrap-mod ImGui windows use opaque HUD background color. */
		std::atomic<bool> solidBackground{ false };

		/** Hotkey prompt position as fraction of screen (0.0–1.0). */
		std::atomic<float> hotkeyPromptX{ 0.69f };
		std::atomic<float> hotkeyPromptY{ 0.963f };
		/** Font scale multiplier for the hotkey prompt (on top of resolution scaling). */
		std::atomic<float> hotkeyPromptScale{ 1.4f };

		/** Optional TTF font path for ImGui menus. Empty = use default fallback chain. Requires restart. */
		std::string GetCustomFontPath() const { std::lock_guard<std::mutex> lk(fontPathMtx_); return customFontPath_; }
		void        SetCustomFontPath(const std::string& a_path) { std::lock_guard<std::mutex> lk(fontPathMtx_); customFontPath_ = a_path; }

	private:
		Settings() = default;

		mutable std::mutex fontPathMtx_;
		std::string        customFontPath_;

		static bool        ParseBool(std::string_view a_value, bool a_default);
		static int         ParseInt(std::string_view a_value, int a_default);
		static float       ParseFloat(std::string_view a_value, float a_default);
		static std::string Trim(std::string_view a_sv);
	};
}
