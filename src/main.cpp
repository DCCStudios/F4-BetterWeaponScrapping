#include "PCH.h"
#include "BWSMenu.h"
#include "ScrapManager.h"
#include "ScrapOverlay.h"
#include "ScrapModManager.h"
#include "Settings.h"

#include <vector>

#pragma warning(push, 0)
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#pragma warning(pop)

namespace Plugin
{
	static constexpr auto NAME = "BetterWeaponScrappingF4SE"sv;
}

namespace
{
	void F4SEAPI OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		if (a_msg->type == F4SE::MessagingInterface::kPostLoad) {
			BWS::RegisterMenuFrameworkPages();
		}
		if (a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
			BWS::ScrapModManager::Install();
		}
	}
}

static void InitializeLogging()
{
	std::vector<spdlog::sink_ptr> sinks;

	if (const auto path = logger::log_directory()) {
		auto logPath = *path / std::format("{}.log"sv, Plugin::NAME);
		sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true));
	}
#ifdef _WIN32
	sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#endif
	if (sinks.empty()) {
		return;
	}

	auto log = std::make_shared<spdlog::logger>("global"s, sinks.begin(), sinks.end());

#ifdef NDEBUG
	log->set_level(spdlog::level::info);
#else
	log->set_level(spdlog::level::trace);
#endif
	// info must flush while the game is running; flush_on(warn) leaves the log file empty in practice
	log->flush_on(spdlog::level::info);
	spdlog::set_default_logger(std::move(log));
	spdlog::default_logger()->flush();
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name        = Plugin::NAME.data();
	a_info->version = 1;

	if (a_f4se->IsEditor()) {
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_162) {
		return false;
	}

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	InitializeLogging();

	logger::info("Better Weapon Scrapping: F4SEPlugin_Load (runtime {})"sv, a_f4se->RuntimeVersion().string());
	spdlog::default_logger()->flush();

	BWS::Settings::Get().Load();

	if (const auto* messaging = F4SE::GetMessagingInterface()) {
		if (!messaging->RegisterListener(OnF4SEMessage)) {
			logger::warn("Better Weapon Scrapping: failed to register F4SE messaging listener; Menu Framework UI may not appear."sv);
		}
	} else {
		logger::warn("Better Weapon Scrapping: no F4SE messaging interface; Menu Framework UI may not appear."sv);
	}

	F4SE::AllocTrampoline(512);

	ScrapOverlay::Install();
	spdlog::default_logger()->flush();

	if (!ScrapManager::Install()) {
		logger::error("Better Weapon Scrapping: workbench scrap hook failed; mod has no effect."sv);
	} else {
		logger::info(
			"Better Weapon Scrapping: after confirming scrap on a weapon at a workbench (Examine menu), "
			"choose which object mods to recover and whether to add matching COBJ ingredients. "
			"The world is paused while the picker is open."sv);
	}
	spdlog::default_logger()->flush();

	return true;
}
