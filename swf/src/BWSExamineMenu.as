package
{
	import flash.display.MovieClip;
	import flash.events.Event;
	import flash.utils.getDefinitionByName;

	// Document (root) class for BWSExamineMenu.swf.
	//
	// Loaded into the game's workbench menu (Interface/ExamineMenu.swf) by the
	// Better Weapon Scrapping plugin via a flash.display.Loader — the same
	// injection pattern the F4SE Menu Framework uses for the pause menu.
	// The plugin's native Scaleform callback (src/ExamineMenuIntegration.cpp)
	// registers a code object on the host root as "bws" and, when possible,
	// pre-creates a Shared.AS3.BSButtonHintData instance as "bws_hintData".
	//
	// This SWF renders nothing of its own. Its jobs are:
	//
	//  1. Append a native "SCRAP MODS" button hint to the workbench button
	//     bar. ExamineMenu's ButtonHintBar_mc declares SetButtonHintData as a
	//     reassignable `public var ...:Function` (replaced by native code once
	//     the bar is acquired), so we wrap it: every time the menu swaps hint
	//     vectors (UpdateButtons), we push our hint into the vector before
	//     forwarding to the original function. Clicking the hint (or pressing
	//     the plugin hotkey) opens the plugin's scrap-mods picker.
	//
	//  2. Wrap BGSCodeObj.ScrapItem so the plugin can intercept the scrap
	//     BEFORE the native confirm dialog is built. The original function is
	//     kept as BGSCodeObj.bws_origScrapItem so the plugin can re-invoke the
	//     vanilla scrap after the player picks recovery options; the plugin's
	//     BuildWeaponScrappingArray hook then appends the chosen items to the
	//     scrap results, so the native confirm lists them and the game itself
	//     grants them.
	public class BWSExamineMenu extends MovieClip
	{
		private var injected:Boolean = false;

		private var base:Object = null;      // root.BaseInstance (the ExamineMenu document class)
		private var bws:Object = null;       // native code object registered by the plugin
		private var hintData:Object = null;  // Shared.AS3.BSButtonHintData driving our bar entry

		// Cached last-pushed state so we only touch the hint when it changes
		// (each property write dispatches a bar redraw).
		private var lastVisible:Boolean = false;
		private var lastKey:String = "";

		public function BWSExamineMenu()
		{
			super();
			// The menu's native registration (BGSCodeObj functions, button bar
			// acquisition) completes after this SWF loads, so poll each frame
			// until everything we depend on exists.
			addEventListener(Event.ENTER_FRAME, onEnterFrame);
		}

		private function onEnterFrame(e:Event):void
		{
			if (!injected)
			{
				tryInject();
			}
			else
			{
				syncHint();
			}
		}

		private function hostRoot():Object
		{
			// stage.getChildAt(0) is the host ExamineMenu.swf root clip.
			return stage ? stage.getChildAt(0) : null;
		}

		private function tryInject():void
		{
			var host:Object = hostRoot();
			if (!host)
			{
				return;
			}

			base = host["BaseInstance"];
			bws = host["bws"];
			if (!base || !bws)
			{
				return;
			}

			// Wait until native code has registered its functions on
			// BGSCodeObj (ScrapItem appears via MapCodeObjectFunctions)...
			var codeObj:Object = base["BGSCodeObj"];
			if (!codeObj || codeObj.ScrapItem == undefined)
			{
				return;
			}

			// ...and until the button bar has been acquired by native code.
			// Native acquisition replaces bar.SetButtonHintData with a native
			// function that forwards hint vectors to the shared ButtonBarMenu;
			// wrapping before that happens would get our wrapper overwritten.
			var bar:Object = base["ButtonHintBar_mc"];
			if (!bar || !bar["bAcquiredByNativeCode"])
			{
				return;
			}

			// --- Build the BSButtonHintData for our bar entry -------------
			// Primary: instance pre-created by the plugin in the host movie's
			// application domain (root.bws_hintData). Fallback: construct it
			// ourselves — our Loader-loaded SWF's application domain is a
			// child of the host's, so the host's class is resolvable by name.
			hintData = host["bws_hintData"];
			if (!hintData)
			{
				try
				{
					var HintClass:Class = getDefinitionByName("Shared.AS3.BSButtonHintData") as Class;
					hintData = new HintClass("SCRAP MODS", String(bws.GetHintKey()), "PSN_Y", "Xenon_Y", 1, onScrapModsPressed);
					bws.Log("BWSExamineMenu.swf: hint data created in ActionScript (getDefinitionByName)");
				}
				catch (err:Error)
				{
					hintData = null;
				}
			}
			else
			{
				bws.Log("BWSExamineMenu.swf: using plugin-created hint data (root.bws_hintData)");
			}

			if (!hintData)
			{
				bws.Log("BWSExamineMenu.swf: FAILED to obtain BSButtonHintData - button hint disabled (hotkey still works)");
				finishInjection();
				return;
			}

			// Start hidden; syncHint() shows it when the plugin says so.
			hintData.ButtonVisible = false;
			lastVisible = false;
			lastKey = String(bws.GetHintKey());

			// --- Wrap ButtonHintBar_mc.SetButtonHintData ------------------
			// The menu calls this with a fresh Vector.<BSButtonHintData> on
			// every mode change (UpdateButtons). We append our hint (once per
			// vector — the menu reuses the same vector instances) and forward.
			var origSet:Function = bar.SetButtonHintData as Function;
			var hd:Object = hintData;
			bar.SetButtonHintData = function(v:*):void
			{
				if (v != null && v.indexOf(hd) < 0)
				{
					v.push(hd);
				}
				origSet.call(bar, v);
			};

			// --- Wrap BGSCodeObj.ScrapItem --------------------------------
			// OnScrapRequested() returns true when the plugin opened its
			// pre-scrap recovery picker (it will re-invoke bws_origScrapItem
			// after the player chooses); false means proceed vanilla.
			var origScrap:Function = codeObj.ScrapItem as Function;
			codeObj.bws_origScrapItem = origScrap;
			var nativeObj:Object = bws;
			codeObj.ScrapItem = function():void
			{
				var handled:Boolean = false;
				try
				{
					handled = Boolean(nativeObj.OnScrapRequested());
				}
				catch (err2:Error)
				{
					handled = false;
				}
				if (!handled)
				{
					origScrap.call(codeObj);
				}
			};

			// Force one bar rebuild so the current mode's vector flows
			// through our wrapper immediately.
			try
			{
				base.UpdateButtons();
			}
			catch (err3:Error)
			{
			}

			bws.Log("BWSExamineMenu.swf: injection complete (hint appended, ScrapItem wrapped)");
			finishInjection();
		}

		private function finishInjection():void
		{
			injected = true;
		}

		// Per-frame sync of hint visibility / key label from the plugin.
		// Calls originate on the Scaleform advance thread, so the plugin's
		// handlers only read simple state.
		private function syncHint():void
		{
			if (!bws || !hintData)
			{
				return;
			}

			var vis:Boolean = false;
			try
			{
				vis = Boolean(bws.IsHintVisible());
			}
			catch (err:Error)
			{
				vis = false;
			}

			var becameVisible:Boolean = false;

			if (vis != lastVisible)
			{
				lastVisible = vis;
				hintData.ButtonVisible = vis;
				becameVisible = vis;
				// Transition log: proves in BetterWeaponScrappingF4SE.log
				// whether the C++ visibility gate ever fires in-game.
				try
				{
					bws.Log("BWSExamineMenu.swf: hint " + (vis ? "SHOWN" : "hidden"));
				}
				catch (errLog:Error)
				{
				}
			}

			if (vis)
			{
				var key:String = String(bws.GetHintKey());
				if (key != lastKey)
				{
					lastKey = key;
					// Gamepad glyphs are placeholders — the feature is
					// keyboard/mouse driven, matching the old ImGui hint.
					hintData.SetButtons(key, "PSN_Y", "Xenon_Y");
					becameVisible = true;
				}
			}

			// The acquired bar renders through the game's separate
			// ButtonBarMenu movie: native code marshals the hint vector
			// across movies when SetButtonHintData runs. A hint submitted
			// while invisible is not reliably re-marshaled by a later
			// ButtonVisible flip alone, so when OUR hint just turned visible
			// (or changed key), re-push the current mode's vector through the
			// menu's own UpdateButtons — our SetButtonHintData wrapper keeps
			// the hint appended. Deliberately NOT done on hide transitions:
			// those coincide with the confirm dialog / picker opening, and
			// resubmitting the workbench bar then could stomp the dialog's
			// own button bar (natural UpdateButtons calls hide it instead).
			if (becameVisible)
			{
				try
				{
					base.UpdateButtons();
				}
				catch (err2:Error)
				{
				}
			}
		}

		// Click handler for the AS-constructed hint (the plugin-created one
		// carries its own native callback). Both routes end in the same
		// native OpenScrapMods() -> scrap-mods picker.
		private function onScrapModsPressed():void
		{
			if (bws)
			{
				try
				{
					bws.OpenScrapMods();
				}
				catch (err:Error)
				{
				}
			}
		}
	}
}
