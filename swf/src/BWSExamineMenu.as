package
{
	import flash.display.MovieClip;
	import flash.events.Event;

	// Document (root) class for BWSExamineMenu.swf.
	//
	// Loaded into Interface/ExamineMenu.swf by ExamineMenuBridge via Loader.
	//
	// IMPORTANT: ExamineMenu.UpdateButtons is a class METHOD — it cannot be
	// replaced with `base.UpdateButtons = fn` (non-dynamic class throws and
	// aborts this SWF before any log). Only `ButtonHintBar_mc.SetButtonHintData`
	// is a reassignable `public var ...:Function` (same as BSButtonHintBar).
	//
	// Jobs:
	//  1. Show SCRAP MODS [G] on ButtonBarMenu by reclaiming the existing
	//     AlternateButton entry already present in the live hint vectors
	//     (wired through ButtonHintDataWithClone). Capture it inside the
	//     SetButtonHintData wrapper; re-apply text/key/visibility every
	//     frame after vanilla UpdateButtons resets it.
	//  2. Wrap BGSCodeObj.ScrapItem for the pre-scrap recovery picker.
	public class BWSExamineMenu extends MovieClip
	{
		private var injected:Boolean = false;

		private var base:Object = null;
		private var bws:Object = null;
		private var bar:Object = null;

		private var altHint:Object = null;
		private var loggedAltCapture:Boolean = false;

		private var origSetNative:Function = null;
		private var setWrapper:Function = null;

		private var lastVisible:Boolean = false;
		private var lastKey:String = "";

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

		// AlternateButton ctor: BSButtonHintData("", "R", ...).
		// ScrapButton also uses PCKey "R" but ButtonText "$SCRAP".
		// Capture runs at SetButtonHintData time, before UpdateButtons
		// mutates AlternateButton.ButtonText / ButtonVisible.
		private function findAlternateButton(v:*):Object
		{
			if (v == null)
			{
				return null;
			}
			var i:int = 0;
			for (i = 0; i < v.length; i++)
			{
				var h:Object = v[i];
				if (!h)
				{
					continue;
				}
				try
				{
					if (String(h.PCKey) == "R" && String(h.ButtonText) == "")
					{
						return h;
					}
				}
				catch (err:Error)
				{
				}
			}
			if (altHint != null)
			{
				try
				{
					if (v.indexOf(altHint) >= 0)
					{
						return altHint;
					}
				}
				catch (err2:Error)
				{
				}
			}
			for (i = 0; i < v.length; i++)
			{
				h = v[i];
				if (h)
				{
					try
					{
						if (String(h.ButtonText) == "SCRAP MODS")
						{
							return h;
						}
					}
					catch (err3:Error)
					{
					}
				}
			}
			return null;
		}

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

		private function applyAltHint():void
		{
			if (!altHint || !bws)
			{
				return;
			}

			// Yield if the game claimed the alternate slot for itself.
			var curText:String = "";
			var gameClaimed:Boolean = false;
			try
			{
				curText = String(altHint.ButtonText);
				gameClaimed = Boolean(altHint.ButtonVisible) &&
					curText.length > 0 &&
					curText != "SCRAP MODS";
			}
			catch (err:Error)
			{
				gameClaimed = false;
			}
			if (gameClaimed)
			{
				return;
			}

			var vis:Boolean = false;
			try
			{
				vis = Boolean(bws.IsHintVisible());
			}
			catch (err2:Error)
			{
				vis = false;
			}

			var key:String = "G";
			try
			{
				key = String(bws.GetHintKey());
			}
			catch (err3:Error)
			{
				key = "G";
			}

			try
			{
				altHint.ButtonText = "SCRAP MODS";
				altHint.SetButtons(key, "PSN_Y", "Xenon_Y");
				altHint.ButtonEnabled = true;
				altHint.ButtonVisible = vis;
				altHint.onTextClick = onScrapModsPressed;
			}
			catch (err4:Error)
			{
				log("BWSExamineMenu.swf: applyAltHint failed: " + err4.message);
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

			bar = base["ButtonHintBar_mc"];
			if (!bar || !bar["bAcquiredByNativeCode"])
			{
				return;
			}

			log("BWSExamineMenu.swf: begin inject (bar acquired)");

			origSetNative = bar.SetButtonHintData as Function;
			var self:BWSExamineMenu = this;
			setWrapper = function(v:*):void
			{
				if (v != null)
				{
					try
					{
						var found:Object = self.findAlternateButton(v);
						if (found != null)
						{
							self.altHint = found;
						}
					}
					catch (errFind:Error)
					{
					}
				}
				self.invokeNativeSet(v);
			};
			bar.SetButtonHintData = setWrapper;

			wrapScrapItem(codeObj);

			// Publish current mode through our wrapper so we capture AlternateButton.
			// Do NOT assign to UpdateButtons — it is a method, not a Function var.
			try
			{
				base.UpdateButtons();
			}
			catch (errUB:Error)
			{
				log("BWSExamineMenu.swf: UpdateButtons failed: " + errUB.message);
			}

			applyAltHint();

			if (altHint)
			{
				log("BWSExamineMenu.swf: injection complete (AlternateButton hijack + ScrapItem wrap)");
			}
			else
			{
				log("BWSExamineMenu.swf: injection complete; AlternateButton not in vector yet (will retry)");
			}
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
			if (!bws || !bar)
			{
				return;
			}

			// Reinstall SetButtonHintData wrap if native stole it back.
			if (setWrapper != null && bar.SetButtonHintData != setWrapper)
			{
				origSetNative = bar.SetButtonHintData as Function;
				bar.SetButtonHintData = setWrapper;
				log("BWSExamineMenu.swf: reinstalled SetButtonHintData wrapper");
				try
				{
					base.UpdateButtons();
				}
				catch (errRe:Error)
				{
				}
			}

			if (!altHint)
			{
				try
				{
					base.UpdateButtons();
				}
				catch (errCap:Error)
				{
				}
			}

			if (altHint && !loggedAltCapture)
			{
				loggedAltCapture = true;
				log("BWSExamineMenu.swf: AlternateButton captured for SCRAP MODS");
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

			var key:String = "G";
			try
			{
				key = String(bws.GetHintKey());
			}
			catch (errKey:Error)
			{
				key = "G";
			}

			if (vis != lastVisible)
			{
				lastVisible = vis;
				log("BWSExamineMenu.swf: hint " + (vis ? "SHOWN" : "hidden"));
			}
			lastKey = key;

			// Every frame: vanilla UpdateButtons may have cleared Alternate;
			// put SCRAP MODS back. Property changes ride ButtonHintDataWithClone.
			applyAltHint();
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
