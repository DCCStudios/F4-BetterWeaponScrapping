#include "PCH.h"
#include "BenchScrapTypes.h"
#include "ExamineMenuBridge.h"
#include "ScrapModManager.h"
#include "ScrapManager.h"
#include "ScrapOverlay.h"
#include "Settings.h"

#include "RE/Bethesda/BGSInventoryItem.h"
#include "RE/Bethesda/BGSMod.h"
#include "RE/Bethesda/BSExtraData.h"
#include "RE/Bethesda/BSFixedString.h"
#include "RE/Bethesda/Events.h"
#include "RE/Bethesda/ControlMap.h"
#include "RE/Bethesda/FormComponents.h"
#include "RE/Bethesda/PlayerControls.h"
#include "RE/Bethesda/TESBoundObjects.h"
#include "RE/Bethesda/TESDataHandler.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/TESObjectREFRs.h"
#include "RE/Bethesda/UI.h"
#include "RE/Bethesda/UIMessage.h"
#include "RE/Bethesda/UIMessageQueue.h"
#include "RE/Bethesda/SendHUDMessage.h"
#include "RE/Bethesda/UserEvents.h"

#include "F4SE/API.h"
#include "F4SE/Interfaces.h"
#include "RE/Bethesda/Settings.h"
#include "RE/NetImmerse/NiColor.h"
#include "REL/Relocation.h"

#pragma warning(push, 0)
#include <imgui.h>
#pragma warning(pop)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace
{
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

		// Rebuild the menu's cached crafting inventory FIRST — the workbench
		// requirement/component counts come from
		// WorkbenchMenuBase::optimizedAutoBuildInv (a snapshot built at menu
		// open), not from the live player inventory. Without this, granted
		// components only show up after re-entering the workbench.
		// Old-gen ID 769581 = WorkbenchMenuBase::UpdateOptimizedAutoBuildInv
		// (commonlibf4-main VariantID{ 769581, 2224955 }).
		{
			using UpdateInvFn = void (*)(void*);
			static REL::Relocation<UpdateInvFn> updateInv{ REL::ID(769581) };
			updateInv(raw);
		}

		constexpr std::size_t kVIdx_UpdateMenu = 0x15;
		reinterpret_cast<VoidFn>(vtbl[kVIdx_UpdateMenu])(raw);

		constexpr std::size_t kVIdx_UpdateModChoiceList = 0x3F;
		reinterpret_cast<VoidFn>(vtbl[kVIdx_UpdateModChoiceList])(raw);

		logger::info("[BWS] RefreshExamineMenuModList: called UpdateOptimizedAutoBuildInv + UpdateMenu + UpdateModChoiceList");
	}

	struct ModIngredientLine
	{
		std::uint32_t formID{ 0 };
		std::string   name;
		std::uint32_t count{ 0 };
	};

	std::atomic<bool> g_examineOpen{ false };
	std::atomic<bool> g_examineConfirmOpen{ false };
	RE::ExamineMenu*  g_examineMenuPtr{ nullptr };
	std::mutex       g_examineMtx;

	enum class FlowPhase : std::uint8_t
	{
		kNone,
		kPicker,
		kConfirm,
		kInvLoosePicker,
		kInvLooseConfirm,
		kSwapSuggest
	};
	std::atomic<FlowPhase> g_phase{ FlowPhase::kNone };

	std::string                 g_weaponName;
	std::vector<PendingModPick> g_mods;
	std::size_t                 g_confirmIndex{ 0 };
	std::vector<ModIngredientLine> g_confirmLines;

	// Picker mode toggle. Scrap mode (false): detach the picked mod and
	// grant its COBJ crafting components. Remove mode (true): detach the
	// picked mod and return it to the inventory as its loose mod item —
	// nothing is scrapped. Session-persistent (survives closing the picker).
	bool g_removeMode{ false };
	// Loose-item label shown on the confirm page while in Remove mode
	// (precomputed when the mod is picked).
	std::string g_confirmLooseLabel;

	struct InvLooseModPick
	{
		std::uint32_t modFormID{ 0 };
		std::uint32_t miscFormID{ 0 };
		std::uint32_t count{ 0 };
		std::string   label;
	};
	std::vector<InvLooseModPick> g_invLooseMods;
	std::size_t                  g_invLooseConfirmIdx{ 0 };
	std::vector<ModIngredientLine> g_invLooseConfirmLines;

	std::vector<PendingModPick> g_swapCandidates;
	std::int8_t                 g_swapSlotIndex{ -1 };

	std::optional<std::uint8_t> FindAttachIndexForMod(RE::BGSObjectInstanceExtra* a_inst, std::uint32_t a_modFormID)
	{
		if (!a_inst) {
			return std::nullopt;
		}
		for (const auto& oid : a_inst->GetIndexData()) {
			if (oid.disabled) {
				continue;
			}
			if (oid.objectID == a_modFormID) {
				return oid.index;
			}
		}
		return std::nullopt;
	}

	RE::ExtraDataList* GetPrimaryInstanceExtraList(RE::BGSInventoryItem* a_inv)
	{
		if (!a_inv) {
			return nullptr;
		}
		for (auto* stack = a_inv->stackData.get(); stack; stack = stack->nextStack.get()) {
			const auto* x = stack->extra.get();
			if (!x) {
				continue;
			}
			if (x->GetByType<RE::BGSObjectInstanceExtra>()) {
				return stack->extra.get();
			}
		}
		return nullptr;
	}

	// Raw ExamineMenu::modStack field. NOTE: verified in-game to NOT be a
	// plain index into the BGSInventoryItem stack list (detaching still hit
	// the wrong stack when it was used as one) — kept only as a last-resort
	// fallback and for diagnostics. The authoritative resolution is
	// FindPlayerStackIndexForInstance (content match), below.
	std::uint32_t GetExamineModStackIndex(const RE::ExamineMenu* a_menu)
	{
		return *reinterpret_cast<const std::uint32_t*>(
			reinterpret_cast<const std::byte*>(a_menu) + kExamineMenu_ModStack);
	}

	// Stack index the examine operations must target: the player stack whose
	// instance data matches the examined snapshot (see header comment), with
	// ExamineMenu::modStack as fallback when no content match exists.
	std::uint32_t ResolveExamineStackIndex(
		RE::ExamineMenu* a_menu, RE::TESBoundObject* a_base, const RE::BGSObjectInstanceExtra* a_snapshot)
	{
		if (const auto match = BWS::ScrapModManager::FindPlayerStackIndexForInstance(a_base, a_snapshot)) {
			return *match;
		}
		const std::uint32_t fallback = GetExamineModStackIndex(a_menu);
		logger::warn(
			"[BWS] ResolveExamineStackIndex: no player stack content-matches the examined instance; falling back to modStack={}"sv,
			fallback);
		return fallback;
	}

	struct ExamineWeaponCtx
	{
		RE::PlayerCharacter*        player{ nullptr };
		RE::TESObjectWEAP*          weapon{ nullptr };
		RE::BGSInventoryItem*       invItem{ nullptr };
		RE::BGSObjectInstanceExtra*  instExtra{ nullptr };
		std::uint32_t               stackIdx{ 0 };
	};

	std::optional<ExamineWeaponCtx> GetExamineWeaponCtx(RE::ExamineMenu* a_menu)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !a_menu) {
			return std::nullopt;
		}
		auto* invItem = reinterpret_cast<RE::BGSInventoryItem*>(
			reinterpret_cast<std::byte*>(a_menu) + kExamineMenu_ModdedInventoryItem);
		auto* weapBase = invItem->object ? invItem->object->As<RE::TESObjectWEAP>() : nullptr;
		if (!weapBase) {
			return std::nullopt;
		}
		auto* targetExtra = GetPrimaryInstanceExtraList(invItem);
		if (!targetExtra) {
			return std::nullopt;
		}
		auto* instExtra = targetExtra->GetByType<RE::BGSObjectInstanceExtra>();
		if (!instExtra) {
			return std::nullopt;
		}
		const std::uint32_t stackIdx = ResolveExamineStackIndex(a_menu, weapBase, instExtra);
		return ExamineWeaponCtx{
			.player    = player,
			.weapon    = weapBase,
			.invItem   = invItem,
			.instExtra = instExtra,
			.stackIdx  = stackIdx,
		};
	}

	bool IsExamineStackEquipped(RE::ExamineMenu* a_menu)
	{
		const auto ctxOpt = GetExamineWeaponCtx(a_menu);
		if (!ctxOpt || !ctxOpt->player->inventoryList) {
			return false;
		}
		// Match by stack INDEX (ExamineMenu::modStack), not by comparing the
		// snapshot's ExtraDataList pointer — moddedInventoryItem is a copy,
		// so its pointers need not match the player's real stack list.
		bool          equipped = false;
		std::uint32_t idx = 0;
		ctxOpt->player->inventoryList->ForEachStack(
			[&](RE::BGSInventoryItem& a_item) {
				return a_item.object == ctxOpt->weapon;
			},
			[&](RE::BGSInventoryItem&, RE::BGSInventoryItem::Stack& a_stack) {
				if (idx++ == ctxOpt->stackIdx) {
					equipped = a_stack.IsEquipped();
					return false;
				}
				return true;
			});
		return equipped;
	}

	void CollectInstalledModFormIDs(RE::BGSObjectInstanceExtra* a_inst, std::set<std::uint32_t>& a_out)
	{
		a_out.clear();
		if (!a_inst) {
			return;
		}
		for (const auto& oid : a_inst->GetIndexData()) {
			if (oid.disabled) {
				continue;
			}
			if (oid.objectID != 0) {
				a_out.insert(oid.objectID);
			}
		}
	}

	RE::BGSKeyword* GetModAttachKeyword(const RE::BGSMod::Attachment::Mod* a_mod)
	{
		if (!a_mod) {
			return nullptr;
		}
		return RE::detail::BGSKeywordGetTypedKeywordByIndex(
			RE::KeywordType::kAttachPoint, a_mod->attachPoint.keywordIndex);
	}

	bool ModAttachPointFitsWeapon(const RE::TESObjectWEAP* a_weapon, const RE::BGSMod::Attachment::Mod* a_mod)
	{
		if (!a_weapon || !a_mod) {
			return false;
		}
		auto* kw = GetModAttachKeyword(a_mod);
		if (!kw) {
			return false;
		}
		return a_weapon->attachParents.HasKeyword(kw);
	}

	bool ModFitsInstalledSlots(RE::BGSObjectInstanceExtra* a_inst, const RE::BGSMod::Attachment::Mod* a_candidate)
	{
		if (!a_inst || !a_candidate) {
			return false;
		}
		const auto candIdx = a_candidate->attachPoint.keywordIndex;
		for (const auto& oid : a_inst->GetIndexData()) {
			if (oid.disabled || oid.objectID == 0) {
				continue;
			}
			auto* instMod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(oid.objectID);
			if (instMod && instMod->attachPoint.keywordIndex == candIdx) {
				return true;
			}
		}
		return false;
	}

	bool ModCompatibleWithWeapon(const RE::TESObjectWEAP* a_weapon, RE::BGSObjectInstanceExtra* a_inst,
		const RE::BGSMod::Attachment::Mod* a_mod)
	{
		if (ModAttachPointFitsWeapon(a_weapon, a_mod)) {
			return true;
		}
		return ModFitsInstalledSlots(a_inst, a_mod);
	}

	bool ModSameAttachSlot(const RE::BGSMod::Attachment::Mod* a_lhs, const RE::BGSMod::Attachment::Mod* a_rhs)
	{
		if (!a_lhs || !a_rhs) {
			return false;
		}
		return a_lhs->attachPoint.keywordIndex == a_rhs->attachPoint.keywordIndex;
	}

	void CollectInventoryLooseModsForWeapon(RE::ExamineMenu* a_menu, std::vector<InvLooseModPick>& a_out)
	{
		a_out.clear();
		const auto ctxOpt = GetExamineWeaponCtx(a_menu);
		if (!ctxOpt) {
			return;
		}
		const bool dbg = BWS::Settings::Get().debugLogging.load();
		std::set<std::uint32_t> installed;
		CollectInstalledModFormIDs(ctxOpt->instExtra, installed);

		if (dbg) {
			logger::info("CollectLoose: weapon {:08X} '{}', {} installed mods, instExtra={}",
				ctxOpt->weapon->GetFormID(),
				ctxOpt->weapon->GetFullName() ? ctxOpt->weapon->GetFullName() : "?",
				installed.size(), fmt::ptr(ctxOpt->instExtra));
		}

		auto& looseMap = RE::BGSMod::Attachment::GetAllLooseMods();
		if (!ctxOpt->player->inventoryList) {
			return;
		}

		ctxOpt->player->inventoryList->ForEachStack(
			[&](RE::BGSInventoryItem&) { return true; },
			[&](RE::BGSInventoryItem& a_item, RE::BGSInventoryItem::Stack& a_stack) {
				auto* bound = a_item.object;
				if (!bound) {
					return true;
				}
				auto* misc = bound->As<RE::TESObjectMISC>();
				if (!misc) {
					return true;
				}
				for (const auto& tup : looseMap) {
					const auto* mod = tup.first;
					auto*       miscForm = tup.second;
					if (!mod || miscForm != misc) {
						continue;
					}
					const auto modFid = static_cast<std::uint32_t>(mod->GetFormID());
					if (installed.contains(modFid)) {
						if (dbg) {
							logger::info("  skip {:08X} '{}': already installed",
								modFid, mod->GetFullName() ? mod->GetFullName() : "?");
						}
						continue;
					}
					if (!ModCompatibleWithWeapon(ctxOpt->weapon, ctxOpt->instExtra, mod)) {
						if (dbg) {
							auto* kw = GetModAttachKeyword(mod);
							logger::info("  skip {:08X} '{}': attach kw={} idx={} not compatible",
								modFid, mod->GetFullName() ? mod->GetFullName() : "?",
								kw ? kw->GetFormEditorID() : "null",
								mod->attachPoint.keywordIndex);
						}
						continue;
					}
					const std::uint32_t cnt = a_stack.GetCount();
					if (cnt == 0) {
						continue;
					}
					std::string label;
					if (const auto* full = mod->As<RE::TESFullName>()) {
						if (const char* n = full->fullName.c_str(); n && n[0]) {
							label = n;
						}
					}
					if (label.empty()) {
						label = std::format("{:08X}", modFid);
					}
					if (cnt > 1) {
						label += std::format(" (x{})", cnt);
					}
					if (dbg) {
						logger::info("  found {:08X} '{}' x{}", modFid, label, cnt);
					}
					a_out.push_back(InvLooseModPick{
						.modFormID  = modFid,
						.miscFormID = static_cast<std::uint32_t>(misc->GetFormID()),
						.count      = cnt,
						.label      = std::move(label),
					});
					return true;
				}
				return true;
			});

		std::sort(a_out.begin(), a_out.end(), [](const InvLooseModPick& a, const InvLooseModPick& b) {
			return a.modFormID < b.modFormID;
		});
		std::vector<InvLooseModPick> deduped;
		for (const auto& e : a_out) {
			if (deduped.empty() || deduped.back().modFormID != e.modFormID) {
				deduped.push_back(e);
			} else {
				deduped.back().count += e.count;
			}
		}
		for (auto& e : deduped) {
			std::string label;
			if (auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(e.modFormID)) {
				if (const auto* full = mod->As<RE::TESFullName>()) {
					if (const char* n = full->fullName.c_str(); n && n[0]) {
						label = n;
					}
				}
			}
			if (label.empty()) {
				label = std::format("{:08X}", e.modFormID);
			}
			if (e.count > 1) {
				label += std::format(" (x{})", e.count);
			}
			e.label = std::move(label);
		}
		a_out = std::move(deduped);
		std::sort(a_out.begin(), a_out.end(), [](const InvLooseModPick& a, const InvLooseModPick& b) {
			return a.label < b.label;
		});
	}

	void RemoveLooseMiscFromPlayer(RE::PlayerCharacter* a_player, RE::TESObjectMISC* a_misc, std::int32_t a_count)
	{
		if (!a_player || !a_misc || a_count < 1) {
			return;
		}
		RE::TESObjectREFR::RemoveItemData data{ static_cast<RE::TESBoundObject*>(a_misc), a_count };
		static_cast<RE::TESObjectREFR*>(a_player)->RemoveItem(data);
	}

	bool AttachModToExamineWeapon(RE::ExamineMenu* a_menu, RE::BGSMod::Attachment::Mod* a_mod, std::int8_t a_slotIndex)
	{
		const auto ctxOpt = GetExamineWeaponCtx(a_menu);
		if (!ctxOpt || !a_mod || a_slotIndex < 0) {
			return false;
		}
		bool            success = false;
		RE::BGSInventoryItem::ModifyModDataFunctor modFn(a_mod, a_slotIndex, true, &success);
		modFn.shouldSplitStacks = false;
		RE::BGSInventoryItem::CheckStackIDFunctor idFn(ctxOpt->stackIdx);
		ctxOpt->player->FindAndWriteStackDataForInventoryItem(ctxOpt->weapon, idFn, modFn);
		if (BWS::Settings::Get().debugLogging.load()) {
			logger::info("[BWS] ScrapMod: AttachMod form={:08X} slot={} success={}"sv,
				static_cast<std::uint32_t>(a_mod->GetFormID()), a_slotIndex, success);
		}
		return success;
	}

	void BuildSwapCandidates(RE::ExamineMenu* a_menu, std::uint32_t a_removeModFormID, std::vector<PendingModPick>& a_out)
	{
		a_out.clear();
		const auto ctxOpt = GetExamineWeaponCtx(a_menu);
		auto*    removeMod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_removeModFormID);
		if (!ctxOpt || !removeMod) {
			return;
		}
		std::set<std::uint32_t> installed;
		CollectInstalledModFormIDs(ctxOpt->instExtra, installed);

		auto& looseMap = RE::BGSMod::Attachment::GetAllLooseMods();
		if (!ctxOpt->player->inventoryList) {
			return;
		}

		ctxOpt->player->inventoryList->ForEachStack(
			[&](RE::BGSInventoryItem&) { return true; },
			[&](RE::BGSInventoryItem& a_item, RE::BGSInventoryItem::Stack& a_stack) {
				auto* bound = a_item.object;
				if (!bound) {
					return true;
				}
				auto* misc = bound->As<RE::TESObjectMISC>();
				if (!misc || a_stack.GetCount() == 0) {
					return true;
				}
				for (const auto& tup : looseMap) {
					const auto* candMod = tup.first;
					auto*       miscForm = tup.second;
					if (!candMod || miscForm != misc) {
						continue;
					}
					const auto candFid = static_cast<std::uint32_t>(candMod->GetFormID());
					if (candFid == a_removeModFormID) {
						continue;
					}
					if (installed.contains(candFid)) {
						continue;
					}
					if (!ModSameAttachSlot(candMod, removeMod)) {
						continue;
					}
					if (!ModCompatibleWithWeapon(ctxOpt->weapon, ctxOpt->instExtra, candMod)) {
						continue;
					}
					if (std::find_if(a_out.begin(), a_out.end(), [candFid](const PendingModPick& p) { return p.formID == candFid; }) != a_out.end()) {
						return true;
					}
					PendingModPick pick{};
					pick.formID = candFid;
					if (const auto* full = candMod->As<RE::TESFullName>()) {
						if (const char* n = full->fullName.c_str(); n && n[0]) {
							pick.label = n;
						}
					}
					if (pick.label.empty()) {
						pick.label = std::format("{:08X}", candFid);
					}
					a_out.push_back(std::move(pick));
					return true;
				}
				return true;
			});
	}

	RE::BGSConstructibleObject* FindConstructibleForForm(const RE::TESForm* a_createdItem)
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

	RE::BGSConstructibleObject* FindCobjForMod(const RE::BGSMod::Attachment::Mod* a_mod, const RE::TESObjectMISC* a_loose)
	{
		if (auto* c = FindConstructibleForForm(a_mod)) {
			return c;
		}
		if (a_loose) {
			return FindConstructibleForForm(a_loose);
		}
		return nullptr;
	}

	void BuildIngredientLinesForPick(const PendingModPick& a_pick, std::vector<ModIngredientLine>& a_out)
	{
		a_out.clear();
		auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_pick.formID);
		if (!mod) {
			return;
		}
		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();
		RE::TESObjectMISC* looseItem = nullptr;
		if (auto it = loose.find(mod); it != loose.end()) {
			looseItem = it->second;
		}
		auto* cobj = FindCobjForMod(mod, looseItem);
		if (!cobj || !cobj->requiredItems) {
			return;
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
			a_out.push_back(ModIngredientLine{
				.formID = static_cast<std::uint32_t>(form->GetFormID()),
				.name   = std::move(name),
				.count  = static_cast<std::uint32_t>(qty),
			});
		}
	}

	struct DeferredItem
	{
		std::uint32_t formID;
		std::int32_t  count;
	};

	void GiveScrapComponentsForPick(RE::PlayerCharacter*, const PendingModPick& a_pick)
	{
		auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_pick.formID);
		if (!mod) {
			return;
		}
		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();
		RE::TESObjectMISC* looseItem = nullptr;
		if (auto it = loose.find(mod); it != loose.end()) {
			looseItem = it->second;
		}
		auto* cobj = FindCobjForMod(mod, looseItem);
		if (!cobj || !cobj->requiredItems) {
			return;
		}
		auto items = std::make_shared<std::vector<DeferredItem>>();

		logger::info("[BWS] GiveScrap: COBJ {:08X} has {} ingredients",
			cobj->GetFormID(), cobj->requiredItems->size());

		for (const auto& tup : *cobj->requiredItems) {
			auto* form = tup.first;
			const auto qty = tup.second.i;
			if (!form || qty == 0) {
				continue;
			}

			RE::TESBoundObject* toAdd = nullptr;
			if (auto* comp = form->As<RE::BGSComponent>(); comp && comp->scrapItem) {
				toAdd = comp->scrapItem;
				logger::info("[BWS] GiveScrap: ingredient {:08X} is BGSComponent -> scrapItem {:08X}, qty={}",
					form->GetFormID(), comp->scrapItem->GetFormID(), qty);
			} else {
				toAdd = form->As<RE::TESBoundObject>();
				logger::info("[BWS] GiveScrap: ingredient {:08X} is not BGSComponent, qty={}",
					form->GetFormID(), qty);
			}
			if (!toAdd) {
				continue;
			}
			const std::uint32_t addID = toAdd->GetFormID();

			if (addID != 0) {
				items->push_back(DeferredItem{ addID, static_cast<std::int32_t>(qty) });
			}
		}

		if (items->empty()) {
			return;
		}

		logger::info("[BWS] GiveScrap: queuing {} deferred items to add", items->size());

		const auto* taskIface = F4SE::GetTaskInterface();
		if (taskIface) {
			taskIface->AddTask([items]() {
				auto* pl = RE::PlayerCharacter::GetSingleton();
				if (!pl) {
					logger::warn("[BWS] GiveScrap deferred: no player singleton!");
					return;
				}
				for (const auto& di : *items) {
					auto* bound = RE::TESForm::GetFormByID<RE::TESBoundObject>(di.formID);
					if (bound) {
						logger::info("[BWS] GiveScrap deferred: AddObjectToContainer {:08X} x{}",
							di.formID, di.count);
						pl->AddObjectToContainer(bound, nullptr, di.count, nullptr,
							static_cast<RE::ITEM_REMOVE_REASON>(0));
					} else {
						logger::warn("[BWS] GiveScrap deferred: form {:08X} resolved to null!", di.formID);
					}
				}
				RefreshExamineMenuModList();
				logger::info("[BWS] GiveScrap deferred: done, added {} batches", items->size());
			});
		} else {
			auto* pl = RE::PlayerCharacter::GetSingleton();
			if (pl) {
				for (const auto& di : *items) {
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

	bool RemoveModFromExamineInstance(RE::ExamineMenu* a_menu, std::uint32_t a_modFormID)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !a_menu) {
			return false;
		}

		auto* invItem = reinterpret_cast<RE::BGSInventoryItem*>(
			reinterpret_cast<std::byte*>(a_menu) + kExamineMenu_ModdedInventoryItem);

		auto* weapBase = invItem->object;
		if (!weapBase) {
			return false;
		}

		auto* targetExtra = GetPrimaryInstanceExtraList(invItem);
		if (!targetExtra) {
			return false;
		}

		auto* instExtra = targetExtra->GetByType<RE::BGSObjectInstanceExtra>();
		if (!instExtra) {
			return false;
		}

		auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_modFormID);
		if (!mod) {
			return false;
		}

		const auto slotOpt = FindAttachIndexForMod(instExtra, a_modFormID);
		if (!slotOpt) {
			return false;
		}

		const std::uint32_t stackIdx = ResolveExamineStackIndex(a_menu, weapBase, instExtra);

		bool            success = false;
		RE::BGSInventoryItem::ModifyModDataFunctor modFn(
			mod, static_cast<std::int8_t>(*slotOpt), false, &success);
		modFn.shouldSplitStacks = false;

		RE::BGSInventoryItem::CheckStackIDFunctor idFn(stackIdx);

		player->FindAndWriteStackDataForInventoryItem(weapBase, idFn, modFn);

		if (BWS::Settings::Get().debugLogging.load()) {
			logger::info("[BWS] ScrapMod: RemoveMod form={:08X} slot={} stackIdx={} (modStack field={}) success={}"sv,
				a_modFormID, *slotOpt, stackIdx, GetExamineModStackIndex(a_menu), success);
		}

		return success;
	}

	// How many UI::menuMode increments THIS flow still owes. OpenPicker
	// bumps it; CloseFlow / ForceReleaseInputGuards drains it. Prevents a
	// stranded menuMode from locking player controls after the workbench.
	std::uint32_t g_scrapModMenuModeLayers{ 0 };

	// After ExamineMenu closes, wait for the engine / FallUI to finish
	// restoring PromptMenu before we touch blockPlayerInput. Clearing that
	// flag too early (or without re-showing PromptMenu) left activate /
	// interaction prompts missing for the rest of the session.
	constexpr std::uint32_t kPostExamineHealDelayFrames = 90;
	std::atomic<std::uint32_t> g_postExamineHealFrames{ 0 };

	// Second diagnostic-only pass ~3s after the heal tick, to see whether
	// whatever is hiding the interaction UI ever self-recovers, or is
	// permanently stuck (which points at a latched flag, not a timing race).
	constexpr std::uint32_t kDiagRecheckDelayFrames = 180;
	std::atomic<std::uint32_t> g_diagRecheckFrames{ 0 };

	// ------------------------------------------------------------ HUD mode
	// Crosshair / activate-prompt visibility is gated by the engine's HUD
	// mode stack (SendHUDMessage::Push/PopHUDMode). Every GameMenuBase menu
	// pushes menuHUDMode in OnAddedToMenuStack and pops it in
	// OnRemovedFromMenuStack. If ExamineMenu's pop is ever skipped, the
	// interaction UI stays hidden for the whole session while all menus
	// still report healthy — exactly the observed bug signature.
	//
	// IDs are the old-gen (1.10.163) line, cross-checked against the
	// commonlibf4-main VariantID table where ShowHUDMessage{1163005,...}
	// matches our vendored SendHUDMessage.h REL::ID(1163005).
	void PushHUDModeByName(const RE::HUDModeType& a_mode)
	{
		using func_t = void (*)(const RE::HUDModeType&);
		static REL::Relocation<func_t> func{ REL::ID(1321764) };
		func(a_mode);
	}

	void PopHUDModeByName(const RE::HUDModeType& a_mode)
	{
		using func_t = void (*)(const RE::HUDModeType&);
		static REL::Relocation<func_t> func{ REL::ID(1495042) };
		func(a_mode);
	}

	// menuHUDMode string captured from the closing ExamineMenu, consumed by
	// the recheck tick if the broken-exit signature was detected.
	std::string g_examineHudModeStr;
	// Set when the heal tick found blockPlayerInput still stuck — the
	// reliable marker of a broken exit in every captured log so far.
	std::atomic<bool> g_exitLookedBroken{ false };

	// Not just "is the menu on the stack" (GetMenuOpen/OnStack) — a menu can
	// remain on-stack yet render nothing if passesTopMenuTest / menuCanBeVisible
	// got latched false while ExamineMenu was on top and never recalculated
	// on close. Dumps the live menu stack plus those flags for the two menus
	// most likely to host the crosshair/activation cue (HUDMenu, ButtonBarMenu)
	// so we can tell a "closed" menu apart from an "open but invisible" one.
	void LogMenuDiagnostics(const char* a_tag)
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			logger::warn("[BWS] menu-diag [{}]: no UI singleton"sv, a_tag);
			return;
		}

		std::string stackNames;
		for (const auto& m : ui->menuStack) {
			if (!m) {
				continue;
			}
			if (!stackNames.empty()) {
				stackNames += ", ";
			}
			stackNames += m->menuName.c_str() ? m->menuName.c_str() : "?";
		}
		logger::info("[BWS] menu-diag [{}]: stack=[{}]"sv, a_tag, stackNames);
		// Engine-global pause/visibility counters: any of these latched
		// nonzero after the workbench closes would also suppress HUD parts.
		logger::info(
			"[BWS] menu-diag [{}]: menuMode={} itemMenuMode={} freezeFrameBG={} freezeFramePause={} pauseMenuDisableCt={}"sv,
			a_tag, ui->menuMode, ui->itemMenuMode.load_unchecked(), ui->freezeFrameMenuBG,
			ui->freezeFramePause, ui->pauseMenuDisableCt.load_unchecked());

		const auto dumpMenu = [&](const char* a_name) {
			const RE::BSFixedString fixed{ a_name };
			const auto menu = ui->GetMenu(fixed);
			if (!menu) {
				logger::info("[BWS] menu-diag [{}]: {} = <not registered>"sv, a_tag, a_name);
				return;
			}
			logger::info(
				"[BWS] menu-diag [{}]: {} onStack={} passesTopMenuTest={} menuCanBeVisible={} "
				"displayEnabled={} advanceCt={}"sv,
				a_tag, a_name, menu->OnStack(), menu->passesTopMenuTest, menu->menuCanBeVisible,
				menu->IsMenuDisplayEnabled(), menu->advanceWithoutRenderCount.load_unchecked());
		};
		dumpMenu("HUDMenu");
		dumpMenu("ButtonBarMenu");
		dumpMenu("PromptMenu");
	}

	void ForceReleaseInputGuardsImpl()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			while (g_scrapModMenuModeLayers > 0 && ui->menuMode > 0) {
				ui->menuMode -= 1;
				--g_scrapModMenuModeLayers;
			}
		}
		g_scrapModMenuModeLayers = 0;
		g_phase.store(FlowPhase::kNone, std::memory_order_release);
		if (auto* cm = RE::ControlMap::GetSingleton()) {
			cm->SetIgnoreKeyboardMouse(false);
		}
	}

	void CloseFlow()
	{
		const bool wasOpen = g_phase.load(std::memory_order_acquire) != FlowPhase::kNone;
		g_phase.store(FlowPhase::kNone, std::memory_order_release);
		g_mods.clear();
		g_confirmLines.clear();
		g_weaponName.clear();
		g_invLooseMods.clear();
		g_invLooseConfirmLines.clear();
		g_invLooseConfirmIdx = 0;
		g_swapCandidates.clear();
		g_swapSlotIndex = -1;
		// Always release guards when closing (or when called as a safety net
		// after ExamineMenu exit). Previously we only cleared
		// ignoreKeyboardMouse when phase was still non-None, which could
		// leave the player control-locked if phase was cleared elsewhere.
		ForceReleaseInputGuardsImpl();
	}

	void OpenPickerFromExamine()
	{
		RE::ExamineMenu* menu{};
		{
			std::lock_guard lk(g_examineMtx);
			menu = g_examineMenuPtr;
		}
		if (!menu) {
			return;
		}

		PendingWeaponScrap snap{};
		if (!ScrapManager::BuildPendingFromExamineMenu(menu, snap) || snap.mods.empty()) {
			if (BWS::Settings::Get().showApplyHudMessage.load()) {
				RE::SendHUDMessage::ShowHUDMessage(
					"Better Weapon Scrapping: no removable mods on this weapon.", nullptr, false, false);
			}
			return;
		}

		g_weaponName = snap.weaponDisplayName;
		g_mods = std::move(snap.mods);
		g_invLooseMods.clear();
		g_swapCandidates.clear();
		g_swapSlotIndex = -1;
		g_confirmLines.clear();
		g_phase.store(FlowPhase::kPicker, std::memory_order_release);

		// Do NOT bump UI::menuMode here. ExamineMenu already holds a menuMode
		// layer; adding ours races with workbench teardown and was leaving
		// menuMode > 0 after exit (player fully control-locked). Input block
		// is ignoreKeyboardMouse + WndProc swallow only.
		if (auto* cm = RE::ControlMap::GetSingleton();
			BWS::Settings::Get().blockInputWhilePicker.load()) {
			cm->SetIgnoreKeyboardMouse(true);
		}
		::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
		logger::info("[BWS] scrap-mod picker opened (no menuMode bump)"sv);
	}

	void RefreshModsAfterRemoval(std::uint32_t a_removedFormID = 0)
	{
		if (a_removedFormID != 0) {
			std::erase_if(g_mods, [a_removedFormID](const PendingModPick& m) {
				return m.formID == a_removedFormID;
			});
		}

		RE::ExamineMenu* menu{};
		{
			std::lock_guard lk(g_examineMtx);
			menu = g_examineMenuPtr;
		}
		if (menu) {
			PendingWeaponScrap snap{};
			if (ScrapManager::BuildPendingFromExamineMenu(menu, snap)) {
				g_weaponName = snap.weaponDisplayName;
				auto freshMods = std::move(snap.mods);
				if (a_removedFormID != 0) {
					std::erase_if(freshMods, [a_removedFormID](const PendingModPick& m) {
						return m.formID == a_removedFormID;
					});
				}
				g_mods = std::move(freshMods);
			}
		}

		if (g_mods.empty()) {
			CloseFlow();
			if (BWS::Settings::Get().showApplyHudMessage.load()) {
				RE::SendHUDMessage::ShowHUDMessage(
					"Better Weapon Scrapping: all listed mods removed.", nullptr, false, false);
			}
		} else {
			g_phase.store(FlowPhase::kPicker, std::memory_order_release);
		}
	}

	bool PerformAttachedModScrap(RE::ExamineMenu* a_menu, const PendingModPick& a_pick)
	{
		if (RemoveModFromExamineInstance(a_menu, a_pick.formID)) {
			if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
				GiveScrapComponentsForPick(pl, a_pick);
			}
			if (BWS::Settings::Get().showApplyHudMessage.load()) {
				RE::SendHUDMessage::ShowHUDMessage(
					"Better Weapon Scrapping: mod scrapped.", nullptr, false, false);
			}
			return true;
		}
		return false;
	}

	// Resolves the loose MISC item for a picked mod. Null when the OMOD has
	// no loose-item mapping (default/template mods never reach the picker,
	// so in practice this should always resolve).
	RE::TESObjectMISC* FindLooseItemForPick(const PendingModPick& a_pick)
	{
		auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_pick.formID);
		if (!mod) {
			return nullptr;
		}
		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();
		if (auto it = loose.find(mod); it != loose.end()) {
			return it->second;
		}
		return nullptr;
	}

	// Remove-mode action: detach the picked mod from the examined weapon and
	// return it to the player's inventory as its loose mod item (same grant
	// path as every other BWS grant: deferred to the game thread, followed by
	// a full menu refresh so the workbench UI updates in place).
	bool PerformAttachedModRemove(RE::ExamineMenu* a_menu, const PendingModPick& a_pick)
	{
		auto* looseItem = FindLooseItemForPick(a_pick);
		if (!RemoveModFromExamineInstance(a_menu, a_pick.formID)) {
			return false;
		}

		if (looseItem) {
			const std::uint32_t looseFID = static_cast<std::uint32_t>(looseItem->GetFormID());
			if (const auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask([looseFID]() {
					auto* pl = RE::PlayerCharacter::GetSingleton();
					auto* bound = RE::TESForm::GetFormByID<RE::TESBoundObject>(looseFID);
					if (pl && bound) {
						pl->AddObjectToContainer(bound, nullptr, 1, nullptr,
							static_cast<RE::ITEM_REMOVE_REASON>(0));
					}
					RefreshExamineMenuModList();
				});
			}
		} else {
			logger::warn("[BWS] remove-mode: mod {:08X} '{}' has no loose item — detached without grant"sv,
				a_pick.formID, a_pick.label);
		}

		if (BWS::Settings::Get().showApplyHudMessage.load()) {
			RE::SendHUDMessage::ShowHUDMessage(
				"Better Weapon Scrapping: mod removed to inventory.", nullptr, false, false);
		}
		return true;
	}

	void TryBeginSwapOrScrapAttached(RE::ExamineMenu* a_menu, const PendingModPick& a_pick)
	{
		g_swapCandidates.clear();
		g_swapSlotIndex = -1;

		const auto ctxOpt = GetExamineWeaponCtx(a_menu);
		if (!ctxOpt) {
			PerformAttachedModScrap(a_menu, a_pick);
			RefreshModsAfterRemoval(a_pick.formID);
			return;
		}

		const auto slotOpt = FindAttachIndexForMod(ctxOpt->instExtra, a_pick.formID);
		if (!slotOpt) {
			PerformAttachedModScrap(a_menu, a_pick);
			RefreshModsAfterRemoval(a_pick.formID);
			return;
		}

		g_swapSlotIndex = static_cast<std::int8_t>(*slotOpt);
		BuildSwapCandidates(a_menu, a_pick.formID, g_swapCandidates);

		if (IsExamineStackEquipped(a_menu) && !g_swapCandidates.empty()) {
			g_phase.store(FlowPhase::kSwapSuggest, std::memory_order_release);
		} else {
			PerformAttachedModScrap(a_menu, a_pick);
			RefreshModsAfterRemoval(a_pick.formID);
		}
	}

	void ApplySwapThenScrapAttached(RE::ExamineMenu* a_menu, const PendingModPick& a_oldPick, std::uint32_t a_newModFormID)
	{
		auto* newMod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(a_newModFormID);
		if (!a_menu || !newMod || g_swapSlotIndex < 0) {
			RefreshModsAfterRemoval(a_oldPick.formID);
			return;
		}

		if (!RemoveModFromExamineInstance(a_menu, a_oldPick.formID)) {
			RefreshModsAfterRemoval(a_oldPick.formID);
			return;
		}
		auto* pl = RE::PlayerCharacter::GetSingleton();
		if (pl) {
			GiveScrapComponentsForPick(pl, a_oldPick);
		}
		AttachModToExamineWeapon(a_menu, newMod, g_swapSlotIndex);

		if (pl) {
			auto& looseMap = RE::BGSMod::Attachment::GetAllLooseMods();
			if (auto it = looseMap.find(newMod); it != looseMap.end() && it->second) {
				RemoveLooseMiscFromPlayer(pl, it->second, 1);
			}
		}

		if (BWS::Settings::Get().showApplyHudMessage.load()) {
			RE::SendHUDMessage::ShowHUDMessage(
				"Better Weapon Scrapping: mod replaced and old mod scrapped for parts.", nullptr, false, false);
		}
		g_swapCandidates.clear();
		g_swapSlotIndex = -1;
		RefreshModsAfterRemoval(a_oldPick.formID);
	}

	class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* GetSingleton()
		{
			static MenuSink s;
			return &s;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event.menuName == "ExamineMenu"sv) {
				std::lock_guard lk(g_examineMtx);
				if (a_event.opening) {
					g_examineOpen.store(true, std::memory_order_release);
					const RE::BSFixedString kExamine{ "ExamineMenu" };
					if (const auto m = RE::UI::GetSingleton()->GetMenu(kExamine)) {
						g_examineMenuPtr = reinterpret_cast<RE::ExamineMenu*>(m.get());
					} else {
						g_examineMenuPtr = nullptr;
					}
				} else {
					// Root-caused via diagnostics (not a guess): blockPlayerInput
					// stayed true for ~4s after ExamineMenu closed and interaction
					// prompts never returned even after that, while ButtonBarMenu's
					// own onStack/passesTopMenuTest/menuCanBeVisible flags were
					// healthy the whole time. That means native teardown itself was
					// stalling — most likely waiting on our injected child SWF
					// (BWSExamineMenu.swf, still ticking its own ENTER_FRAME loop
					// and still attached under ExamineMenu's root) to fully unload
					// before it considers the menu's Scaleform resources released.
					// Force-unload it here, synchronously, while ExamineMenu's own
					// Scaleform movie is still guaranteed alive (this IS the event
					// notifying us it's closing) — BEFORE any of our own C++-side
					// teardown, so native cleanup isn't left waiting on us at all.
					if (auto* ui = RE::UI::GetSingleton()) {
						static const RE::BSFixedString kExamine{ "ExamineMenu" };
						if (const auto closingMenu = ui->GetMenu(kExamine)) {
							// Record the HUD mode this menu pushed on open.
							// ExamineMenu derives from GameMenuBase (it has a
							// button bar / shader FX / menuHUDMode); the pop
							// happens in OnRemovedFromMenuStack, and if that
							// is ever skipped the crosshair + activate
							// prompts stay hidden all session.
							auto* gmb = static_cast<RE::GameMenuBase*>(closingMenu.get());
							g_examineHudModeStr.clear();
							if (gmb->menuHUDMode.has_value()) {
								if (const char* s = gmb->menuHUDMode.value().modeString.c_str()) {
									g_examineHudModeStr = s;
								}
							}
							logger::info("[BWS] ExamineMenu closing: menuHUDMode='{}'"sv,
								g_examineHudModeStr.empty() ? "<none>" : g_examineHudModeStr);

							if (closingMenu->uiMovie && closingMenu->uiMovie->asMovieRoot) {
								auto* root = closingMenu->uiMovie->asMovieRoot.get();

								// ROOT CAUSE FIX (bisected 2026-07-22): leaving our
								// AS3 closure installed as BGSCodeObj.ScrapItem (plus
								// the stashed native original in bws_origScrapItem)
								// breaks the menu's native teardown — controls stay
								// blocked, the workbench screen effect lingers, and
								// interaction prompts never come back. Confirmed by
								// bisect: wrap on -> broken exit, wrap off -> healthy.
								// So put the code object back EXACTLY as the game
								// built it before teardown runs: restore the original
								// native ScrapItem and drop our extra member.
								RE::Scaleform::GFx::Value codeObj;
								if (root->GetVariable(std::addressof(codeObj), "root.BaseInstance.BGSCodeObj") &&
									codeObj.IsObject()) {
									RE::Scaleform::GFx::Value orig;
									if (codeObj.GetMember("bws_origScrapItem", std::addressof(orig)) &&
										!orig.IsUndefined()) {
										codeObj.SetMember("ScrapItem", orig);
										// No DeleteMember in this GFx wrapper; undefined
										// releases the held reference, which is what matters.
										codeObj.SetMember("bws_origScrapItem", RE::Scaleform::GFx::Value{});
										logger::info("[BWS] ExamineMenu closing: restored native ScrapItem on BGSCodeObj"sv);
									}
								}

								const bool unloaded = root->Invoke("root.bws_loader.unload", nullptr, nullptr, 0);
								logger::info("[BWS] ExamineMenu closing: bws_loader.unload() -> {}"sv, unloaded);
							}
						} else {
							logger::info("[BWS] ExamineMenu closing: menu already gone from map"sv);
						}
					}
					g_exitLookedBroken.store(false, std::memory_order_release);

					g_examineOpen.store(false, std::memory_order_release);
					g_examineMenuPtr = nullptr;
					// Do NOT clear blockPlayerInput here — it is normally true
					// during this event while ButtonBarMenu finishes teardown.
					// Clearing it immediately freed WASD but left activate /
					// interaction prompts hidden for the rest of the session
					// (verified: ButtonBarMenu still open at this exact moment).
					CloseFlow();
					ForceReleaseInputGuardsImpl();
					ScrapOverlay::ForceDismiss();
					BWS::ExamineMenuBridge::ClearStagedItems();

					std::uint32_t menuMode = 0;
					bool          ignore = false;
					bool          blockInput = false;
					bool          buttonBarOpen = false;

					if (auto* ui = RE::UI::GetSingleton()) {
						menuMode = ui->menuMode;
						static const RE::BSFixedString kButtonBar{ "ButtonBarMenu" };
						buttonBarOpen = ui->GetMenuOpen(kButtonBar);
					}
					if (auto* cm = RE::ControlMap::GetSingleton()) {
						ignore = cm->ignoreKeyboardMouse;
					}
					if (auto* pc = RE::PlayerControls::GetSingleton()) {
						blockInput = pc->blockPlayerInput;
					}

					logger::info(
						"[BWS] ExamineMenu closed — menuMode={} ignore={} blockPlayerInput={} "
						"ButtonBarMenu={} (deferring blockPlayerInput heal)"sv,
						menuMode, ignore, blockInput, buttonBarOpen);

					if (ignore) {
						if (auto* cm = RE::ControlMap::GetSingleton()) {
							cm->SetIgnoreKeyboardMouse(false);
						}
					}
					if (::GetCapture()) {
						::ReleaseCapture();
					}

					LogMenuDiagnostics("close");

					// Arm deferred heal — TickPostExamineInputHeal clears a
					// stuck blockPlayerInput only after the engine has finished
					// restoring ButtonBarMenu / activate prompts.
					g_postExamineHealFrames.store(kPostExamineHealDelayFrames, std::memory_order_release);
					g_diagRecheckFrames.store(kDiagRecheckDelayFrames, std::memory_order_release);

					spdlog::default_logger()->flush();
				}
			}
			if (a_event.menuName == "ExamineConfirmMenu"sv) {
				g_examineConfirmOpen.store(a_event.opening, std::memory_order_release);
				if (!a_event.opening) {
					// The native confirm closed (accepted OR cancelled). On
					// accept the game already granted from its own copy of
					// scrappingArray; on cancel the staged items must not
					// leak into an unrelated later scrap. Either way, drop
					// leftovers and clear the in-flight flag so the next
					// SCRAP press is intercepted again.
					BWS::ExamineMenuBridge::ClearStagedItems();
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};
}

namespace BWS::ScrapModManager
{
	std::optional<std::uint32_t> FindPlayerStackIndexForInstance(
		RE::TESBoundObject* a_base, const RE::BGSObjectInstanceExtra* a_snapshot)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->inventoryList || !a_base || !a_snapshot) {
			return std::nullopt;
		}
		const auto snap = a_snapshot->GetIndexData();

		std::optional<std::uint32_t> found;
		std::uint32_t                idx = 0;
		player->inventoryList->ForEachStack(
			[&](RE::BGSInventoryItem& a_item) {
				return a_item.object == a_base;
			},
			[&](RE::BGSInventoryItem&, RE::BGSInventoryItem::Stack& a_stack) {
				const std::uint32_t thisIdx = idx++;
				const auto*         xList = a_stack.extra.get();
				const auto*         inst = xList ? xList->GetByType<RE::BGSObjectInstanceExtra>() : nullptr;
				if (inst) {
					const auto data = inst->GetIndexData();
					// Exact content match: same OID entries in the same order
					// (ObjectIndexData is a POD of objectID/index/rank/disabled,
					// so a byte compare of the spans is a full equality check).
					if (data.size() == snap.size() &&
						std::memcmp(data.data(), snap.data(), data.size_bytes()) == 0) {
						found = thisIdx;
						return false;
					}
				}
				return true;
			});
		return found;
	}

	void Install()
	{
		static std::atomic<bool> s_registered{ false };
		if (s_registered.exchange(true)) {
			return;
		}
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
			logger::info("Better Weapon Scrapping: ScrapMod MenuOpenCloseEvent sink registered."sv);
		}
	}

	bool BlocksGameInput()
	{
		return g_phase.load(std::memory_order_acquire) != FlowPhase::kNone;
	}

	void ForceClose()
	{
		CloseFlow();
	}

	bool IsExamineMenuOpen()
	{
		return g_examineOpen.load(std::memory_order_acquire);
	}

	void ForceReleaseInputGuards()
	{
		ForceReleaseInputGuardsImpl();
	}

	void TickPostExamineInputHeal()
	{
		// Diagnostic-only recheck, independent of the heal countdown below,
		// so we can see whether the hidden-UI state ever self-clears.
		std::uint32_t diagFrames = g_diagRecheckFrames.load(std::memory_order_acquire);
		if (diagFrames > 0) {
			if (g_examineOpen.load(std::memory_order_acquire)) {
				g_diagRecheckFrames.store(0, std::memory_order_release);
			} else {
				--diagFrames;
				g_diagRecheckFrames.store(diagFrames, std::memory_order_release);
				if (diagFrames == 0) {
					LogMenuDiagnostics("recheck+3s");
					// If this exit showed the broken signature (blockPlayerInput
					// still stuck at heal time), assume ExamineMenu's HUD-mode
					// pop was skipped too and re-pop it. Restores crosshair /
					// activate prompts if the stuck-HUD-mode diagnosis is
					// right; popping an already-popped mode is a no-op
					// (removes nothing from the mode stack).
					if (g_exitLookedBroken.load(std::memory_order_acquire) &&
						!g_examineHudModeStr.empty()) {
						RE::HUDModeType mode{ RE::BSFixedString(g_examineHudModeStr.c_str()) };
						PopHUDModeByName(mode);
						logger::warn("[BWS] recheck: exit looked broken — popped HUD mode '{}' to restore interaction UI"sv,
							g_examineHudModeStr);
					}
					spdlog::default_logger()->flush();
				}
			}
		}

		std::uint32_t frames = g_postExamineHealFrames.load(std::memory_order_acquire);
		if (frames == 0) {
			return;
		}
		if (g_examineOpen.load(std::memory_order_acquire)) {
			g_postExamineHealFrames.store(0, std::memory_order_release);
			return;
		}

		--frames;
		g_postExamineHealFrames.store(frames, std::memory_order_release);
		if (frames > 0) {
			return;
		}

		auto* ui = RE::UI::GetSingleton();
		auto* pc = RE::PlayerControls::GetSingleton();
		static const RE::BSFixedString kPrompt{ "PromptMenu" };
		static const RE::BSFixedString kButtonBar{ "ButtonBarMenu" };

		const bool promptOpen = ui && ui->GetMenuOpen(kPrompt);
		const bool buttonBarOpen = ui && ui->GetMenuOpen(kButtonBar);
		const bool blocked = pc && pc->blockPlayerInput;

		logger::info(
			"[BWS] post-Examine heal tick — blockPlayerInput={} PromptMenu={} ButtonBarMenu={}"sv,
			blocked, promptOpen, buttonBarOpen);
		LogMenuDiagnostics("heal-tick");

		if (!blocked) {
			// Controls already free. If FallUI's PromptMenu is still down,
			// bring it back without touching player input flags.
			if (ui && !promptOpen) {
				if (auto* q = RE::UIMessageQueue::GetSingleton()) {
					q->AddMessage(kPrompt, RE::UI_MESSAGE_TYPE::kShow);
					logger::warn("[BWS] post-Examine heal: re-showing PromptMenu (controls already free)"sv);
				}
			}
			spdlog::default_logger()->flush();
			return;
		}

		// Stuck controls: restore FallUI PromptMenu FIRST, then clear the flag.
		// Clearing blockPlayerInput alone was leaving activate prompts dead.
		if (auto* q = RE::UIMessageQueue::GetSingleton()) {
			if (!promptOpen) {
				q->AddMessage(kPrompt, RE::UI_MESSAGE_TYPE::kShow);
				logger::warn("[BWS] post-Examine heal: showing PromptMenu before clearing blockPlayerInput"sv);
			}
		}

		// Stuck blockPlayerInput at this point is the reliable marker of a
		// broken exit — remember it so the +3s recheck can also re-pop the
		// menu's HUD mode (crosshair / activate prompt restore).
		g_exitLookedBroken.store(true, std::memory_order_release);

		pc->blockPlayerInput = false;
		logger::warn("[BWS] post-Examine heal: cleared stuck PlayerControls::blockPlayerInput"sv);
		spdlog::default_logger()->flush();
	}

	bool ShouldShowNativeHint()
	{
		// Same gating the old ImGui hint used: feature on, workbench open on
		// a weapon with instance data, no native confirm dialog up, and no
		// picker flow already running. Called per-frame from the Scaleform
		// advance thread by the injected SWF.
		if (!BWS::Settings::Get().enableScrapMod.load() || !BWS::Settings::Get().masterEnabled.load()) {
			return false;
		}
		if (!g_examineOpen.load(std::memory_order_acquire) ||
			g_examineConfirmOpen.load(std::memory_order_acquire) ||
			g_phase.load(std::memory_order_acquire) != FlowPhase::kNone) {
			return false;
		}
		RE::ExamineMenu* menu = nullptr;
		{
			std::lock_guard lk(g_examineMtx);
			menu = g_examineMenuPtr;
		}
		return menu && GetExamineWeaponCtx(menu).has_value();
	}

	void OpenPickerFromExternal()
	{
		if (!BWS::Settings::Get().enableScrapMod.load() || !BWS::Settings::Get().masterEnabled.load()) {
			return;
		}
		if (!g_examineOpen.load(std::memory_order_acquire) ||
			g_phase.load(std::memory_order_acquire) != FlowPhase::kNone) {
			return;
		}
		OpenPickerFromExamine();
	}

	bool TryHotkey(std::uintptr_t a_vk)
	{
		if (!BWS::Settings::Get().enableScrapMod.load() || !BWS::Settings::Get().masterEnabled.load()) {
			return false;
		}
		if (!g_examineOpen.load(std::memory_order_acquire)) {
			return false;
		}
		if (g_phase.load(std::memory_order_acquire) != FlowPhase::kNone) {
			return false;
		}

		const int vk = BWS::Settings::Get().scrapModHotkey.load();
		if (vk <= 0 || static_cast<int>(a_vk) != vk) {
			return false;
		}

		OpenPickerFromExamine();
		return true;
	}

	void Draw()
	{
		if (!BWS::Settings::Get().enableScrapMod.load() || !BWS::Settings::Get().masterEnabled.load()) {
			return;
		}

		// Safety net: if the workbench closed but our ImGui flow / input block
		// is still active, WndProc would keep swallowing all keyboard/mouse
		// and the player stays locked. Clear it immediately.
		if (!g_examineOpen.load(std::memory_order_acquire) &&
			g_phase.load(std::memory_order_acquire) != FlowPhase::kNone) {
			logger::warn("[BWS] scrap-mod UI stranded after ExamineMenu close — forcing release"sv);
			CloseFlow();
			return;
		}

		// One-shot focus grab per flow open (see SetNextWindowFocus below).
		static bool s_focusApplied = false;

		const auto phase = g_phase.load(std::memory_order_acquire);
		if (phase == FlowPhase::kNone) {
			s_focusApplied = false;
			return;
		}

		ImGuiIO& io = ImGui::GetIO();
		io.MouseDrawCursor = true;

		const float sc = io.DisplaySize.y / 1080.0f;
		const float btnW = std::round(140.0f * sc);

		const ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
		ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

		// Focus ONCE when the flow opens — never per-frame. A per-frame
		// SetNextWindowFocus re-focuses the parent window every frame, which
		// rips gamepad/keyboard nav back out of child windows (the mod list)
		// the instant the user navigates into them: the child highlights its
		// first item for one frame, then focus snaps back to the parent.
		if (!s_focusApplied) {
			ImGui::SetNextWindowFocus();
			s_focusApplied = true;
		}

		const float winW = std::round(620.0f * sc);
		ImGui::SetNextWindowSize(ImVec2(winW, 0.0f), ImGuiCond_Always);

		ImGui::Begin(
			"##bws_scrap_mod",
			nullptr,
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_AlwaysAutoResize);

		// True while gamepad/keyboard nav focus sits INSIDE one of this
		// window's child lists (mod list, swap list, ...). ImGui's built-in
		// nav-cancel (B) already pops focus from a child back to the parent
		// window — our own B handler below must stay quiet on that press,
		// otherwise one press would exit the list AND back out of the menu.
		const bool navInChildList =
			ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsWindowFocused();

		const float listH = std::min(std::round(400.0f * sc), io.DisplaySize.y * 0.45f);
		const float swapH = std::min(std::round(240.0f * sc), io.DisplaySize.y * 0.3f);

		if (phase == FlowPhase::kPicker) {
			ImGui::TextUnformatted(g_removeMode
			                           ? "Remove Mod \xe2\x80\x94 Select a Mod"
			                           : "Scrap Mod \xe2\x80\x94 Select a Mod");
			ImGui::Separator();
			ImGui::TextWrapped("%s", g_weaponName.c_str());
			ImGui::Spacing();

			// Mode toggle: Scrap (grant components) vs Remove (return the
			// loose mod item to the inventory). Session-persistent.
			ImGui::Checkbox("Remove mode", &g_removeMode);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Checked: selected mods are detached and returned to your inventory as loose mods.\n"
					"Unchecked: selected mods are detached and scrapped for crafting components.");
			}
			ImGui::SameLine();
			ImGui::TextDisabled(g_removeMode
			                        ? "\xe2\x80\x94 detach and return the loose mod to your inventory"
			                        : "\xe2\x80\x94 detach and scrap the mod for components");
			ImGui::Spacing();

			ImGui::TextUnformatted(g_removeMode
			                           ? "Mods on this weapon (removes from weapon, loose mod goes to inventory):"
			                           : "Mods on this weapon (removes from weapon and grants components):");
			ImGui::BeginChild("##mod_list", ImVec2(-FLT_MIN, listH), ImGuiChildFlags_Borders);
			for (std::size_t i = 0; i < g_mods.size(); ++i) {
				const char* label = g_mods[i].label.empty() ? "(mod)" : g_mods[i].label.c_str();
				ImGui::PushID(static_cast<int>(i));
				if (ImGui::Button(label, ImVec2(-FLT_MIN, 0))) {
					g_confirmIndex = i;
					if (g_removeMode) {
						// Remove mode shows the loose item the player will
						// get back instead of component yields.
						g_confirmLines.clear();
						g_confirmLooseLabel.clear();
						if (auto* looseItem = FindLooseItemForPick(g_mods[i])) {
							if (const auto* full = looseItem->As<RE::TESFullName>()) {
								if (const char* n = full->fullName.c_str(); n && n[0]) {
									g_confirmLooseLabel = n;
								}
							}
						}
						if (g_confirmLooseLabel.empty()) {
							g_confirmLooseLabel = g_mods[i].label;
						}
					} else {
						BuildIngredientLinesForPick(g_mods[i], g_confirmLines);
					}
					g_phase.store(FlowPhase::kConfirm, std::memory_order_release);
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::Spacing();
			RE::ExamineMenu* menu{};
			{
				std::lock_guard lk(g_examineMtx);
				menu = g_examineMenuPtr;
			}
			if (menu && ImGui::Button("Scrap unequipped mods from inventory", ImVec2(-FLT_MIN, 0))) {
				CollectInventoryLooseModsForWeapon(menu, g_invLooseMods);
				if (g_invLooseMods.empty()) {
					if (BWS::Settings::Get().showApplyHudMessage.load()) {
						RE::SendHUDMessage::ShowHUDMessage(
							"Better Weapon Scrapping: no compatible loose mods in inventory for this weapon.",
							nullptr, false, false);
					}
				} else {
					g_phase.store(FlowPhase::kInvLoosePicker, std::memory_order_release);
				}
			}
			ImGui::Spacing();
			if (ImGui::Button("Close", ImVec2(btnW, 0))) {
				CloseFlow();
			}
		} else if (phase == FlowPhase::kInvLoosePicker) {
			ImGui::TextUnformatted("Scrap Loose Mods from Inventory");
			ImGui::Separator();
			ImGui::TextWrapped("%s", g_weaponName.c_str());
			ImGui::Spacing();
			ImGui::TextWrapped(
				"These object mods are in your inventory (not installed on this weapon). Scrapping removes the loose mod item and grants components.");
			ImGui::BeginChild("##inv_loose_list", ImVec2(-FLT_MIN, listH), ImGuiChildFlags_Borders);
			for (std::size_t i = 0; i < g_invLooseMods.size(); ++i) {
				ImGui::PushID(static_cast<int>(i + 9000));
				if (ImGui::Button(g_invLooseMods[i].label.c_str(), ImVec2(-FLT_MIN, 0))) {
					g_invLooseConfirmIdx = i;
					PendingModPick tmp{};
					tmp.formID = g_invLooseMods[i].modFormID;
					tmp.label = g_invLooseMods[i].label;
					BuildIngredientLinesForPick(tmp, g_invLooseConfirmLines);
					g_phase.store(FlowPhase::kInvLooseConfirm, std::memory_order_release);
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::Spacing();
			if (ImGui::Button("Back", ImVec2(btnW, 0))) {
				g_phase.store(FlowPhase::kPicker, std::memory_order_release);
			}
		} else if (phase == FlowPhase::kInvLooseConfirm) {
			if (g_invLooseConfirmIdx >= g_invLooseMods.size()) {
				g_phase.store(FlowPhase::kInvLoosePicker, std::memory_order_release);
			} else {
				const auto& invPick = g_invLooseMods[g_invLooseConfirmIdx];
				ImGui::TextUnformatted("Confirm Scrap Loose Mod");
				ImGui::Separator();
				ImGui::TextWrapped("Scrap %s from inventory?", invPick.label.c_str());
				ImGui::Spacing();
				ImGui::TextUnformatted("You will receive (per mod scrapped):");
				if (g_invLooseConfirmLines.empty()) {
					ImGui::TextDisabled("(no COBJ ingredients found \xe2\x80\x94 item still removed)");
				} else {
					for (const auto& line : g_invLooseConfirmLines) {
						ImGui::BulletText("%u x %s", line.count, line.name.c_str());
					}
				}
				ImGui::Spacing();
				if (ImGui::Button("Confirm", ImVec2(btnW, 0))) {
					RE::ExamineMenu* menu{};
					{
						std::lock_guard lk(g_examineMtx);
						menu = g_examineMenuPtr;
					}
					auto* pl = RE::PlayerCharacter::GetSingleton();
					auto* misc = RE::TESForm::GetFormByID<RE::TESObjectMISC>(invPick.miscFormID);
					if (pl && misc && invPick.count > 0) {
						PendingModPick modPick{};
						modPick.formID = invPick.modFormID;
						modPick.label = invPick.label;
						RemoveLooseMiscFromPlayer(pl, misc, static_cast<std::int32_t>(invPick.count));
						for (std::uint32_t c = 0; c < invPick.count; ++c) {
							GiveScrapComponentsForPick(pl, modPick);
						}
						if (BWS::Settings::Get().showApplyHudMessage.load()) {
							RE::SendHUDMessage::ShowHUDMessage(
								"Better Weapon Scrapping: loose mod(s) scrapped.", nullptr, false, false);
						}
					}
					if (menu) {
						CollectInventoryLooseModsForWeapon(menu, g_invLooseMods);
					}
					if (g_invLooseMods.empty()) {
						g_phase.store(FlowPhase::kPicker, std::memory_order_release);
					} else {
						g_phase.store(FlowPhase::kInvLoosePicker, std::memory_order_release);
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Back", ImVec2(btnW, 0))) {
					g_phase.store(FlowPhase::kInvLoosePicker, std::memory_order_release);
				}
			}
		} else if (phase == FlowPhase::kSwapSuggest) {
			if (g_confirmIndex >= g_mods.size() || g_swapSlotIndex < 0) {
				g_phase.store(FlowPhase::kConfirm, std::memory_order_release);
			} else {
				const auto& pick = g_mods[g_confirmIndex];
				ImGui::TextUnformatted("Equipped Weapon \xe2\x80\x94 Replace Mod Before Scrapping?");
				ImGui::Separator();
				ImGui::TextWrapped(
					"This item matches your equipped weapon instance. You can equip another mod from inventory for the same slot, then scrap %s for parts.",
					pick.label.empty() ? "(mod)" : pick.label.c_str());
				ImGui::Spacing();
				ImGui::TextUnformatted("Replacement from inventory:");
				ImGui::BeginChild("##swap_list", ImVec2(-FLT_MIN, swapH), ImGuiChildFlags_Borders);
				for (std::size_t si = 0; si < g_swapCandidates.size(); ++si) {
					const auto& cand = g_swapCandidates[si];
					ImGui::PushID(static_cast<int>(si + 5000));
					if (ImGui::Button(cand.label.empty() ? "(mod)" : cand.label.c_str(), ImVec2(-FLT_MIN, 0))) {
						RE::ExamineMenu* menu{};
						{
							std::lock_guard lk(g_examineMtx);
							menu = g_examineMenuPtr;
						}
						if (menu) {
							ApplySwapThenScrapAttached(menu, pick, cand.formID);
						}
					}
					ImGui::PopID();
				}
				ImGui::EndChild();
				ImGui::Spacing();
				if (ImGui::Button("No replacement \xe2\x80\x94 scrap only", ImVec2(-FLT_MIN, 0))) {
					RE::ExamineMenu* menu{};
					{
						std::lock_guard lk(g_examineMtx);
						menu = g_examineMenuPtr;
					}
					if (menu) {
						PerformAttachedModScrap(menu, pick);
						RefreshModsAfterRemoval(pick.formID);
					}
					g_swapCandidates.clear();
					g_swapSlotIndex = -1;
				}
				ImGui::Spacing();
				if (ImGui::Button("Back", ImVec2(btnW, 0))) {
					g_swapCandidates.clear();
					g_swapSlotIndex = -1;
					g_phase.store(FlowPhase::kConfirm, std::memory_order_release);
				}
			}
		} else if (phase == FlowPhase::kConfirm) {
			if (g_confirmIndex >= g_mods.size()) {
				g_phase.store(FlowPhase::kPicker, std::memory_order_release);
			} else if (g_removeMode) {
				// Remove mode: detach the mod and return the loose item.
				const auto& pick = g_mods[g_confirmIndex];
				ImGui::TextUnformatted("Confirm Remove Mod");
				ImGui::Separator();
				ImGui::TextWrapped("Remove %s from this weapon?", pick.label.empty() ? "(mod)" : pick.label.c_str());
				ImGui::Spacing();
				ImGui::TextUnformatted("You will receive:");
				ImGui::BulletText("1 x %s", g_confirmLooseLabel.empty() ? "(loose mod)" : g_confirmLooseLabel.c_str());
				ImGui::Spacing();
				if (ImGui::Button("Confirm", ImVec2(btnW, 0))) {
					RE::ExamineMenu* menu{};
					{
						std::lock_guard lk(g_examineMtx);
						menu = g_examineMenuPtr;
					}
					if (menu) {
						// No swap-suggest step here: the player gets the mod
						// back intact, so there is nothing to preserve.
						PerformAttachedModRemove(menu, pick);
						RefreshModsAfterRemoval(pick.formID);
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Back", ImVec2(btnW, 0))) {
					g_phase.store(FlowPhase::kPicker, std::memory_order_release);
				}
			} else {
				const auto& pick = g_mods[g_confirmIndex];
				ImGui::TextUnformatted("Confirm Scrap Mod");
				ImGui::Separator();
				ImGui::TextWrapped("Scrap %s?", pick.label.empty() ? "(mod)" : pick.label.c_str());
				ImGui::Spacing();
				ImGui::TextUnformatted("You will receive:");
				if (g_confirmLines.empty()) {
					ImGui::TextDisabled("(no COBJ ingredients found \xe2\x80\x94 mod still removed)");
				} else {
					for (const auto& line : g_confirmLines) {
						ImGui::BulletText("%u x %s", line.count, line.name.c_str());
					}
				}
				ImGui::Spacing();
				if (ImGui::Button("Confirm", ImVec2(btnW, 0))) {
					RE::ExamineMenu* menu{};
					{
						std::lock_guard lk(g_examineMtx);
						menu = g_examineMenuPtr;
					}
					if (menu) {
						TryBeginSwapOrScrapAttached(menu, pick);
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Back", ImVec2(btnW, 0))) {
					g_phase.store(FlowPhase::kPicker, std::memory_order_release);
				}
			}
		}

		// Gamepad B (or keyboard Backspace via ImGui nav) = back one step /
		// close, mirroring the on-screen Back/Close buttons so a controller
		// never gets stuck in the flow. Skipped while nav focus is inside a
		// child list: that press only pops focus back to this window (ImGui
		// built-in); the NEXT press backs out of the phase / closes.
		if (!navInChildList && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
			switch (phase) {
			case FlowPhase::kConfirm:
			case FlowPhase::kSwapSuggest:
				g_swapCandidates.clear();
				g_swapSlotIndex = -1;
				g_phase.store(FlowPhase::kPicker, std::memory_order_release);
				break;
			case FlowPhase::kInvLooseConfirm:
				g_phase.store(FlowPhase::kInvLoosePicker, std::memory_order_release);
				break;
			case FlowPhase::kInvLoosePicker:
				g_phase.store(FlowPhase::kPicker, std::memory_order_release);
				break;
			case FlowPhase::kPicker:
			default:
				CloseFlow();
				break;
			}
		}

		ImGui::End();
	}
}
