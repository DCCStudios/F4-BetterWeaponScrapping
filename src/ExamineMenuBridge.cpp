#include "PCH.h"
#include "ExamineMenuBridge.h"

#include "BenchScrapTypes.h"
#include "ScrapManager.h"
#include "ScrapModManager.h"
#include "ScrapOverlay.h"
#include "Settings.h"

#include "RE/Bethesda/BSFixedString.h"
#include "RE/Bethesda/BSTArray.h"
#include "RE/Bethesda/BSTTuple.h"
#include "RE/Bethesda/TESBoundObjects.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/UI.h"
#include "RE/Scaleform/GFx/GFx_ASMovieRootBase.h"
#include "RE/Scaleform/GFx/GFx_Player.h"

#include "F4SE/API.h"
#include "F4SE/Interfaces.h"
#include "REL/Relocation.h"

// DetourXS: function-entry detour with prologue relocation (LDE64 length
// disassembler). Used because CommonLibF4's trampoline only patches existing
// call/jmp sites; BuildWeaponScrappingArray needs a "call original, then
// modify the result" entry hook, exactly as F76 Overhaul does.
#pragma warning(push, 0)
#include "../DetourXS/detourxs.h"
#pragma warning(pop)

#include <cctype>
#include <mutex>
#include <string>
#include <vector>

// Workbench ExamineMenu Scaleform bridge.
//
// Mirrors the F4SE Menu Framework pause-menu injection (F4SE-Menu-Framework-3/
// src/PauseMenuButton.cpp), applied to the weapon workbench:
//
//   1. F4SE Scaleform callback fires for Interface/ExamineMenu.swf.
//   2. We register `root.bws` (native code object) and load
//      swf/BWSExamineMenu.swf into the menu with a flash.display.Loader.
//   3. The child SWF constructs a host-domain BSButtonHintData, in-place
//      pushes it onto ExamineMenu's live hint vectors via a SetButtonHintData
//      wrapper, and wraps BGSCodeObj.ScrapItem so `bws.OnScrapRequested()`
//      runs BEFORE the vanilla scrap builds its confirm dialog.
//   4. When the player confirms the pre-scrap picker, the chosen recovery
//      items are staged here and the vanilla scrap is re-invoked through the
//      saved original (`bws_origScrapItem`). Our DetourXS hook on
//      ExamineMenu::BuildWeaponScrappingArray then appends the staged items
//      to the menu's scrappingArray, so the native confirm dialog LISTS them
//      and the native accept path GRANTS them — no AddObjectToContainer.

namespace
{
	using RE::Scaleform::GFx::FunctionHandler;
	using RE::Scaleform::GFx::Movie;
	using RE::Scaleform::GFx::Value;

	// Child SWF we ship in Data/Interface/ (bare name: the engine's Scaleform
	// file opener resolves it against Interface/, including BA2 archives).
	constexpr auto kSWFName = "BWSExamineMenu.swf";

	// Host movie. Exact match against root.loaderInfo.url, like MCM and the
	// F4SE Menu Framework do (ExamineConfirmMenu.swf is a different URL and
	// never matches).
	constexpr auto kHostSWF = "Interface/ExamineMenu.swf";

	// ---------------------------------------------------------------- staged
	// Items chosen in the pre-scrap picker, waiting for the vanilla scrap to
	// rebuild the scrappingArray. One-shot: consumed by the detour below.
	std::mutex                     g_stagedMtx;
	std::vector<RecoveryGrantItem> g_stagedItems;

	// True from InvokeVanillaScrap() until the confirm menu closes (cleared by
	// ScrapModManager's menu sink via ClearStagedItems, or consumed+cleared on
	// ExamineMenu close). Guards both re-interception and the legacy
	// post-scrap picker queue.
	std::atomic<bool> g_nativeGrantInFlight{ false };

	// ------------------------------------------------------- helper: PC key
	// Human-readable PC key label for the current hotkey VK, matching what the
	// old ImGui hint displayed ("G", "VK 0x70", ...). Vanilla hints use plain
	// strings like "E" / "Wheel up", so single characters render natively.
	std::string HotkeyDisplayName()
	{
		const int vk = BWS::Settings::Get().scrapModHotkey.load();
		if (vk > 0 && vk < 256) {
			if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
				return std::string(1, static_cast<char>(vk));
			}
			return std::format("VK 0x{:X}", vk);
		}
		return "?";
	}

	// --------------------------------------------- native funcs for the SWF
	// All of these run on the Scaleform/UI thread during movie advance. They
	// only read atomics/settings or enqueue work — no heavy lifting here.

	// bws.Log(message) — lets the injected ActionScript report its progress
	// into our plugin log (there is no other AS3 debug channel in-game).
	class LogFunc : public FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			if (a_params.argCount >= 1 && a_params.args[0].IsString()) {
				logger::info("[BWS] SWF: {}"sv, a_params.args[0].GetString());
			}
		}
	};

	// bws.IsHintVisible() — polled every frame by the SWF to drive the
	// ButtonVisible property of our bar hint.
	class IsHintVisibleFunc : public FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			if (a_params.retVal) {
				*a_params.retVal = BWS::ScrapModManager::ShouldShowNativeHint();
			}
		}
	};

	// bws.GetHintKey() — PC key label for the hint (settings can change it at
	// runtime through the Menu Framework panel, so the SWF re-polls).
	class GetHintKeyFunc : public FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			if (a_params.retVal && a_params.movie && a_params.movie->asMovieRoot) {
				a_params.movie->asMovieRoot->CreateString(a_params.retVal, HotkeyDisplayName().c_str());
			}
		}
	};

	// bws.GetConfigFlags() — feature bitmask polled once by the SWF at
	// inject time, driving the intra-injection bisect of the broken-exit
	// bug without SWF rebuilds:
	//   bit 0 (1) = wrap BGSCodeObj.ScrapItem
	//   bit 1 (2) = attach the SCRAP MODS cue sprite
	class GetConfigFlagsFunc : public FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			if (a_params.retVal) {
				std::uint32_t flags = 0;
				if (BWS::Settings::Get().wrapScrapItemEnabled.load()) {
					flags |= 1u;
				}
				if (BWS::Settings::Get().scrapCueEnabled.load()) {
					flags |= 2u;
				}
				*a_params.retVal = flags;
			}
		}
	};

	// bws.OpenScrapMods() — click handler for the button-bar hint. Same flow
	// as the keyboard hotkey. Deferred to the game task queue so the picker
	// state flips outside Scaleform's advance.
	class OpenScrapModsFunc : public FunctionHandler
	{
	public:
		void Call(const Params&) override
		{
			logger::info("[BWS] SWF: SCRAP MODS hint activated"sv);
			if (const auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask([]() { BWS::ScrapModManager::OpenPickerFromExternal(); });
			} else {
				BWS::ScrapModManager::OpenPickerFromExternal();
			}
		}
	};

	// bws.OnScrapRequested() — the pre-scrap intercept. Called by the SWF's
	// ScrapItem wrapper when the player presses SCRAP on a weapon.
	// Returns true  -> we suppressed the vanilla scrap and opened the picker;
	//                  the picker later re-invokes bws_origScrapItem.
	// Returns false -> the SWF proceeds with the vanilla scrap untouched.
	class OnScrapRequestedFunc : public FunctionHandler
	{
	public:
		void Call(const Params& a_params) override
		{
			bool handled = false;

			const auto& s = BWS::Settings::Get();
			const bool  eligible =
				s.masterEnabled.load() &&
				s.useNativeGrant.load() &&
				!s.nativeUIOnly.load() &&
				!g_nativeGrantInFlight.load(std::memory_order_acquire);

			if (eligible) {
				RE::ExamineMenu* menu = nullptr;
				if (auto* ui = RE::UI::GetSingleton()) {
					static const RE::BSFixedString kExamine{ "ExamineMenu" };
					if (const auto m = ui->GetMenu(kExamine)) {
						menu = reinterpret_cast<RE::ExamineMenu*>(m.get());
					}
				}

				PendingWeaponScrap pending{};
				if (menu && ScrapManager::BuildPendingFromExamineMenu(menu, pending) &&
					(!pending.mods.empty() || !pending.recipeMaterials.empty())) {
					// Something is recoverable: show the picker first. The
					// overlay queue is mutex-protected and drained on the
					// render thread (HUDMenu::PostDisplay).
					pending.preScrap = true;
					if (BWS::Settings::Get().debugLogging.load()) {
						logger::info(
							"[BWS] pre-scrap intercept: '{}' ({} mods, {} recipe lines) — opening picker"sv,
							pending.weaponDisplayName, pending.mods.size(), pending.recipeMaterials.size());
					}
					ScrapOverlay::QueuePending(std::move(pending));
					handled = true;
				}
			}

			if (a_params.retVal) {
				*a_params.retVal = handled;
			}
		}
	};

	// ------------------------------------------------- Scaleform injection
	// F4SE invokes this for every movie the game loads; act only on the
	// workbench menu movie.
	bool ScaleformCallback(Movie* a_view, Value* /*a_f4seRoot*/)
	{
		if (!a_view || !a_view->asMovieRoot) {
			return true;
		}
		auto* root = a_view->asMovieRoot.get();

		Value url;
		if (!root->GetVariable(std::addressof(url), "root.loaderInfo.url") || !url.IsString()) {
			return true;
		}
		if (url.GetString() != std::string_view{ kHostSWF }) {
			return true;
		}

		// Bisect switch (EnableExamineMenuInjection=0): skip ALL Scaleform
		// work so a single in-game test can tell SWF-injection bugs apart
		// from native-hook bugs.
		if (!BWS::Settings::Get().swfInjectionEnabled.load()) {
			logger::warn("[BWS] ExamineMenuBridge: injection DISABLED via EnableExamineMenuInjection=0 (bisect mode)"sv);
			return true;
		}

		logger::info("[BWS] ExamineMenuBridge: injecting into {}"sv, kHostSWF);

		Value rootObj;
		if (!root->GetVariable(std::addressof(rootObj), "root")) {
			logger::warn("[BWS] ExamineMenuBridge: could not resolve 'root'"sv);
			return true;
		}

		// Native code object: root.bws.{OnScrapRequested, OpenScrapMods,
		// IsHintVisible, GetHintKey, Log}. Each function gets its own fresh
		// Value — CreateFunction overwrites the destination without releasing
		// a previously held managed reference.
		Value codeObj;
		root->CreateObject(std::addressof(codeObj));

		const auto registerFn = [&](const char* a_name, FunctionHandler* a_handler) {
			Value fn;
			root->CreateFunction(std::addressof(fn), a_handler);
			codeObj.SetMember(a_name, fn);
		};
		registerFn("OnScrapRequested", new OnScrapRequestedFunc());
		registerFn("OpenScrapMods", new OpenScrapModsFunc());
		registerFn("IsHintVisible", new IsHintVisibleFunc());
		registerFn("GetHintKey", new GetHintKeyFunc());
		registerFn("GetConfigFlags", new GetConfigFlagsFunc());
		registerFn("Log", new LogFunc());

		rootObj.SetMember("bws", codeObj);

		// BSButtonHintData is constructed in BWSExamineMenu.as via
		// getDefinitionByName("Shared.AS3.BSButtonHintData") in the host
		// application domain. C++ CreateObject previously produced an object
		// that failed typed Vector.<BSButtonHintData>.push, so AS3 owns
		// construction exclusively.

		// Loader + URLRequest: load our SWF and parent it under the menu root
		// (mirrors PauseMenuButton.cpp / MCM's "mcm_loader" sequence).
		Value loader;
		root->CreateObject(std::addressof(loader), "flash.display.Loader");

		Value urlName;
		root->CreateString(std::addressof(urlName), kSWFName);

		Value urlRequest;
		root->CreateObject(std::addressof(urlRequest), "flash.net.URLRequest", std::addressof(urlName), 1);

		rootObj.SetMember("bws_loader", loader);

		if (!root->Invoke("root.bws_loader.load", nullptr, std::addressof(urlRequest), 1)) {
			logger::warn("[BWS] ExamineMenuBridge: bws_loader.load failed (is {} in Data/Interface?)"sv, kSWFName);
		}
		if (!root->Invoke("root.addChild", nullptr, std::addressof(loader), 1)) {
			logger::warn("[BWS] ExamineMenuBridge: root.addChild failed"sv);
		}

		logger::info("[BWS] ExamineMenuBridge: injection complete"sv);
		return true;
	}

	// ------------------------------------- BuildWeaponScrappingArray detour
	// void ExamineMenu::BuildWeaponScrappingArray()
	// AddressLibrary ID 646841 (runtime 1.10.163 line; NG line would be
	// 2223077) — from PluginTemplate/GunMover commonlibf4 RE::ID::ExamineMenu.
	// The vanilla ScrapItem handler calls this to fill scrappingArray, then
	// copies the array into ExamineConfirmMenu::InitDataScrap for display,
	// and the same array drives what the accept path grants.
	using BuildScrapArrayFn = void (*)(RE::ExamineMenu*);

	DetourXS           g_buildScrapDetour;
	BuildScrapArrayFn  g_buildScrapOriginal{ nullptr };

	void HookedBuildWeaponScrappingArray(RE::ExamineMenu* a_menu)
	{
		g_buildScrapOriginal(a_menu);

		if (!a_menu) {
			return;
		}

		// Take (and consume) staged items. One-shot: if the confirm dialog is
		// cancelled, the array is discarded with it and nothing is granted;
		// pressing SCRAP again re-runs the picker and re-stages.
		std::vector<RecoveryGrantItem> staged;
		{
			std::lock_guard lk(g_stagedMtx);
			staged = std::move(g_stagedItems);
			g_stagedItems.clear();
		}
		if (staged.empty()) {
			return;
		}

		using ScrapArray = RE::BSTArray<RE::BSTTuple<RE::TESBoundObject*, std::uint32_t>>;
		auto* arr = reinterpret_cast<ScrapArray*>(
			reinterpret_cast<std::byte*>(a_menu) + kExamineMenu_ScrappingArray);

		const bool dbg = BWS::Settings::Get().debugLogging.load();
		std::uint32_t appended = 0;

		for (const auto& item : staged) {
			auto* bound = RE::TESForm::GetFormByID<RE::TESBoundObject>(item.formID);
			if (!bound || item.count == 0) {
				if (dbg) {
					logger::info("[BWS] scrappingArray: skipping {:08X} x{} (unresolved / zero)"sv,
						item.formID, item.count);
				}
				continue;
			}
			// Merge with an existing line for the same object so the confirm
			// dialog shows one row per component, like vanilla.
			bool merged = false;
			for (auto& tup : *arr) {
				if (tup.first == bound) {
					tup.second += item.count;
					merged = true;
					break;
				}
			}
			if (!merged) {
				arr->push_back(RE::BSTTuple<RE::TESBoundObject*, std::uint32_t>(bound, item.count));
			}
			++appended;
			if (dbg) {
				logger::info("[BWS] scrappingArray: +{}x {:08X}{}"sv,
					item.count, item.formID, merged ? " (merged)" : "");
			}
		}

		logger::info("[BWS] BuildWeaponScrappingArray: appended {} staged recovery item(s), array size now {}"sv,
			appended, arr->size());
	}
}

namespace BWS::ExamineMenuBridge
{
	void Install()
	{
		// Scaleform callback — drives the SWF injection.
		if (const auto* scaleform = F4SE::GetScaleformInterface();
			scaleform && scaleform->Register("bws", ScaleformCallback)) {
			logger::info("[BWS] ExamineMenuBridge: Scaleform callback registered (SWF: {})"sv, kSWFName);
		} else {
			logger::warn("[BWS] ExamineMenuBridge: Scaleform registration failed; native workbench UI disabled"sv);
		}

		// BuildWeaponScrappingArray detour — drives native component granting.
		try {
			REL::Relocation<std::uintptr_t> target{ REL::ID(646841) };
			if (g_buildScrapDetour.Create(
					reinterpret_cast<LPVOID>(target.address()),
					reinterpret_cast<LPVOID>(&HookedBuildWeaponScrappingArray))) {
				g_buildScrapOriginal = reinterpret_cast<BuildScrapArrayFn>(g_buildScrapDetour.GetTrampoline());
				logger::info("[BWS] ExamineMenuBridge: BuildWeaponScrappingArray detour installed @ {:X}"sv,
					target.address());
			} else {
				logger::error("[BWS] ExamineMenuBridge: BuildWeaponScrappingArray detour FAILED; falling back to legacy grant"sv);
				BWS::Settings::Get().useNativeGrant.store(false);
			}
		} catch (const std::exception& e) {
			logger::error("[BWS] ExamineMenuBridge: detour target resolve failed: {}"sv, e.what());
			BWS::Settings::Get().useNativeGrant.store(false);
		}
		spdlog::default_logger()->flush();
	}

	void StageRecoveryItems(std::vector<RecoveryGrantItem> a_items)
	{
		std::lock_guard lk(g_stagedMtx);
		g_stagedItems = std::move(a_items);
	}

	void ClearStagedItems()
	{
		{
			std::lock_guard lk(g_stagedMtx);
			g_stagedItems.clear();
		}
		g_nativeGrantInFlight.store(false, std::memory_order_release);
	}

	bool IsNativeGrantScrapInFlight()
	{
		return g_nativeGrantInFlight.load(std::memory_order_acquire);
	}

	void InvokeVanillaScrap()
	{
		g_nativeGrantInFlight.store(true, std::memory_order_release);

		// bws_origScrapItem is the original native ScrapItem function the SWF
		// wrapper stashed on BGSCodeObj. Invoking it by path gives it the
		// same `this` (BGSCodeObj) as the vanilla call site. Deferred to the
		// UI task queue so the Scaleform invoke happens at a safe point.
		auto invoke = []() {
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			static const RE::BSFixedString kExamine{ "ExamineMenu" };
			const auto menu = ui->GetMenu(kExamine);
			if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot) {
				logger::warn("[BWS] InvokeVanillaScrap: ExamineMenu gone; aborting"sv);
				ClearStagedItems();
				return;
			}
			if (!menu->uiMovie->asMovieRoot->Invoke(
					"root.BaseInstance.BGSCodeObj.bws_origScrapItem", nullptr, nullptr, 0)) {
				logger::warn("[BWS] InvokeVanillaScrap: bws_origScrapItem invoke failed"sv);
				ClearStagedItems();
			}
		};

		if (const auto* tasks = F4SE::GetTaskInterface()) {
			tasks->AddUITask(invoke);
		} else {
			invoke();
		}
	}
}
