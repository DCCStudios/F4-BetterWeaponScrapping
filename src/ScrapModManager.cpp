#include "PCH.h"
#include "BenchScrapTypes.h"
#include "ExamineMenuBridge.h"
#include "ScrapModManager.h"
#include "ScrapManager.h"
#include "Settings.h"

#include "RE/Bethesda/BGSInventoryItem.h"
#include "RE/Bethesda/BGSMod.h"
#include "RE/Bethesda/BSExtraData.h"
#include "RE/Bethesda/BSFixedString.h"
#include "RE/Bethesda/Events.h"
#include "RE/Bethesda/ControlMap.h"
#include "RE/Bethesda/FormComponents.h"
#include "RE/Bethesda/TESBoundObjects.h"
#include "RE/Bethesda/TESDataHandler.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/TESObjectREFRs.h"
#include "RE/Bethesda/UI.h"
#include "RE/Bethesda/UIMessage.h"
#include "RE/Bethesda/UIMessageQueue.h"
#include "RE/Bethesda/SendHUDMessage.h"

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

		constexpr std::size_t kVIdx_UpdateMenu = 0x15;
		reinterpret_cast<VoidFn>(vtbl[kVIdx_UpdateMenu])(raw);

		constexpr std::size_t kVIdx_UpdateModChoiceList = 0x3F;
		reinterpret_cast<VoidFn>(vtbl[kVIdx_UpdateModChoiceList])(raw);

		logger::info("[BWS] RefreshExamineMenuModList: called UpdateMenu + UpdateModChoiceList");
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

	std::uint32_t FindStackIndexForExtra(RE::BGSInventoryItem* a_inv, const RE::ExtraDataList* a_target)
	{
		if (!a_inv || !a_target) {
			return 0;
		}
		std::uint32_t idx = 0;
		for (auto* stack = a_inv->stackData.get(); stack; stack = stack->nextStack.get(), ++idx) {
			if (stack->extra.get() == a_target) {
				return idx;
			}
		}
		return 0;
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
		const std::uint32_t stackIdx = FindStackIndexForExtra(invItem, targetExtra);
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
		if (!ctxOpt) {
			return false;
		}
		const auto* targetExtra = GetPrimaryInstanceExtraList(ctxOpt->invItem);
		if (!targetExtra || !ctxOpt->player->inventoryList) {
			return false;
		}
		bool equipped = false;
		ctxOpt->player->inventoryList->ForEachStack(
			[&](RE::BGSInventoryItem& a_item) {
				return a_item.object == ctxOpt->weapon;
			},
			[&](RE::BGSInventoryItem&, RE::BGSInventoryItem::Stack& a_stack) {
				if (a_stack.extra.get() == targetExtra && a_stack.IsEquipped()) {
					equipped = true;
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

		const std::uint32_t stackIdx = FindStackIndexForExtra(invItem, targetExtra);

		bool            success = false;
		RE::BGSInventoryItem::ModifyModDataFunctor modFn(
			mod, static_cast<std::int8_t>(*slotOpt), false, &success);
		modFn.shouldSplitStacks = false;

		RE::BGSInventoryItem::CheckStackIDFunctor idFn(stackIdx);

		player->FindAndWriteStackDataForInventoryItem(weapBase, idFn, modFn);

		if (BWS::Settings::Get().debugLogging.load()) {
			logger::info("[BWS] ScrapMod: RemoveMod form={:08X} slot={} success={}"sv,
				a_modFormID, *slotOpt, success);
		}

		return success;
	}

	void CloseFlow()
	{
		if (g_phase.load(std::memory_order_acquire) != FlowPhase::kNone) {
			if (auto* ui = RE::UI::GetSingleton(); ui && ui->menuMode > 0) {
				ui->menuMode -= 1;
			}
			if (auto* cm = RE::ControlMap::GetSingleton();
				BWS::Settings::Get().blockInputWhilePicker.load()) {
				cm->SetIgnoreKeyboardMouse(false);
			}
		}
		g_phase.store(FlowPhase::kNone, std::memory_order_release);
		g_mods.clear();
		g_confirmLines.clear();
		g_weaponName.clear();
		g_invLooseMods.clear();
		g_invLooseConfirmLines.clear();
		g_invLooseConfirmIdx = 0;
		g_swapCandidates.clear();
		g_swapSlotIndex = -1;
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

		if (auto* ui = RE::UI::GetSingleton()) {
			ui->menuMode += 1;
		}
		if (auto* cm = RE::ControlMap::GetSingleton();
			BWS::Settings::Get().blockInputWhilePicker.load()) {
			cm->SetIgnoreKeyboardMouse(true);
		}
		::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
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
					g_examineOpen.store(false, std::memory_order_release);
					g_examineMenuPtr = nullptr;
					if (g_phase.load(std::memory_order_acquire) != FlowPhase::kNone) {
						CloseFlow();
					}
					// Leaving the workbench entirely: no vanilla scrap can
					// consume staged recovery items anymore.
					BWS::ExamineMenuBridge::ClearStagedItems();
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

		// The "SCRAP MODS" hotkey hint is no longer drawn here: it now lives
		// in the game's own workbench button bar, appended by the injected
		// BWSExamineMenu.swf (see ExamineMenuBridge.cpp + ShouldShowNativeHint
		// below). Only the picker/confirm windows remain ImGui.

		const auto phase = g_phase.load(std::memory_order_acquire);
		if (phase == FlowPhase::kNone) {
			return;
		}

		ImGuiIO& io = ImGui::GetIO();
		io.MouseDrawCursor = true;

		const float sc = io.DisplaySize.y / 1080.0f;
		const float btnW = std::round(140.0f * sc);

		const ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
		ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowFocus();

		const float winW = std::round(620.0f * sc);
		ImGui::SetNextWindowSize(ImVec2(winW, 0.0f), ImGuiCond_Always);

		ImGui::Begin(
			"##bws_scrap_mod",
			nullptr,
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_AlwaysAutoResize);

		const float listH = std::min(std::round(400.0f * sc), io.DisplaySize.y * 0.45f);
		const float swapH = std::min(std::round(240.0f * sc), io.DisplaySize.y * 0.3f);

		if (phase == FlowPhase::kPicker) {
			ImGui::TextUnformatted("Scrap Mod \xe2\x80\x94 Select a Mod");
			ImGui::Separator();
			ImGui::TextWrapped("%s", g_weaponName.c_str());
			ImGui::Spacing();
			ImGui::TextUnformatted("Mods on this weapon (removes from weapon and grants components):");
			ImGui::BeginChild("##mod_list", ImVec2(-FLT_MIN, listH), ImGuiChildFlags_Borders);
			for (std::size_t i = 0; i < g_mods.size(); ++i) {
				const char* label = g_mods[i].label.empty() ? "(mod)" : g_mods[i].label.c_str();
				ImGui::PushID(static_cast<int>(i));
				if (ImGui::Button(label, ImVec2(-FLT_MIN, 0))) {
					g_confirmIndex = i;
					BuildIngredientLinesForPick(g_mods[i], g_confirmLines);
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

		ImGui::End();
	}
}
