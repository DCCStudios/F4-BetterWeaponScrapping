#include "PCH.h"
#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace BWS
{
	namespace
	{
		constexpr std::string_view kIniName = "BetterWeaponScrappingF4SE.ini"sv;

		[[nodiscard]] std::optional<std::filesystem::path> PluginDllContainingFolder() noexcept
		{
			HMODULE module{};
			if (!::GetModuleHandleExW(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<const wchar_t*>(&PluginDllContainingFolder),
					&module)) {
				return std::nullopt;
			}
			std::wstring buf;
			buf.resize(MAX_PATH);
			const DWORD n = ::GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
			if (n == 0 || n >= buf.size()) {
				return std::nullopt;
			}
			buf.resize(n);
			std::filesystem::path p(buf);
			return p.parent_path();
		}
	}

	Settings& Settings::Get()
	{
		static Settings s;
		return s;
	}

	std::optional<std::filesystem::path> Settings::IniPath() const
	{
		const auto dir = PluginDllContainingFolder();
		if (!dir) {
			return std::nullopt;
		}
		return *dir / std::string{ kIniName };
	}

	std::string Settings::Trim(std::string_view a_sv)
	{
		while (!a_sv.empty() && (a_sv.front() == ' ' || a_sv.front() == '\t')) {
			a_sv.remove_prefix(1);
		}
		while (!a_sv.empty() && (a_sv.back() == ' ' || a_sv.back() == '\t' || a_sv.back() == '\r')) {
			a_sv.remove_suffix(1);
		}
		return std::string{ a_sv };
	}

	int Settings::ParseInt(std::string_view a_value, int a_default)
	{
		const auto t = Trim(a_value);
		if (t.empty()) {
			return a_default;
		}
		try {
			return std::stoi(t, nullptr, 0);
		} catch (...) {
			return a_default;
		}
	}

	float Settings::ParseFloat(std::string_view a_value, float a_default)
	{
		const auto t = Trim(a_value);
		if (t.empty()) {
			return a_default;
		}
		try {
			return std::stof(t);
		} catch (...) {
			return a_default;
		}
	}

	bool Settings::ParseBool(std::string_view a_value, bool a_default)
	{
		const auto t = Trim(a_value);
		if (t.empty()) {
			return a_default;
		}
		char c = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
		if (c == '1' || c == 'y' || c == 't') {
			return true;
		}
		if (c == '0' || c == 'n' || c == 'f') {
			return false;
		}
		if (t.size() >= 4 && _stricmp(t.c_str(), "true") == 0) {
			return true;
		}
		if (t.size() >= 5 && _stricmp(t.c_str(), "false") == 0) {
			return false;
		}
		return a_default;
	}

	void Settings::Load()
	{
		const auto path = IniPath();
		if (!path) {
			return;
		}
		if (!std::filesystem::exists(*path)) {
			Save();
			return;
		}
		std::ifstream f(*path);
		if (!f) {
			return;
		}

		std::string line;
		bool        inGeneral = false;
		while (std::getline(f, line)) {
			const auto view = std::string_view{ line };
			auto       trimmed = Trim(view);
			if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
				continue;
			}
			if (trimmed.front() == '[') {
				inGeneral = (_stricmp(trimmed.c_str(), "[General]") == 0);
				continue;
			}
			if (!inGeneral) {
				continue;
			}
			const auto eq = trimmed.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const auto key = Trim(std::string_view{ trimmed }.substr(0, eq));
			const auto val = Trim(std::string_view{ trimmed }.substr(eq + 1));

			if (key == "MasterEnabled") {
				masterEnabled.store(ParseBool(val, true));
			} else if (key == "PauseWhilePicker") {
				pauseWhilePicker.store(ParseBool(val, false));
			} else if (key == "BlockInputWhilePicker") {
				blockInputWhilePicker.store(ParseBool(val, true));
			} else if (key == "DefaultSelectAllMods") {
				defaultSelectAllMods.store(ParseBool(val, true));
			} else if (key == "DefaultSelectRecipeMaterials") {
				defaultSelectRecipeMaterials.store(ParseBool(val, false));
			} else if (key == "ShowApplyHudMessage") {
				showApplyHudMessage.store(ParseBool(val, true));
		} else if (key == "NativeUIOnly") {
			nativeUIOnly.store(ParseBool(val, false));
			} else if (key == "DebugLogging") {
				debugLogging.store(ParseBool(val, false));
			} else if (key == "EnableScrapMod") {
				enableScrapMod.store(ParseBool(val, true));
			} else if (key == "ScrapModHotkey") {
				scrapModHotkey.store(ParseInt(val, 0x47));
			} else if (key == "SolidBackground") {
				solidBackground.store(ParseBool(val, false));
			} else if (key == "HotkeyPromptX") {
				hotkeyPromptX.store(std::clamp(ParseFloat(val, 0.69f), 0.0f, 1.0f));
			} else if (key == "HotkeyPromptY") {
				hotkeyPromptY.store(std::clamp(ParseFloat(val, 0.963f), 0.0f, 1.0f));
			} else if (key == "HotkeyPromptScale") {
				hotkeyPromptScale.store(std::clamp(ParseFloat(val, 1.4f), 0.5f, 4.0f));
			} else if (key == "CustomFontPath") {
				SetCustomFontPath(val);
			}
		}
	}

	void Settings::Save()
	{
		const auto path = IniPath();
		if (!path) {
			return;
		}

		std::ostringstream oss;
		oss << "[General]\n";
		oss << "MasterEnabled=" << (masterEnabled.load() ? 1 : 0) << '\n';
		oss << "PauseWhilePicker=" << (pauseWhilePicker.load() ? 1 : 0) << '\n';
		oss << "BlockInputWhilePicker=" << (blockInputWhilePicker.load() ? 1 : 0) << '\n';
		oss << "DefaultSelectAllMods=" << (defaultSelectAllMods.load() ? 1 : 0) << '\n';
		oss << "DefaultSelectRecipeMaterials=" << (defaultSelectRecipeMaterials.load() ? 1 : 0) << '\n';
		oss << "ShowApplyHudMessage=" << (showApplyHudMessage.load() ? 1 : 0) << '\n';
		oss << "NativeUIOnly=" << (nativeUIOnly.load() ? 1 : 0) << '\n';
		oss << "DebugLogging=" << (debugLogging.load() ? 1 : 0) << '\n';
		oss << "EnableScrapMod=" << (enableScrapMod.load() ? 1 : 0) << '\n';
		oss << "ScrapModHotkey=" << scrapModHotkey.load() << '\n';
		oss << "SolidBackground=" << (solidBackground.load() ? 1 : 0) << '\n';
		oss << "HotkeyPromptX=" << hotkeyPromptX.load() << '\n';
		oss << "HotkeyPromptY=" << hotkeyPromptY.load() << '\n';
		oss << "HotkeyPromptScale=" << hotkeyPromptScale.load() << '\n';
		oss << "CustomFontPath=" << GetCustomFontPath() << '\n';

		std::ofstream f(*path, std::ios::trunc);
		if (f) {
			f << oss.str();
		}
	}
}
