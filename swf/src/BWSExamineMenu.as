package
{
	import flash.display.MovieClip;
	import flash.events.Event;
	import flash.utils.getDefinitionByName;

	// Document (root) class for BWSExamineMenu.swf.
	//
	// Loaded into Interface/ExamineMenu.swf by ExamineMenuBridge via a
	// flash.display.Loader (same pattern as F4SE Menu Framework pause inject).
	// The plugin registers root.bws (native Scaleform callbacks).
	//
	// Jobs:
	//  1. Append a "SCRAP MODS" BSButtonHintData onto ExamineMenu's LIVE mode
	//     hint vectors (InventoryButtonHints, ModsListHints, ...) so the native
	//     ButtonBarMenu marshal (ButtonHintDataWithClone) paints it. FallUI /
	//     vanilla mutate the same Vector identity in place — we must NOT
	//     Vector.concat a shadow copy.
	//  2. Wrap BGSCodeObj.ScrapItem for the pre-scrap recovery picker.
	public class BWSExamineMenu extends MovieClip
	{
		private var injected:Boolean = false;

		private var base:Object = null;      // root.BaseInstance
		private var bws:Object = null;       // native code object
		private var hintData:Object = null;  // host-domain BSButtonHintData
		private var bar:Object = null;       // ButtonHintBar_mc
		private var origSetNative:Function = null;
		private var setWrapper:Function = null;

		private var lastVisible:Boolean = false;
		private var lastKey:String = "";

		private var loggedPushOk:Boolean = false;
		private var loggedPushFail:Boolean = false;

		public function BWSExamineMenu()
		{
			super();
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

		// Prefer host-domain construction via getDefinitionByName. C++
		// CreateObject can yield an object that fails typed Vector.push.
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

		// Forward to the native SetButtonHintData through the property slot
		// (not Function.call), matching how ExamineMenu invokes it.
		private function invokeNativeSet(v:*):void
		{
			bar.SetButtonHintData = origSetNative;
			try
			{
				bar.SetButtonHintData(v);
			}
			finally
			{
				bar.SetButtonHintData = setWrapper;
			}
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

			var codeObj:Object = base["BGSCodeObj"];
			if (!codeObj || codeObj.ScrapItem == undefined)
			{
				return;
			}

			// Wait until native has replaced SetButtonHintData — wrapping
			// earlier would be overwritten on acquisition.
			bar = base["ButtonHintBar_mc"];
			if (!bar || !bar["bAcquiredByNativeCode"])
			{
				return;
			}

			hintData = createHintData();
			if (!hintData)
			{
				log("BWSExamineMenu.swf: FAILED to obtain BSButtonHintData - button hint disabled (hotkey still works)");
				wrapScrapItem(codeObj);
				finishInjection();
				return;
			}

			hintData.ButtonVisible = false;
			lastVisible = false;
			lastKey = String(bws.GetHintKey());

			// In-place push onto the live mode Vector, then property-slot
			// native forward (FallUI / vanilla pattern — no concat).
			origSetNative = bar.SetButtonHintData as Function;
			var hd:Object = hintData;
			var self:BWSExamineMenu = this;
			setWrapper = function(v:*):void
			{
				if (v == null)
				{
					self.invokeNativeSet(v);
					return;
				}
				try
				{
					if (v.indexOf(hd) < 0)
					{
						v.push(hd);
					}
					if (!self.loggedPushOk)
					{
						self.loggedPushOk = true;
						self.log("BWSExamineMenu.swf: hint vector OK (len=" + v.length + ", visible=" + hd.ButtonVisible + ")");
					}
					self.invokeNativeSet(v);
				}
				catch (pushErr:Error)
				{
					if (!self.loggedPushFail)
					{
						self.loggedPushFail = true;
						self.log("BWSExamineMenu.swf: hint vector FAILED: " + pushErr.message);
					}
					self.invokeNativeSet(v);
				}
			};
			bar.SetButtonHintData = setWrapper;

			wrapScrapItem(codeObj);

			try
			{
				base.UpdateButtons();
			}
			catch (err3:Error)
			{
				log("BWSExamineMenu.swf: initial UpdateButtons failed: " + err3.message);
			}

			log("BWSExamineMenu.swf: injection complete (in-place hint + ScrapItem wrap)");
			finishInjection();
		}

		private function wrapScrapItem(codeObj:Object):void
		{
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
					hintData.SetButtons(key, "PSN_Y", "Xenon_Y");
					becameVisible = true;
				}
			}

			// Republish the live vector once visibility is true so native
			// ButtonHintDataWithClone wires a clone that starts visible.
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
