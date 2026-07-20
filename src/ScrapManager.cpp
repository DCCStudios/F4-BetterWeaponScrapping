#include "PCH.h"
#include "BenchScrapTypes.h"
#include "ExamineMenuBridge.h"
#include "ScrapManager.h"
#include "ScrapOverlay.h"
#include "Settings.h"

#include "RE/Bethesda/BGSInventoryItem.h"
#include "RE/Bethesda/BGSMod.h"
#include "RE/Bethesda/BSExtraData.h"
#include "RE/Bethesda/TESBoundObjects.h"
#include "RE/Bethesda/TESDataHandler.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/VTABLE_IDs.h"
#include "REL/Relocation.h"

#include <algorithm>
#include <format>

namespace
{
	using ScrapOnAcceptFn = void (*)(void* a_self);

	ScrapOnAcceptFn g_scrapOnAcceptOriginal{ nullptr };

	static std::string ModDisplayName(const RE::BGSMod::Attachment::Mod* a_mod, std::uint32_t a_fid)
	{
		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();
		const auto it = loose.find(a_mod);
		if (it != loose.end() && it->second) {
			if (const auto* full = it->second->As<RE::TESFullName>()) {
				if (const char* n = full->fullName.c_str(); n && n[0] != 0) {
					return std::string{ n };
				}
			}
		}
		if (const auto* full = a_mod->As<RE::TESFullName>()) {
			if (const char* n = full->fullName.c_str(); n && n[0] != 0) {
				return std::string{ n };
			}
		}
		return std::format("{:08X}", a_fid);
	}

	void AppendUniqueModPick(std::vector<PendingModPick>& a_out, const RE::BGSMod::Attachment::Mod* a_mod)
	{
		if (!a_mod) {
			return;
		}
		const auto fid = static_cast<std::uint32_t>(a_mod->GetFormID());
		if (fid == 0) {
			return;
		}

		auto& loose = RE::BGSMod::Attachment::GetAllLooseMods();
		if (loose.find(a_mod) == loose.end()) {
			if (BWS::Settings::Get().debugLogging.load()) {
				logger::info("[BWS]   OMOD {:08X} has no loose item — skipping (default/template mod)"sv, fid);
			}
			return;
		}

		if (std::find_if(a_out.begin(), a_out.end(), [fid](const PendingModPick& p) { return p.formID == fid; }) != a_out.end()) {
			return;
		}
		PendingModPick pick{ .formID = fid, .label = ModDisplayName(a_mod, fid) };
		a_out.push_back(std::move(pick));
	}

	void CollectWeaponMods(const RE::TESObjectWEAP* a_weapon, const RE::BGSObjectInstanceExtra* a_inst,
		std::vector<PendingModPick>& a_outMods)
	{
		if (!a_weapon || !a_inst) {
			return;
		}

		const bool dbg = BWS::Settings::Get().debugLogging.load();
		const auto indices = a_inst->GetIndexData();

		if (dbg) {
			logger::info("[BWS] CollectWeaponMods: weapon={:08X} '{}', itemIndex={}, {} OID entries"sv,
				static_cast<std::uint32_t>(a_weapon->GetFormID()),
				a_weapon->GetFullName() ? a_weapon->GetFullName() : "?",
				a_inst->itemIndex, indices.size());
		}

		for (std::size_t i = 0; i < indices.size(); ++i) {
			const auto& oid = indices[i];
			if (oid.disabled) {
				if (dbg) {
					logger::info("[BWS]   OID[{}]: objectID={:08X} index={} rank={} DISABLED"sv,
						i, oid.objectID, oid.index, oid.rank);
				}
				continue;
			}

			auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(oid.objectID);
			if (mod) {
				AppendUniqueModPick(a_outMods, mod);
				if (dbg) {
					logger::info("[BWS]   OID[{}]: objectID={:08X} -> '{}'"sv,
						i, oid.objectID, ModDisplayName(mod, oid.objectID));
				}
			} else if (dbg) {
				logger::info("[BWS]   OID[{}]: objectID={:08X} -> GetFormByID<Mod> returned null"sv,
					i, oid.objectID);
			}
		}
	}

	void CollectWeaponModsFromInventory(const RE::TESObjectWEAP* a_weapon, const RE::BGSInventoryItem* a_inv,
		std::vector<PendingModPick>& a_outMods)
	{
		if (!a_weapon || !a_inv) {
			return;
		}

		const bool dbg = BWS::Settings::Get().debugLogging.load();
		std::uint32_t stackIdx = 0;

		for (const RE::BGSInventoryItem::Stack* stack = a_inv->stackData.get(); stack; stack = stack->nextStack.get()) {
			const auto* xList = stack->extra.get();
			if (!xList) {
				if (dbg) {
					logger::info("[BWS] CollectWeaponModsFromInventory: stack[{}] has no extra-data list"sv, stackIdx);
				}
				++stackIdx;
				continue;
			}
			const auto* inst = xList->GetByType<RE::BGSObjectInstanceExtra>();
			if (!inst) {
				if (dbg) {
					logger::info("[BWS] CollectWeaponModsFromInventory: stack[{}] has no BGSObjectInstanceExtra"sv, stackIdx);
				}
				++stackIdx;
				continue;
			}
			if (dbg) {
				logger::info("[BWS] CollectWeaponModsFromInventory: stack[{}] has InstanceExtra, collecting mods"sv, stackIdx);
			}
			CollectWeaponMods(a_weapon, inst, a_outMods);
			++stackIdx;
		}

		if (dbg) {
			logger::info("[BWS] CollectWeaponModsFromInventory: {} stacks scanned, {} total mods collected"sv,
				stackIdx, a_outMods.size());
		}
	}

	static std::string BoundObjectDisplayName(const RE::TESBoundObject* a_bound)
	{
		if (!a_bound) {
			return "?";
		}
		if (const auto* full = a_bound->As<RE::TESFullName>()) {
			if (const char* n = full->fullName.c_str(); n && n[0] != 0) {
				return std::string{ n };
			}
		}
		if (const char* eid = a_bound->GetFormEditorID(); eid && eid[0] != 0) {
			return std::string{ eid };
		}
		return std::format("{:08X}", a_bound->GetFormID());
	}

	RE::BGSConstructibleObject* FindConstructibleForWeapon(const RE::TESObjectWEAP* a_weapon)
	{
		if (!a_weapon) {
			return nullptr;
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return nullptr;
		}
		auto& arr = dh->GetFormArray<RE::BGSConstructibleObject>();
		for (auto* cobj : arr) {
			if (!cobj) {
				continue;
			}
			if (cobj->GetCreatedItem() == static_cast<const RE::TESForm*>(a_weapon)) {
				return cobj;
			}
		}
		return nullptr;
	}

	void FillRecipeMaterials(const RE::BGSConstructibleObject* a_cobj, std::vector<RecipeMaterialLine>& a_out)
	{
		a_out.clear();
		if (!a_cobj || !a_cobj->requiredItems) {
			return;
		}
		for (const auto& tup : *a_cobj->requiredItems) {
			auto* form = tup.first;
			const auto qty = tup.second.i;
			if (!form || qty == 0) {
				continue;
			}
			auto* bound = form->As<RE::TESBoundObject>();
			if (bound) {
				a_out.push_back(RecipeMaterialLine{
					.ingredientFormID = static_cast<std::uint32_t>(bound->GetFormID()),
					.count            = static_cast<std::uint32_t>(qty),
					.displayName      = BoundObjectDisplayName(bound),
				});
			}
		}
	}

	bool BuildPendingFromExamineMenuImpl(RE::ExamineMenu* a_menu, PendingWeaponScrap& a_out)
	{
		if (!a_menu) {
			return false;
		}

		auto* invItem = reinterpret_cast<RE::BGSInventoryItem*>(
			reinterpret_cast<std::byte*>(a_menu) + kExamineMenu_ModdedInventoryItem);

		auto* base = invItem->object;
		auto* weap = base ? base->As<RE::TESObjectWEAP>() : nullptr;
		if (!weap) {
			return false;
		}

		if (const char* fn = weap->GetFullName(); fn && fn[0] != 0) {
			a_out.weaponDisplayName = fn;
		} else if (const char* eid = weap->GetFormEditorID(); eid && eid[0] != 0) {
			a_out.weaponDisplayName = eid;
		} else {
			a_out.weaponDisplayName = "(weapon)";
		}

		CollectWeaponModsFromInventory(weap, invItem, a_out.mods);

		if (const auto* cobj = FindConstructibleForWeapon(weap)) {
			if (const char* ce = cobj->GetFormEditorID(); ce && ce[0] != 0) {
				a_out.matchedCobjEditorID = ce;
			} else {
				a_out.matchedCobjEditorID = std::format("COBJ {:08X}", cobj->GetFormID());
			}
			FillRecipeMaterials(cobj, a_out.recipeMaterials);
		}

		return true;
	}

	void HookedScrapItemOnAccept(void* a_self)
	{
		PendingWeaponScrap pending{};
		const auto* layout = reinterpret_cast<const ScrapItemCallbackLayout*>(a_self);
		if (layout && layout->examineMenu) {
			BuildPendingFromExamineMenuImpl(layout->examineMenu, pending);
		}

		g_scrapOnAcceptOriginal(a_self);

		if (!BWS::Settings::Get().masterEnabled.load()) {
			if (BWS::Settings::Get().debugLogging.load()) {
				logger::info("[BWS] scrap confirm ignored (master disabled)."sv);
			}
			return;
		}

		// Native grant pipeline: recovery ran BEFORE this confirm (pre-scrap
		// picker + scrappingArray staging), and the original call above just
		// granted everything natively. The legacy post-scrap picker must not
		// open on top of that — it would double-grant.
		if (BWS::ExamineMenuBridge::IsNativeGrantScrapInFlight() ||
			(BWS::Settings::Get().useNativeGrant.load() && !BWS::Settings::Get().nativeUIOnly.load())) {
			if (BWS::Settings::Get().debugLogging.load()) {
				logger::info("[BWS] scrap confirm accepted (native grant pipeline; legacy queue skipped)."sv);
			}
			return;
		}

		if (!pending.weaponDisplayName.empty()) {
			if (BWS::Settings::Get().debugLogging.load()) {
				logger::info(
					"[BWS] queued recovery UI for '{}' ({} mods, {} recipe lines)."sv,
					pending.weaponDisplayName,
					pending.mods.size(),
					pending.recipeMaterials.size());
			}
			ScrapOverlay::QueuePending(std::move(pending));
		} else if (BWS::Settings::Get().debugLogging.load() && layout && layout->examineMenu) {
			logger::info("[BWS] scrap confirm did not produce a weapon pending state (not a weapon or bad menu snapshot)."sv);
		}
	}
}

namespace ScrapManager
{
	bool BuildPendingFromExamineMenu(RE::ExamineMenu* a_menu, PendingWeaponScrap& a_out)
	{
		return BuildPendingFromExamineMenuImpl(a_menu, a_out);
	}

	bool Install()
	{
		if (g_scrapOnAcceptOriginal) {
			return true;
		}

		try {
			// VTABLE::__ScrapItemCallback[1] == OnAccept (ExamineConfirmMenu::ICallback vfunc 01)
			REL::Relocation<std::uintptr_t> vtblStart{ RE::VTABLE::__ScrapItemCallback[0] };
			auto*           vtable = reinterpret_cast<std::uintptr_t*>(vtblStart.address());
			constexpr auto    kOnAcceptIndex = 1u;
			const auto        slotAddr = reinterpret_cast<std::uintptr_t>(&vtable[kOnAcceptIndex]);
			const auto        savedVfn = vtable[kOnAcceptIndex];
			g_scrapOnAcceptOriginal = reinterpret_cast<ScrapOnAcceptFn>(savedVfn);

			REL::safe_write(slotAddr, reinterpret_cast<std::uintptr_t>(&HookedScrapItemOnAccept));

			logger::info(
				"Better Weapon Scrapping: hooked ExamineMenu ScrapItemCallback::OnAccept (workbench scrap) @ vtable[{}] slot {:X}"sv,
				kOnAcceptIndex,
				slotAddr);
			spdlog::default_logger()->flush();
			return true;
		} catch (const std::exception& e) {
			logger::error("Better Weapon Scrapping: workbench scrap hook install failed: {}"sv, e.what());
			return false;
		} catch (...) {
			logger::error("Better Weapon Scrapping: workbench scrap hook install failed (unknown)"sv);
			return false;
		}
	}
}
