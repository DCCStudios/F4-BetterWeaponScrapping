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
	// The plugin's native Scaleform callback (src/ExamineMenuBridge.cpp)
	// registers a code object on the host root as "bws".
	//
	// This SWF renders nothing of its own. Its jobs are:
	//
	//  1. Append a native "SCRAP MODS" button hint to the workbench button
	//     bar. ExamineMenu's ButtonHintBar_mc declares SetButtonHintData as a
	//     reassignable `public var ...:Function` (replaced by native code once
	//     the bar is acquired), so we wrap it: every time the menu swaps hint
	//     vectors (UpdateButtons), we submit a FRESH Vector that includes our
	//     hint (native ButtonBarMenu marshaling appears to no-op when handed
	//     the same Vector reference again), then forward to the original.
	//
	//  2. Wrap BGSCodeObj.ScrapItem so the plugin can intercept the scrap
	//     BEFORE the native confirm dialog is built. The original function is
	//     kept as BGSCodeObj.bws_origScrapItem so the plugin can re-invoke the
	//     vanilla scrap after the player picks recovery options.
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

		// One-shot diagnostics so we don't spam the plugin log every frame.
		private var loggedPushOk:Boolean = false;
		private var loggedPushFail:Boolean = false;

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

		private function log(msg:String):void
		{
			if (!bws)
			{
				return;
			}
			try
			{
				bws.Log(msg);
			}
			catch (err:Error)
			{
			}
		}

		// Build a BSButtonHintData in the HOST movie's application domain.
		// Prefer getDefinitionByName over the C++-precreated root.bws_hintData:
		// Scaleform CreateObject from native has produced an object that is
		// "IsObject" but is rejected by Vector.<BSButtonHintData>.push (typed
		// Vector type-check), which silently aborted our SetButtonHintData
		// wrapper before the native forward ever ran.
		private function createHintData():Object
		{
			try
			{
				var HintClass:Class = getDefinitionByName("Shared.AS3.BSButtonHintData") as Class;
				var hd:Object = new HintClass(
					"SCRAP MODS",
					String(bws.GetHintKey()),
					"PSN_Y",
					"Xenon_Y",
					1,
					onScrapModsPressed);
				log("BWSExamineMenu.swf: hint data created via getDefinitionByName");
				return hd;
			}
			catch (err:Error)
			{
				log("BWSExamineMenu.swf: getDefinitionByName failed: " + err.message);
			}

			var host:Object = hostRoot();
			var fallback:Object = host ? host["bws_hintData"] : null;
			if (fallback)
			{
				log("BWSExamineMenu.swf: falling back to plugin-created hint data (may fail Vector.push)");
			}
			return fallback;
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

			hintData = createHintData();
			if (!hintData)
			{
				log("BWSExamineMenu.swf: FAILED to obtain BSButtonHintData - button hint disabled (hotkey still works)");
				// Still wrap ScrapItem so the pre-scrap picker keeps working.
				wrapScrapItem(codeObj);
				finishInjection();
				return;
			}

			// Start hidden; syncHint() shows it when the plugin says so.
			hintData.ButtonVisible = false;
			lastVisible = false;
			lastKey = String(bws.GetHintKey());

			// --- Wrap ButtonHintBar_mc.SetButtonHintData ------------------
			// Always forward a NEW Vector (via concat) that includes our hint.
			// Mutating the menu's vector in place and re-submitting the same
			// reference is not reliably remapped by the native ButtonBarMenu
			// path; a fresh Vector forces the marshal.
			var origSet:Function = bar.SetButtonHintData as Function;
			var hd:Object = hintData;
			var self:BWSExamineMenu = this;
			bar.SetButtonHintData = function(v:*):void
			{
				if (v == null)
				{
					origSet.call(bar, v);
					return;
				}
				try
				{
					var out:* = v.concat();
					if (out.indexOf(hd) < 0)
					{
						out.push(hd);
					}
					if (!self.loggedPushOk)
					{
						self.loggedPushOk = true;
						self.log("BWSExamineMenu.swf: hint vector OK (len=" + out.length + ", visible=" + hd.ButtonVisible + ")");
					}
					origSet.call(bar, out);
				}
				catch (pushErr:Error)
				{
					if (!self.loggedPushFail)
					{
						self.loggedPushFail = true;
						self.log("BWSExamineMenu.swf: hint vector FAILED: " + pushErr.message);
					}
					// Never block the vanilla bar if our append blows up.
					origSet.call(bar, v);
				}
			};

			wrapScrapItem(codeObj);

			// Force one bar rebuild so the current mode's vector flows
			// through our wrapper immediately.
			try
			{
				base.UpdateButtons();
			}
			catch (err3:Error)
			{
				log("BWSExamineMenu.swf: initial UpdateButtons failed: " + err3.message);
			}

			log("BWSExamineMenu.swf: injection complete (hint wrapper + ScrapItem wrap)");
			finishInjection();
		}

		private function wrapScrapItem(codeObj:Object):void
		{
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
				log("BWSExamineMenu.swf: hint " + (vis ? "SHOWN" : "hidden"));
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

			// Re-submit through UpdateButtons when becoming visible so the
			// wrapper builds a fresh Vector with ButtonVisible already true.
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

		// Click handler for the AS-constructed hint. Ends in the same native
		// OpenScrapMods() -> scrap-mods picker as the keyboard hotkey.
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
