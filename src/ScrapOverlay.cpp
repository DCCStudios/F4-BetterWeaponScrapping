#include "PCH.h"
#include "ScrapOverlay.h"
#include "ExamineMenuBridge.h"
#include "GamepadInput.h"
#include "ScrapModManager.h"
#include "Settings.h"
#include "BWS_RendererData.h"

#include "RE/Bethesda/BGSInventoryItem.h"
#include "RE/Bethesda/BGSMod.h"
#include "RE/Bethesda/BSTSmartPointer.h"
#include "RE/Bethesda/ControlMap.h"
#include "RE/Bethesda/IMenu.h"
#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/TESBoundObjects.h"
#include "RE/Bethesda/TESDataHandler.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/UI.h"
#include "RE/Bethesda/UIMessage.h"
#include "RE/Bethesda/UIMessageQueue.h"
#include "RE/Bethesda/SendHUDMessage.h"

#include "F4SE/API.h"
#include "F4SE/Interfaces.h"
#include "RE/Bethesda/Settings.h"
#include "RE/NetImmerse/NiColor.h"
#include "RE/VTABLE_IDs.h"
#include "REL/Relocation.h"

#pragma warning(push, 0)
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#pragma warning(pop)

#include <imgui_internal.h>

#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>

namespace RE
{
	class alignas(0x10) IMessageBoxCallback :
		public BSIntrusiveRefCounted
	{
	public:
		static constexpr auto RTTI{ RTTI::IMessageBoxCallback };
		static constexpr auto VTABLE{ VTABLE::IMessageBoxCallback };

		virtual ~IMessageBoxCallback() = default;
		virtual void operator()(std::uint8_t a_buttonIdx) = 0;
	};
	static_assert(sizeof(IMessageBoxCallback) == 0x10);

	class MessageMenuManager :
		public BSTSingletonSDM<MessageMenuManager>
	{
	public:
		[[nodiscard]] static MessageMenuManager* GetSingleton()
		{
			static REL::Relocation<MessageMenuManager**> singleton{ REL::ID(959572) };
			return *singleton;
		}

		void Create(
			const char*          a_headerText,
			const char*          a_bodyText,
			IMessageBoxCallback* a_callback,
			std::uint32_t        a_warningContext,
			const char*          a_button1Text = nullptr,
			const char*          a_button2Text = nullptr,
			const char*          a_button3Text = nullptr,
			const char*          a_button4Text = nullptr,
			bool                 a_ensureUnique = false)
		{
			using func_t = decltype(&MessageMenuManager::Create);
			static REL::Relocation<func_t> func{ REL::ID(89563) };
			return func(this, a_headerText, a_bodyText, a_callback, a_warningContext, a_button1Text, a_button2Text, a_button3Text, a_button4Text, a_ensureUnique);
		}
	};
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	RE::TESBoundObject* ResolveIngredientForAdd(std::uint32_t a_formID)
	{
		auto* form = RE::TESForm::GetFormByID(a_formID);
		if (!form) {
			return nullptr;
		}
		if (auto* comp = form->As<RE::BGSComponent>(); comp && comp->scrapItem) {
			return comp->scrapItem;
		}
		return form->As<RE::TESBoundObject>();
	}

	void RefreshExamineMenuModList()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return;
		}
		static const RE::BSFixedString kExamine{ "ExamineMenu" };
		auto menu = ui->GetMenu(kExamine);
		if (!menu) {
			return;
		}
		auto* raw = menu.get();
		auto* vtbl = *reinterpret_cast<void***>(raw);
		using VoidFn = void (*)(void*);

		// Rebuild the menu's cached crafting inventory FIRST. The workbench
		// requirement/component counts are NOT read live from the player —
		// they come from WorkbenchMenuBase::optimizedAutoBuildInv, a
		// BGSInventoryList snapshot built when the menu opens. Items we add
		// with AddObjectToContainer are invisible to the menu until this
		// snapshot is rebuilt, which is why counts previously only updated
		// after leaving and re-entering the workbench.
		// Old-gen (1.10.163) Address Library ID 769581 =
		// WorkbenchMenuBase::UpdateOptimizedAutoBuildInv, cross-checked
		// against commonlibf4-main's VariantID{ 769581, 2224955 }.
		{
			using UpdateInvFn = void (*)(void*);
			static REL::Relocation<UpdateInvFn> updateInv{ REL::ID(769581) };
			updateInv(raw);
		}

		constexpr std::size_t kVIdx_UpdateMenu = 0x15;
		reinterpret_cast<VoidFn>(vtbl[kVIdx_UpdateMenu])(raw);

		constexpr std::size_t kVIdx_UpdateModChoiceList = 0x3F;
		reinterpret_cast<VoidFn>(vtbl[kVIdx_UpdateModChoiceList])(raw);
	}

	struct DeferredItem
	{
		std::uint32_t formID;
		std::int32_t  count;
	};

	void DeferAddItemsToPlayer(std::shared_ptr<std::vector<DeferredItem>> a_items)
	{
		if (!a_items || a_items->empty()) {
			return;
		}
		const auto* taskIface = F4SE::GetTaskInterface();
		if (taskIface) {
			taskIface->AddTask([items = std::move(a_items)]() {
				auto* pl = RE::PlayerCharacter::GetSingleton();
				if (!pl) {
					return;
				}
				for (const auto& di : *items) {
					auto* bound = RE::TESForm::GetFormByID<RE::TESBoundObject>(di.formID);
					if (bound) {
						pl->AddObjectToContainer(bound, nullptr, di.count, nullptr,
							static_cast<RE::ITEM_REMOVE_REASON>(0));
					}
				}
				RefreshExamineMenuModList();
			});
		} else {
			auto* pl = RE::PlayerCharacter::GetSingleton();
			if (pl) {
				for (const auto& di : *a_items) {
					auto* bound = RE::TESForm::GetFormByID<RE::TESBoundObject>(di.formID);
					if (bound) {
						pl->AddObjectToContainer(bound, nullptr, di.count, nullptr,
							static_cast<RE::ITEM_REMOVE_REASON>(0));
					}
				}
			}
			RefreshExamineMenuModList();
		}
	}

	std::mutex                       g_queueMtx;
	std::deque<PendingWeaponScrap> g_pendingQueue;

	std::atomic<bool>    g_initialized{ false };
	ImGuiContext*        g_imguiCtx{ nullptr };
	ID3D11Device*        g_device{ nullptr };
	ID3D11DeviceContext* g_deviceCtx{ nullptr };
	HWND                 g_hwnd{ nullptr };

	std::atomic<bool>     g_popupVisible{ false };
	PendingWeaponScrap    g_active{};
	std::vector<std::uint8_t> g_modSelected;
	bool                  g_recipeSelected{ false };
	// True while the picker is running in pre-scrap mode: it was opened by the
	// SWF ScrapItem intercept BEFORE the vanilla scrap. Apply then stages the
	// selection into the native scrappingArray pipeline and re-invokes the
	// vanilla scrap, instead of adding items to the player afterwards.
	bool                  g_preScrapMode{ false };

	struct IngredientEntry
	{
		std::uint32_t formID{ 0 };
		std::string   name;
		std::uint32_t count{ 0 };
	};
	std::vector<std::vector<IngredientEntry>> g_modRecipes;

	using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
	using PostDisplayFn = void (*)(RE::IMenu*);

	ClipCursorFn    g_origClipCursor{ nullptr };
	WNDPROC         g_origWndProc{ nullptr };
	PostDisplayFn   g_origHudPostDisplay{ nullptr };

	std::uint32_t g_worldPauseLayers{ 0 };

	// True only while THIS plugin set ControlMap::ignoreKeyboardMouse.
	// Cleared on release so we never fight other mods' menus.
	bool g_bwsOwnsIgnoreKeyboard{ false };

	LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static void DismissPopup();

	static void PushWorldPause()
	{
		// Never add a menuMode layer while ExamineMenu is open — it already
		// paused the game. Extra layers were the prime suspect for the
		// post-workbench control lock (menuMode left > 0 after exit).
		if (BWS::ScrapModManager::IsExamineMenuOpen()) {
			return;
		}
		if (!BWS::Settings::Get().pauseWhilePicker.load()) {
			return;
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->menuMode += 1;
			++g_worldPauseLayers;
		}
	}

	static void PopWorldPause()
	{
		if (g_worldPauseLayers == 0) {
			return;
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			if (ui->menuMode > 0) {
				ui->menuMode -= 1;
			}
		}
		--g_worldPauseLayers;
	}

	// Ground-truth check: do not trust only our MenuOpenCloseEvent flag.
	static bool IsExamineMenuReallyOpen()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		static const RE::BSFixedString kExamine{ "ExamineMenu" };
		return ui->GetMenuOpen(kExamine);
	}

	static void ReleaseAllBwsInputState(const char* a_reason)
	{
		logger::warn("[BWS] ReleaseAllBwsInputState: {}"sv, a_reason);
		if (g_popupVisible.load()) {
			DismissPopup();
		}
		while (g_worldPauseLayers > 0) {
			PopWorldPause();
		}
		BWS::ScrapModManager::ForceClose();
		BWS::ScrapModManager::ForceReleaseInputGuards();
		if (auto* cm = RE::ControlMap::GetSingleton()) {
			cm->SetIgnoreKeyboardMouse(false);
		}
		g_bwsOwnsIgnoreKeyboard = false;
		spdlog::default_logger()->flush();
	}

	static void BlockGameInput(bool a_blocked)
	{
		if (a_blocked && !BWS::Settings::Get().blockInputWhilePicker.load()) {
			return;
		}
		if (auto* cm = RE::ControlMap::GetSingleton()) {
			cm->SetIgnoreKeyboardMouse(a_blocked);
			g_bwsOwnsIgnoreKeyboard = a_blocked;
		}
	}

	static void RefreshBwsImGuiStyle()
	{
		if (!g_initialized.load() || !g_imguiCtx) {
			return;
		}
		ImGui::SetCurrentContext(g_imguiCtx);

		static float s_cachedScale = 0.0f;
		const float  scale = ImGui::GetIO().DisplaySize.y / 1080.0f;
		const bool     resChanged = (std::abs(scale - s_cachedScale) > 0.02f);

		auto readHUDColorFromINI = []() -> RE::NiColor {
			auto* prefs = RE::INIPrefSettingCollection::GetSingleton();
			if (!prefs) {
				return RE::NiColor(0.07f, 1.0f, 0.08f);
			}
			float r = 18.0f, g = 255.0f, b = 21.0f;
			if (auto* s = prefs->GetSetting("iHUDColorR:Interface"sv)) { r = static_cast<float>(s->GetInt()); }
			if (auto* s = prefs->GetSetting("iHUDColorG:Interface"sv)) { g = static_cast<float>(s->GetInt()); }
			if (auto* s = prefs->GetSetting("iHUDColorB:Interface"sv)) { b = static_cast<float>(s->GetInt()); }
			return RE::NiColor(r / 255.0f, g / 255.0f, b / 255.0f);
		};
		const auto hudNi = readHUDColorFromINI();
		const auto bgNi = RE::NiColor(hudNi.r * 0.12f, hudNi.g * 0.12f, hudNi.b * 0.12f);

		static RE::NiColor lastHud{};
		static RE::NiColor lastBg{};
		static bool         lastSolid{ false };

		const bool solid = BWS::Settings::Get().solidBackground.load();
		const bool paletteChanged =
			(std::abs(hudNi.r - lastHud.r) > 1e-4f) || (std::abs(bgNi.r - lastBg.r) > 1e-4f) ||
			(solid != lastSolid) || resChanged;

		if (!paletteChanged && s_cachedScale != 0.0f) {
			return;
		}
		lastHud = hudNi;
		lastBg = bgNi;
		lastSolid = solid;
		s_cachedScale = scale;

		ImGuiStyle style{};
		ImGui::StyleColorsDark(&style);
		auto& colors = style.Colors;

		const ImVec4 hud = ImVec4(hudNi.r, hudNi.g, hudNi.b, 1.0f);
		const float  bgA = solid ? 1.0f : 0.85f;
		const ImVec4 bg = ImVec4(bgNi.r, bgNi.g, bgNi.b, bgA);

		style.WindowRounding = 0.0f;
		style.ChildRounding = 0.0f;
		style.FrameRounding = 0.0f;
		style.PopupRounding = 0.0f;
		style.ScrollbarRounding = 0.0f;
		style.GrabRounding = 0.0f;
		style.TabRounding = 0.0f;
		style.WindowBorderSize = 2.5f;
		style.FrameBorderSize = 1.0f;

		colors[ImGuiCol_Text] = hud;
		colors[ImGuiCol_TextDisabled] = ImVec4(hud.x * 0.5f, hud.y * 0.5f, hud.z * 0.5f, 0.7f);
		colors[ImGuiCol_WindowBg] = bg;
		colors[ImGuiCol_ChildBg] = bg;
		colors[ImGuiCol_Border] = hud;
		colors[ImGuiCol_Separator] = hud;
		colors[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0, 0, 0, 0);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0, 0, 0, 0);
		colors[ImGuiCol_CheckMark] = hud;
		colors[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
		colors[ImGuiCol_ButtonHovered] = ImVec4(hud.x, hud.y, hud.z, 0.25f);
		colors[ImGuiCol_ButtonActive] = ImVec4(hud.x, hud.y, hud.z, 0.35f);
		colors[ImGuiCol_ScrollbarGrab] = hud;
		colors[ImGuiCol_ScrollbarGrabHovered] = hud;
		colors[ImGuiCol_ScrollbarGrabActive] = hud;
		colors[ImGuiCol_Header] = ImVec4(hud.x, hud.y, hud.z, 0.35f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(hud.x, hud.y, hud.z, 0.5f);
		colors[ImGuiCol_HeaderActive] = ImVec4(hud.x, hud.y, hud.z, 0.55f);

		style.ScaleAllSizes(scale);
		ImGui::GetStyle() = style;
	}

	static void PrecomputeModRecipes(const PendingWeaponScrap& a_data);
	static void ShowNativeMessageBox(PendingWeaponScrap a_pending);

	static void ProcessPendingScrapQueue()
	{
		if (!g_initialized.load() || g_popupVisible.load()) {
			return;
		}
		std::lock_guard lk(g_queueMtx);
		if (g_pendingQueue.empty()) {
			return;
		}
		auto pending = std::move(g_pendingQueue.front());
		g_pendingQueue.pop_front();

		// Pre-scrap requests always use the ImGui picker: the SWF intercept
		// only fires when nativeUIOnly is off (see OnScrapRequestedFunc).
		if (BWS::Settings::Get().nativeUIOnly.load() && !pending.preScrap) {
			ShowNativeMessageBox(std::move(pending));
		} else {
			g_preScrapMode = pending.preScrap;
			g_active = std::move(pending);
			PrecomputeModRecipes(g_active);
			const auto defMod = BWS::Settings::Get().defaultSelectAllMods.load() ? 1 : 0;
			g_modSelected.assign(g_active.mods.size(), static_cast<std::uint8_t>(defMod));
			g_recipeSelected = BWS::Settings::Get().defaultSelectRecipeMaterials.load();
			g_popupVisible.store(true);
			BlockGameInput(true);
			PushWorldPause();
			::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
		}
	}

	static void DismissPopup()
	{
		g_popupVisible.store(false);
		g_active = {};
		g_modSelected.clear();
		g_modRecipes.clear();
		g_recipeSelected = false;
		g_preScrapMode = false;
		BlockGameInput(false);
		PopWorldPause();
		// Drain any leftover pause layers (open/close races) so leaving the
		// workbench cannot strand the player with menuMode > 0.
		while (g_worldPauseLayers > 0) {
			PopWorldPause();
		}
		// Always clear keyboard/mouse ignore when the overlay closes — even if
		// BlockInputWhilePicker is off — so a stuck ControlMap flag cannot
		// lock the player after exiting the workbench.
		if (auto* cm = RE::ControlMap::GetSingleton()) {
			cm->SetIgnoreKeyboardMouse(false);
		}
		g_bwsOwnsIgnoreKeyboard = false;
	}

	static RE::BGSConstructibleObject* FindConstructibleForForm(const RE::TESForm* a_createdItem)
	{
		if (!a_createdItem) {
			return nullptr;
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return nullptr;
		}
		auto& arr = dh->GetFormArray<RE::BGSConstructibleObject>();
		for (auto* cobj : arr) {
			if (cobj && cobj->GetCreatedItem() == a_createdItem) {
				return cobj;
			}
		}
		return nullptr;
	}

	static RE::BGSConstructibleObject* FindCobjForMod(
		const RE::BGSMod::Attachment::Mod* a_mod, const RE::TESObjectMISC* a_loose)
	{
		if (auto* c = FindConstructibleForForm(a_mod)) {
			return c;
		}
		if (a_loose) {
			return FindConstructibleForForm(a_loose);
		}
		return nullptr;
	}

	static void PrecomputeModRecipes(const PendingWeaponScrap& a_data)
	{
		g_modRecipes.clear();
		g_modRecipes.resize(a_data.mods.size());

		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();
		for (std::size_t i = 0; i < a_data.mods.size(); ++i) {
			auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_data.mods[i].formID);
			if (!mod) {
				continue;
			}
			RE::TESObjectMISC* looseItem = nullptr;
			if (auto it = loose.find(mod); it != loose.end()) {
				looseItem = it->second;
			}
			auto* cobj = FindCobjForMod(mod, looseItem);
			if (!cobj || !cobj->requiredItems) {
				continue;
			}
			for (const auto& tup : *cobj->requiredItems) {
				auto* form = tup.first;
				const auto qty = tup.second.i;
				if (!form || qty == 0) {
					continue;
				}
				std::string name;
				if (auto* full = form->As<RE::TESFullName>()) {
					if (auto* n = full->fullName.c_str(); n && n[0]) {
						name = n;
					}
				}
				if (name.empty()) {
					name = std::format("{:08X}", form->GetFormID());
				}
				g_modRecipes[i].push_back(IngredientEntry{
					.formID = static_cast<std::uint32_t>(form->GetFormID()),
					.name   = std::move(name),
					.count  = static_cast<std::uint32_t>(qty),
				});
			}
		}
	}

	// Appends the COBJ scrap components for one mod pick to a_out (no player
	// mutation). Shared by the legacy "grant now" path and the pre-scrap
	// staging path.
	static void CollectScrapComponentsForMod(const PendingModPick& a_pick, std::vector<DeferredItem>& a_out, bool a_dbg)
	{
		auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_pick.formID);
		if (!mod) {
			if (a_dbg) {
				logger::info("[BWS] ScrapParts: {:08X} '{}' -> GetFormByID returned null"sv,
					a_pick.formID, a_pick.label);
			}
			return;
		}

		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();
		RE::TESObjectMISC* looseItem = nullptr;
		if (auto it = loose.find(mod); it != loose.end()) {
			looseItem = it->second;
		}

		auto* cobj = FindCobjForMod(mod, looseItem);
		if (!cobj || !cobj->requiredItems) {
			if (a_dbg) {
				logger::info("[BWS] ScrapParts: {:08X} '{}' -> no COBJ found (checked OMOD + MISC)"sv,
					a_pick.formID, a_pick.label);
			}
			return;
		}

		if (a_dbg) {
			logger::info("[BWS] ScrapParts: {:08X} '{}' -> COBJ {:08X}, collecting ingredients"sv,
				a_pick.formID, a_pick.label,
				static_cast<std::uint32_t>(cobj->GetFormID()));
		}

		for (const auto& tup : *cobj->requiredItems) {
			auto* form = tup.first;
			const auto qty = tup.second.i;
			if (!form || qty == 0) {
				continue;
			}
			// BGSComponent ingredients are granted as their scrap MISC item,
			// matching what the vanilla scrap pipeline lists and grants.
			RE::TESBoundObject* toAdd = nullptr;
			if (auto* comp = form->As<RE::BGSComponent>(); comp && comp->scrapItem) {
				toAdd = comp->scrapItem;
			} else {
				toAdd = form->As<RE::TESBoundObject>();
			}
			if (!toAdd) {
				continue;
			}
			a_out.push_back(DeferredItem{
				static_cast<std::uint32_t>(toAdd->GetFormID()),
				static_cast<std::int32_t>(qty)
			});
			if (a_dbg) {
				logger::info("[BWS]   +{}x {:08X} (from {:08X})"sv, qty,
					static_cast<std::uint32_t>(toAdd->GetFormID()),
					static_cast<std::uint32_t>(form->GetFormID()));
			}
		}
	}

	static void GiveScrapComponentsForMod(RE::PlayerCharacter*, const PendingModPick& a_pick, bool a_dbg)
	{
		auto items = std::make_shared<std::vector<DeferredItem>>();
		CollectScrapComponentsForMod(a_pick, *items, a_dbg);
		DeferAddItemsToPlayer(std::move(items));
	}

	static void ApplyRecoveryImpl(const PendingWeaponScrap& a_data, bool a_allMods, bool a_recipe)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			logger::warn("[BWS] ApplyRecoveryImpl: no player singleton"sv);
			return;
		}

		const bool dbg = BWS::Settings::Get().debugLogging.load();
		auto items = std::make_shared<std::vector<DeferredItem>>();

		if (a_allMods) {
			auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();
			if (dbg) {
				logger::info("[BWS] ApplyRecoveryImpl: recovering {} mods, loose-mod map size={}"sv,
					a_data.mods.size(), loose.size());
			}
			for (const auto& pick : a_data.mods) {
				auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(pick.formID);
				if (!mod) {
					if (dbg) {
						logger::info("[BWS]   formID {:08X} '{}': GetFormByID returned null"sv,
							pick.formID, pick.label);
					}
					continue;
				}
				const auto it = loose.find(mod);
				if (it == loose.end() || !it->second) {
					if (dbg) {
						logger::info("[BWS]   formID {:08X} '{}': no loose-mod item in map"sv,
							pick.formID, pick.label);
					}
					continue;
				}
				items->push_back(DeferredItem{
					static_cast<std::uint32_t>(it->second->GetFormID()), 1
				});
				if (dbg) {
					logger::info("[BWS]   formID {:08X} '{}': queued loose item {:08X}"sv,
						pick.formID, pick.label,
						static_cast<std::uint32_t>(it->second->GetFormID()));
				}
			}
		}

		if (a_recipe) {
			for (const auto& line : a_data.recipeMaterials) {
				if (line.ingredientFormID == 0 || line.count == 0) {
					continue;
				}
				auto* bound = ResolveIngredientForAdd(line.ingredientFormID);
				if (!bound) {
					continue;
				}
				items->push_back(DeferredItem{
					static_cast<std::uint32_t>(bound->GetFormID()),
					static_cast<std::int32_t>(line.count)
				});
			}
		}

		const bool any = !items->empty();
		if (any) {
			DeferAddItemsToPlayer(std::move(items));
			if (BWS::Settings::Get().showApplyHudMessage.load()) {
				RE::SendHUDMessage::ShowHUDMessage("Better Weapon Scrapping: recovery applied.", nullptr, false, false);
			}
		}
		if (!any && dbg) {
			logger::info("[BWS] ApplyRecoveryImpl: nothing was recovered (allMods={}, recipe={})"sv,
				a_allMods, a_recipe);
		}
	}

	// Resolves the picker's current selection (kept mods -> loose mod items,
	// scrapped mods -> COBJ components, optional recipe materials) into one
	// flat item list. Used by both the legacy grant and pre-scrap staging.
	static void CollectSelectedRecoveryItems(std::vector<DeferredItem>& a_out)
	{
		const bool dbg = BWS::Settings::Get().debugLogging.load();
		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();

		for (std::size_t i = 0; i < g_active.mods.size(); ++i) {
			const bool selected = (i < g_modSelected.size() && g_modSelected[i] != 0);
			const auto& pick = g_active.mods[i];

			if (selected) {
				// Keep the mod: recover it as its loose MISC item.
				auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(pick.formID);
				if (!mod) {
					if (dbg) {
						logger::info("[BWS] Recover: {:08X} '{}' -> GetFormByID null"sv, pick.formID, pick.label);
					}
					continue;
				}
				const auto it = loose.find(mod);
				if (it != loose.end() && it->second) {
					a_out.push_back(DeferredItem{
						static_cast<std::uint32_t>(it->second->GetFormID()), 1
					});
					if (dbg) {
						logger::info("[BWS] Recover: {:08X} '{}' -> queued loose item {:08X}"sv,
							pick.formID, pick.label,
							static_cast<std::uint32_t>(it->second->GetFormID()));
					}
				} else if (dbg) {
					logger::info("[BWS] Recover: {:08X} '{}' -> no loose-mod item"sv, pick.formID, pick.label);
				}
			} else {
				// Scrap the mod for parts: its COBJ ingredients.
				CollectScrapComponentsForMod(pick, a_out, dbg);
			}
		}

		if (g_recipeSelected) {
			for (const auto& line : g_active.recipeMaterials) {
				if (line.ingredientFormID == 0 || line.count == 0) {
					continue;
				}
				auto* bound = ResolveIngredientForAdd(line.ingredientFormID);
				if (!bound) {
					continue;
				}
				a_out.push_back(DeferredItem{
					static_cast<std::uint32_t>(bound->GetFormID()),
					static_cast<std::int32_t>(line.count)
				});
			}
		}
	}

	// Legacy (post-scrap) Apply: add the selection to the player directly.
	static void ApplyRecovery()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			DismissPopup();
			return;
		}

		auto items = std::make_shared<std::vector<DeferredItem>>();
		CollectSelectedRecoveryItems(*items);

		if (!items->empty()) {
			DeferAddItemsToPlayer(std::move(items));
		}
		if (BWS::Settings::Get().showApplyHudMessage.load()) {
			RE::SendHUDMessage::ShowHUDMessage("Better Weapon Scrapping: recovery applied.", nullptr, false, false);
		}
		DismissPopup();
	}

	// Pre-scrap Apply: stage the selection for the BuildWeaponScrappingArray
	// detour and let the vanilla scrap run. The native confirm dialog then
	// lists the staged items alongside the vanilla scrap yields, and the
	// game's own accept path grants everything (no AddObjectToContainer).
	static void ApplyPreScrapAndInvokeVanilla()
	{
		std::vector<DeferredItem> items;
		CollectSelectedRecoveryItems(items);

		std::vector<RecoveryGrantItem> staged;
		staged.reserve(items.size());
		for (const auto& di : items) {
			if (di.formID != 0 && di.count > 0) {
				staged.push_back(RecoveryGrantItem{ di.formID, static_cast<std::uint32_t>(di.count) });
			}
		}

		logger::info("[BWS] pre-scrap apply: staging {} item(s), invoking vanilla scrap"sv, staged.size());
		BWS::ExamineMenuBridge::StageRecoveryItems(std::move(staged));
		DismissPopup();
		BWS::ExamineMenuBridge::InvokeVanillaScrap();
	}

	// What the player gets back when a mod is removed from the weapon while
	// KEEPING the weapon.
	enum class RemoveYield
	{
		kLooseItems,  // return each removed mod as its loose OMOD misc item
		kComponents   // break each removed mod into its crafting components
	};

	// "Keep weapon" action. Detaches every attached mod from the examined
	// weapon in the player's inventory using the game's own remove path
	// (BGSInventoryItem::ModifyModDataFunctor with attach=false, applied via
	// TESObjectREFR::FindAndWriteStackDataForInventoryItem — the same path the
	// weapons workbench UI uses), then grants the recovered items. The weapon
	// itself is never scrapped and the vanilla scrap is never invoked.
	static void RemoveModsKeepWeapon(RemoveYield a_yield)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			DismissPopup();
			return;
		}
		const bool dbg = BWS::Settings::Get().debugLogging.load();

		// Snapshot state before DismissPopup() clears g_active.
		const std::uint32_t weaponFID = g_active.weaponBaseFormID;
		const std::uint32_t stackID   = g_active.modStackID;

		struct DetachTarget
		{
			std::uint32_t formID;
			std::uint8_t  attachIndex;
		};
		std::vector<DetachTarget> detach;
		detach.reserve(g_active.mods.size());

		// Build the grant list according to the chosen yield (independent of
		// the per-mod checkboxes: these buttons act on ALL attached mods).
		auto items = std::make_shared<std::vector<DeferredItem>>();
		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();

		for (const auto& pick : g_active.mods) {
			detach.push_back({ pick.formID, pick.attachIndex });

			if (a_yield == RemoveYield::kLooseItems) {
				if (auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(pick.formID)) {
					if (auto it = loose.find(mod); it != loose.end() && it->second) {
						items->push_back(DeferredItem{
							static_cast<std::uint32_t>(it->second->GetFormID()), 1 });
					}
				}
			} else {
				CollectScrapComponentsForMod(pick, *items, dbg);
			}
		}

		logger::info(
			"[BWS] remove-mods (keep weapon, {}): weapon {:08X} stack {}, {} mod(s), {} grant stack(s)"sv,
			a_yield == RemoveYield::kLooseItems ? "loose" : "components",
			weaponFID, stackID, detach.size(), items->size());

		DismissPopup();

		// Inventory mutation must run on the game thread, not during ImGui
		// render. Detach first, then grant.
		auto job = [weaponFID, stackID, detach = std::move(detach), items, dbg]() {
			auto* pl = RE::PlayerCharacter::GetSingleton();
			auto* weaponBase = RE::TESForm::GetFormByID<RE::TESBoundObject>(weaponFID);
			if (!pl || !weaponBase) {
				logger::warn("[BWS] remove-mods: player or weapon base missing; aborting"sv);
				return;
			}

			for (const auto& t : detach) {
				auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(t.formID);
				if (!mod) {
					continue;
				}
				bool ok = false;
				RE::BGSInventoryItem::CheckStackIDFunctor  compare{ stackID };
				RE::BGSInventoryItem::ModifyModDataFunctor writer{
					mod, static_cast<std::int8_t>(t.attachIndex), /*attach*/ false, std::addressof(ok)
				};
				pl->FindAndWriteStackDataForInventoryItem(weaponBase, compare, writer);
				if (dbg) {
					logger::info("[BWS]   detach {:08X} (slot {}) -> {}"sv,
						t.formID, t.attachIndex, ok ? "applied" : "not-applied");
				}
			}

			for (const auto& di : *items) {
				if (auto* bound = RE::TESForm::GetFormByID<RE::TESBoundObject>(di.formID)) {
					pl->AddObjectToContainer(bound, nullptr, di.count, nullptr,
						static_cast<RE::ITEM_REMOVE_REASON>(0));
				}
			}

			if (BWS::Settings::Get().showApplyHudMessage.load()) {
				RE::SendHUDMessage::ShowHUDMessage(
					"Better Weapon Scrapping: mods removed from weapon.", nullptr, false, false);
			}
			RefreshExamineMenuModList();
		};

		if (const auto* tasks = F4SE::GetTaskInterface()) {
			tasks->AddTask(job);
		} else {
			job();
		}
	}

	static void ScrapAllModsForParts(const PendingWeaponScrap& a_data)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		const bool dbg = BWS::Settings::Get().debugLogging.load();
		for (const auto& pick : a_data.mods) {
			GiveScrapComponentsForMod(player, pick, dbg);
		}
		if (BWS::Settings::Get().showApplyHudMessage.load()) {
			RE::SendHUDMessage::ShowHUDMessage("Better Weapon Scrapping: scrap components received.", nullptr, false, false);
		}
	}

	static void ShowPerModDialog(PendingWeaponScrap a_data, std::vector<std::uint8_t> a_choices, std::size_t a_index);

	class PerModNativeCallback : public RE::IMessageBoxCallback
	{
	public:
		PerModNativeCallback(PendingWeaponScrap a_data, std::vector<std::uint8_t> a_choices, std::size_t a_index) :
			m_data(std::move(a_data)), m_choices(std::move(a_choices)), m_index(a_index) {}

		void operator()(std::uint8_t a_buttonIdx) override
		{
			m_choices[m_index] = a_buttonIdx;
			ShowPerModDialog(std::move(m_data), std::move(m_choices), m_index + 1);
		}

	private:
		PendingWeaponScrap        m_data;
		std::vector<std::uint8_t> m_choices;
		std::size_t               m_index;
	};

	static void ApplyPerModChoices(const PendingWeaponScrap& a_data, const std::vector<std::uint8_t>& a_choices, bool a_hasRecipe)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		const bool dbg = BWS::Settings::Get().debugLogging.load();
		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();
		auto items = std::make_shared<std::vector<DeferredItem>>();

		for (std::size_t i = 0; i < a_data.mods.size(); ++i) {
			const auto choice = (i < a_choices.size()) ? a_choices[i] : std::uint8_t{ 2 };
			const auto& pick = a_data.mods[i];

			if (choice == 0) {
				auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(pick.formID);
				if (!mod) {
					continue;
				}
				const auto it = loose.find(mod);
				if (it != loose.end() && it->second) {
					items->push_back(DeferredItem{
						static_cast<std::uint32_t>(it->second->GetFormID()), 1
					});
					if (dbg) {
						logger::info("[BWS] PerMod Keep: {:08X} '{}' -> queued loose item"sv, pick.formID, pick.label);
					}
				}
			} else if (choice == 1) {
				GiveScrapComponentsForMod(player, pick, dbg);
			}
		}

		if (a_hasRecipe) {
			for (const auto& line : a_data.recipeMaterials) {
				if (line.ingredientFormID == 0 || line.count == 0) {
					continue;
				}
				auto* bound = ResolveIngredientForAdd(line.ingredientFormID);
				if (bound) {
					items->push_back(DeferredItem{
						static_cast<std::uint32_t>(bound->GetFormID()),
						static_cast<std::int32_t>(line.count)
					});
				}
			}
		}

		if (!items->empty()) {
			DeferAddItemsToPlayer(std::move(items));
		}
		if (BWS::Settings::Get().showApplyHudMessage.load()) {
			RE::SendHUDMessage::ShowHUDMessage("Better Weapon Scrapping: recovery applied.", nullptr, false, false);
		}
	}

	static void ShowPerModDialog(PendingWeaponScrap a_data, std::vector<std::uint8_t> a_choices, std::size_t a_index)
	{
		if (a_index >= a_data.mods.size()) {
			const bool hasRecipe = !a_data.recipeMaterials.empty();
			ApplyPerModChoices(a_data, a_choices, hasRecipe);
			return;
		}

		auto* mgr = RE::MessageMenuManager::GetSingleton();
		if (!mgr) {
			return;
		}

		const auto& pick = a_data.mods[a_index];
		const std::string body = std::format("Mod {} of {}:\n{}",
			a_index + 1, a_data.mods.size(),
			pick.label.empty() ? "(mod)" : pick.label);

		auto* cb = new PerModNativeCallback(std::move(a_data), std::move(a_choices), a_index);

		mgr->Create(
			"Select Mod Action",
			body.c_str(),
			cb,
			0,
			"Keep",
			"Scrap for Parts",
			"Skip",
			nullptr,
			false);
	}

	class NativeScrapCallback : public RE::IMessageBoxCallback
	{
	public:
		NativeScrapCallback(PendingWeaponScrap a_data, bool a_hasRecipe) :
			m_data(std::move(a_data)), m_hasRecipe(a_hasRecipe) {}

		void operator()(std::uint8_t a_buttonIdx) override
		{
			switch (a_buttonIdx) {
			case 0:
				ApplyRecoveryImpl(m_data, true, m_hasRecipe);
				break;
			case 1:
			{
				std::vector<std::uint8_t> choices(m_data.mods.size(), 2);
				ShowPerModDialog(std::move(m_data), std::move(choices), 0);
				break;
			}
			case 2:
				ScrapAllModsForParts(m_data);
				break;
			default:
				break;
			}
		}

	private:
		PendingWeaponScrap m_data;
		bool               m_hasRecipe;
	};

	static void ShowNativeMessageBox(PendingWeaponScrap a_pending)
	{
		auto* mgr = RE::MessageMenuManager::GetSingleton();
		if (!mgr) {
			logger::warn("[BWS] MessageMenuManager unavailable, falling back."sv);
			return;
		}

		std::ostringstream body;
		body << a_pending.weaponDisplayName << " scrapped.\n";

		if (!a_pending.mods.empty()) {
			body << a_pending.mods.size() << " mod(s) can be recovered.";
		} else {
			body << "No recoverable mods.";
		}

		std::string bodyStr = body.str();
		const bool hasRecipe = !a_pending.recipeMaterials.empty();
		auto* cb = new NativeScrapCallback(std::move(a_pending), hasRecipe);

		mgr->Create(
			"Weapon Scrapped",
			bodyStr.c_str(),
			cb,
			0,
			"Keep All Mods",
			"Select Mods",
			"Scrap for Parts",
			"Scrap Weapon Only",
			false);
	}

	static void RenderScrapModal()
	{
		// One-shot focus grab per popup open (see SetNextWindowFocus below).
		static bool s_focusApplied = false;

		if (!g_popupVisible.load()) {
			s_focusApplied = false;
			return;
		}

		ImGui::SetCurrentContext(g_imguiCtx);

		ImGuiIO& io = ImGui::GetIO();
		io.MouseDrawCursor = true;

		const ImVec2 screenMax{ io.DisplaySize.x, io.DisplaySize.y };
		ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), screenMax, IM_COL32(0, 0, 0, 40));

		const float sc = io.DisplaySize.y / 1080.0f;
		const float btnW = std::round(140.0f * sc);

		const ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
		ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		const float winW = std::round(620.0f * sc);
		ImGui::SetNextWindowSize(ImVec2(winW, 0.0f), ImGuiCond_Always);

		// Focus ONCE when the popup opens — never per-frame. A per-frame
		// SetNextWindowFocus re-focuses the parent window every frame, which
		// rips gamepad/keyboard nav back out of child windows (the mod list)
		// the instant the user navigates into them: the child highlights its
		// first item for one frame, then focus snaps back to the parent.
		if (!s_focusApplied) {
			ImGui::SetNextWindowFocus();
			s_focusApplied = true;
		}

		ImGui::Begin(
			"##bws_weapon_scrap",
			nullptr,
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_AlwaysAutoResize);

		// True while gamepad/keyboard nav focus sits INSIDE one of this
		// window's child lists (mod checkboxes, tally list). ImGui's built-in
		// nav-cancel (B) already pops focus from a child back to the parent
		// window — the B-dismiss handler below must stay quiet on that press,
		// otherwise one press would exit the list AND close the popup.
		const bool navInChildList =
			ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsWindowFocused();

		if (g_preScrapMode) {
			ImGui::TextUnformatted("Scrap Weapon \xe2\x80\x94 Recovery Options");
		} else {
			ImGui::TextUnformatted("Weapon Scrapped \xe2\x80\x94 Recovery Options");
		}
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::TextWrapped("%s", g_active.weaponDisplayName.c_str());
		ImGui::Spacing();

		if (!g_active.mods.empty()) {
			ImGui::TextUnformatted("Attached mods:");
			ImGui::TextDisabled("Checked = recover as loose item. Unchecked = scrap for crafting components.");
			ImGui::Spacing();
			const float modLineH = ImGui::GetTextLineHeightWithSpacing();
			const float modsDesiredH =
				static_cast<float>(g_active.mods.size()) * modLineH + ImGui::GetStyle().WindowPadding.y + 8.f;
			const float modsMaxH = std::max(100.0f, io.DisplaySize.y * 0.42f);
			const float modsPickH = std::min(modsDesiredH, modsMaxH);
			ImGui::BeginChild(
				"##bws_mod_checks",
				ImVec2(-FLT_MIN, modsPickH),
				ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
			for (std::size_t i = 0; i < g_active.mods.size(); ++i) {
				const char* label = g_active.mods[i].label.empty() ? "(mod)" : g_active.mods[i].label.c_str();
				bool        sel = g_modSelected[i] != 0;
				ImGui::PushID(static_cast<int>(i));
				ImGui::Checkbox(label, &sel);
				ImGui::PopID();
				g_modSelected[i] = sel ? 1 : 0;
			}
			ImGui::EndChild();
			ImGui::Spacing();
		} else {
			ImGui::TextDisabled("No object mods were found on this instance.");
			ImGui::Spacing();
		}

		if (!g_active.recipeMaterials.empty()) {
			ImGui::Separator();
			ImGui::TextUnformatted("Weapon recipe materials:");
			ImGui::Spacing();
			ImGui::Checkbox("Include weapon recipe materials", &g_recipeSelected);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Adds ingredients from the constructible object that creates this weapon.");
			}
			ImGui::Spacing();
		}

		{
			ImGui::Separator();
			ImGui::TextUnformatted("You will receive:");
			ImGui::Spacing();

			struct Tally { std::uint32_t formID; std::string name; std::uint32_t count; };
			std::vector<Tally> combined;
			auto addToTally = [&](std::uint32_t fid, const std::string& name, std::uint32_t qty) {
				for (auto& t : combined) {
					if (t.formID == fid) {
						t.count += qty;
						return;
					}
				}
				combined.push_back(Tally{ fid, name, qty });
			};

			std::uint32_t keptCount = 0;
			std::uint32_t scrappedCount = 0;

			for (std::size_t i = 0; i < g_active.mods.size(); ++i) {
				const bool selected = (i < g_modSelected.size() && g_modSelected[i] != 0);
				if (selected) {
					++keptCount;
				} else {
					++scrappedCount;
					if (i < g_modRecipes.size()) {
						for (const auto& ing : g_modRecipes[i]) {
							addToTally(ing.formID, ing.name, ing.count);
						}
					}
				}
			}

			if (g_recipeSelected) {
				for (const auto& line : g_active.recipeMaterials) {
					addToTally(line.ingredientFormID, line.displayName, line.count);
				}
			}

			const float lineH = ImGui::GetTextLineHeightWithSpacing();
			std::uint32_t tallyLines = 0;
			if (keptCount > 0) {
				tallyLines += 1 + keptCount;
			}
			if (!combined.empty()) {
				if (keptCount > 0) {
					tallyLines += 1;
				}
				tallyLines += 1 + static_cast<std::uint32_t>(combined.size());
			}
			if (keptCount == 0 && combined.empty()) {
				tallyLines = 1;
			}
			const float desiredTallyH = static_cast<float>(tallyLines) * lineH + ImGui::GetStyle().WindowPadding.y * 2.f + 12.f;
			const float maxTallyH = std::max(72.0f, io.DisplaySize.y * 0.62f);
			const float listHeight = std::min(desiredTallyH, maxTallyH);
			ImGui::BeginChild(
				"##bws_tally",
				ImVec2(-FLT_MIN, listHeight),
				ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
				ImGuiWindowFlags_HorizontalScrollbar);

			if (keptCount > 0) {
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Loose mods recovered: %u", keptCount);
				for (std::size_t i = 0; i < g_active.mods.size(); ++i) {
					if (i < g_modSelected.size() && g_modSelected[i] != 0) {
						const char* n = g_active.mods[i].label.empty() ? "(mod)" : g_active.mods[i].label.c_str();
						ImGui::BulletText("%s", n);
					}
				}
			}

			if (!combined.empty()) {
				if (keptCount > 0) {
					ImGui::Spacing();
				}
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Scrap components (from %u mod%s):", scrappedCount, scrappedCount == 1 ? "" : "s");
				for (const auto& t : combined) {
					ImGui::BulletText("%u x %s", t.count, t.name.c_str());
				}
			}

			if (keptCount == 0 && combined.empty()) {
				ImGui::TextDisabled("Nothing selected.");
			}

			ImGui::EndChild();
			ImGui::Spacing();
		}

		ImGui::Separator();
		if (g_preScrapMode) {
			// Pre-scrap: the weapon has NOT been scrapped yet. The vanilla
			// confirm dialog follows and shows the combined yield list.
			if (ImGui::Button("Scrap + Recover", ImVec2(std::round(btnW * 1.4f), 0))) {
				ApplyPreScrapAndInvokeVanilla();
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Continue to the game's scrap confirmation with the selection above added to the yield.");
			}
			ImGui::SameLine();
			if (ImGui::Button("Scrap Only", ImVec2(btnW, 0))) {
				// Vanilla scrap with no extras staged.
				DismissPopup();
				BWS::ExamineMenuBridge::InvokeVanillaScrap();
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Vanilla scrap: no loose mods or extra materials.");
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(btnW, 0))) {
				// Abort entirely — the weapon is untouched.
				DismissPopup();
			}

			// Second row: keep the weapon, just strip its mods. Only makes
			// sense when the weapon actually has removable mods.
			// NOTE: plain-ASCII labels only — the game-font atlas has no
			// U+2192 arrow glyph (it rendered as '?').
			if (!g_active.mods.empty()) {
				ImGui::Spacing();
				ImGui::TextDisabled("Keep the weapon \xe2\x80\x94 detach ALL its mods and get back:");
				if (ImGui::Button("Detach Mods, Keep as Items", ImVec2(0, 0))) {
					// Detach all mods; return each as its loose mod item.
					RemoveModsKeepWeapon(RemoveYield::kLooseItems);
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Detach every mod from this weapon and place the loose mods in your inventory. "
						"The weapon stays and reverts to its default parts.");
				}
				ImGui::SameLine();
				if (ImGui::Button("Detach Mods, Scrap for Parts", ImVec2(0, 0))) {
					// Detach all mods; break each into crafting components.
					RemoveModsKeepWeapon(RemoveYield::kComponents);
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Detach every mod from this weapon and break the mods into crafting components. "
						"The weapon stays and reverts to its default parts.");
				}
			}
		} else {
			if (ImGui::Button("Apply", ImVec2(btnW, 0))) {
				ApplyRecovery();
			}
			ImGui::SameLine();
			if (ImGui::Button("Skip", ImVec2(btnW, 0))) {
				DismissPopup();
			}
		}

		// Gamepad B = dismiss (same as Cancel/Skip). In pre-scrap mode the
		// weapon is untouched, so closing is always safe. Skipped while nav
		// focus is inside a child list: that press only pops focus back to
		// this window (ImGui built-in); the NEXT press dismisses.
		if (!navInChildList && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
			DismissPopup();
		}

		ImGui::End();
	}

	static void TryInitImGuiFromRenderer()
	{
		if (g_initialized.load()) {
			return;
		}

		auto* rd = BWS::Graphics::RendererData::GetSingleton();
		if (!rd || !rd->device || !rd->context) {
			return;
		}

		auto* swapChain = rd->renderWindow[0].swapChain;
		if (!swapChain) {
			return;
		}

		DXGI_SWAP_CHAIN_DESC desc{};
		if (FAILED(swapChain->GetDesc(&desc))) {
			return;
		}

		g_hwnd = desc.OutputWindow;
		g_device = reinterpret_cast<ID3D11Device*>(rd->device);
		g_deviceCtx = reinterpret_cast<ID3D11DeviceContext*>(rd->context);

		IMGUI_CHECKVERSION();
		g_imguiCtx = ImGui::CreateContext();
		ImGui::SetCurrentContext(g_imguiCtx);

		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		// Full controller + keyboard navigation for every BWS menu. The
		// Win32 backend polls XInput itself (dynamically resolved via
		// LoadLibrary/GetProcAddress, so it is NOT affected by our IAT
		// suppression hook) and feeds ImGui nav events when this flag is on.
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;

		ImGui_ImplWin32_Init(g_hwnd);
		ImGui_ImplDX11_Init(g_device, g_deviceCtx);

		RECT rect{};
		::GetClientRect(g_hwnd, &rect);
		io.DisplaySize = ImVec2(
			static_cast<float>(rect.right - rect.left),
			static_cast<float>(rect.bottom - rect.top));

		{
			const float resSc = io.DisplaySize.y / 1080.0f;
			const float baseFontSize = std::round(18.0f * resSc);
			bool fontLoaded = false;

			logger::info("Better Weapon Scrapping: display {}x{}, resSc={:.2f}, font atlas size={:.0f}px",
				static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y), resSc, baseFontSize);

			const auto customPath = BWS::Settings::Get().GetCustomFontPath();
			if (!customPath.empty() && std::filesystem::exists(customPath)) {
				if (io.Fonts->AddFontFromFileTTF(customPath.c_str(), baseFontSize)) {
					fontLoaded = true;
					logger::info("Better Weapon Scrapping: loaded custom font '{}'", customPath);
				} else {
					logger::warn("Better Weapon Scrapping: failed to load custom font '{}', falling back", customPath);
				}
			} else if (!customPath.empty()) {
				logger::warn("Better Weapon Scrapping: custom font path '{}' not found, falling back", customPath);
			}

			if (!fontLoaded) {
				const char* fontPaths[] = {
					"Data/F4SE/Plugins/Fonts/FalloutMenuFont.ttf",
					"Data/F4SE/Plugins/Fonts/MainFont.ttf",
				};
				for (const auto* path : fontPaths) {
					if (std::filesystem::exists(path)) {
						if (io.Fonts->AddFontFromFileTTF(path, baseFontSize)) {
							fontLoaded = true;
							logger::info("Better Weapon Scrapping: loaded font '{}'", path);
							break;
						}
					}
				}
			}

			if (!fontLoaded) {
				wchar_t winDir[MAX_PATH]{};
				::GetWindowsDirectoryW(winDir, MAX_PATH);
				auto segoeUI = std::filesystem::path(winDir) / L"Fonts" / L"segoeui.ttf";
				if (std::filesystem::exists(segoeUI)) {
					auto segoeStr = segoeUI.string();
					if (io.Fonts->AddFontFromFileTTF(segoeStr.c_str(), baseFontSize)) {
						fontLoaded = true;
						logger::info("Better Weapon Scrapping: loaded system font 'segoeui.ttf'");
					}
				}
			}

			if (!fontLoaded) {
				io.Fonts->AddFontDefault();
				logger::info("Better Weapon Scrapping: using ImGui default font");
			}

			io.Fonts->Build();
		}

		g_origWndProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrA(
			g_hwnd,
			GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(&WndProcHook)));

		g_initialized.store(true);
		logger::info("Better Weapon Scrapping: ImGui overlay initialized (RendererData + HUDMenu::PostDisplay)"sv);

		// Gamepad: hide controller input from the game while a BWS menu is
		// open, and watch for the "open scrap-mod picker" pad button. Done
		// here (first HUD frame) so every other plugin's own XInput IAT pass
		// has already run and gets chained, not clobbered.
		BWS::GamepadInput::Install();
		spdlog::default_logger()->flush();
	}

	static void HookedHudPostDisplay(RE::IMenu* a_menu)
	{
		TryInitImGuiFromRenderer();

		if (!g_initialized.load() || !g_imguiCtx) {
			if (g_origHudPostDisplay) {
				g_origHudPostDisplay(a_menu);
			}
			return;
		}

		ImGui::SetCurrentContext(g_imguiCtx);
		RefreshBwsImGuiStyle();

		const bool imguiCapturesInput =
			g_popupVisible.load() || BWS::ScrapModManager::BlocksGameInput();
		ImGui::GetIO().MouseDrawCursor = imguiCapturesInput;

		ProcessPendingScrapQueue();

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		if (GImGui) {
			GImGui->NavWindowingTarget = nullptr;
		}

		// If ExamineMenu is actually gone (ground truth) but we still hold
		// any input/pause state, release it. Relies on GetMenuOpen, not only
		// our MenuOpenCloseEvent flag — that race was leaving players locked.
		if (!IsExamineMenuReallyOpen()) {
			const bool stranded =
				g_popupVisible.load() ||
				BWS::ScrapModManager::BlocksGameInput() ||
				g_worldPauseLayers > 0 ||
				g_bwsOwnsIgnoreKeyboard ||
				BWS::ScrapModManager::IsExamineMenuOpen();
			if (stranded) {
				ReleaseAllBwsInputState("HUD tick: ExamineMenu gone, BWS state stranded");
			}
		}

		BWS::ScrapModManager::TickPostExamineInputHeal();

		RenderScrapModal();
		BWS::ScrapModManager::Draw();

		ImGui::EndFrame();
		ImGui::Render();
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		if (g_origHudPostDisplay) {
			g_origHudPostDisplay(a_menu);
		}
	}

	static void DismissAllImGuiMenus()
	{
		if (g_popupVisible.load()) {
			DismissPopup();
		}
		if (BWS::ScrapModManager::BlocksGameInput()) {
			BWS::ScrapModManager::ForceClose();
		}
	}

	LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (g_initialized.load()) {
			// Ground truth: if ExamineMenu is gone, never swallow input.
			if (!IsExamineMenuReallyOpen() &&
				(g_popupVisible.load() || BWS::ScrapModManager::BlocksGameInput() ||
					g_bwsOwnsIgnoreKeyboard || g_worldPauseLayers > 0)) {
				ReleaseAllBwsInputState("WndProc: ExamineMenu gone, refusing to swallow input");
			}

			if (msg == WM_KEYDOWN && !g_popupVisible.load() &&
				!BWS::ScrapModManager::BlocksGameInput()) {
				if (BWS::ScrapModManager::TryHotkey(wParam)) {
					return 0;
				}
			}
		}

		const bool ourMenuActive =
			g_initialized.load() &&
			(g_popupVisible.load() || BWS::ScrapModManager::BlocksGameInput());

		if (ourMenuActive) {
			if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
				DismissAllImGuiMenus();
				return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
			}

			// TAB also closes our menus — but unlike ESC it must be SWALLOWED:
			// TAB is "BACK" inside the workbench ExamineMenu, so forwarding it
			// would close our menu AND back the player out of the workbench in
			// the same press.
			if (msg == WM_KEYDOWN && wParam == VK_TAB) {
				DismissAllImGuiMenus();
				return 0;
			}

			if (msg == WM_ACTIVATEAPP && wParam == FALSE) {
				DismissAllImGuiMenus();
				return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
			}

			if (msg == WM_KILLFOCUS) {
				DismissAllImGuiMenus();
				return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
			}

			ImGui::SetCurrentContext(g_imguiCtx);
			ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

			switch (msg) {
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
			case WM_CHAR:
			case WM_MOUSEMOVE:
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MOUSEWHEEL:
			case WM_INPUT:
				return 0;
			}
		}
		return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
	}

	BOOL WINAPI ClipCursorHook(const RECT* lpRect)
	{
		if (g_popupVisible.load() || BWS::ScrapModManager::BlocksGameInput()) {
			return g_origClipCursor(nullptr);
		}
		return g_origClipCursor(lpRect);
	}
}

void ScrapOverlay::Install()
{
	REL::Relocation<std::uintptr_t> clipIAT{ REL::ID(641385) };
	auto* clipPtr = reinterpret_cast<std::uintptr_t*>(clipIAT.address());
	g_origClipCursor = reinterpret_cast<ClipCursorFn>(*clipPtr);
	REL::safe_write(clipIAT.address(), reinterpret_cast<std::uintptr_t>(&ClipCursorHook));
	logger::info("Better Weapon Scrapping: ClipCursor IAT hook installed"sv);

	REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::HUDMenu[0] };
	g_origHudPostDisplay = reinterpret_cast<PostDisplayFn>(
		vtbl.write_vfunc(0x6, reinterpret_cast<std::uintptr_t>(&HookedHudPostDisplay)));
	logger::info("Better Weapon Scrapping: HUDMenu::PostDisplay (ImGui) hook installed"sv);
	spdlog::default_logger()->flush();
}

void ScrapOverlay::QueuePending(PendingWeaponScrap a_pending)
{
	std::lock_guard lk(g_queueMtx);
	g_pendingQueue.push_back(std::move(a_pending));
}

bool ScrapOverlay::IsPopupVisible()
{
	return g_popupVisible.load();
}

void ScrapOverlay::ForceDismiss()
{
	if (g_popupVisible.load()) {
		DismissPopup();
	} else {
		// Even with no visible popup, clear any stranded ControlMap / pause
		// layers (e.g. workbench closed mid-transition).
		while (g_worldPauseLayers > 0) {
			PopWorldPause();
		}
		if (auto* cm = RE::ControlMap::GetSingleton()) {
			cm->SetIgnoreKeyboardMouse(false);
		}
		g_bwsOwnsIgnoreKeyboard = false;
	}
	std::lock_guard lk(g_queueMtx);
	g_pendingQueue.clear();
}
