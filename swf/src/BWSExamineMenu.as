package
{
	import flash.display.MovieClip;
	import flash.events.Event;

	// Document (root) class for BWSExamineMenu.swf.
	//
	// Loaded into Interface/ExamineMenu.swf by ExamineMenuBridge via a
	// flash.display.Loader. Registers against root.bws (native callbacks).
	//
	// Jobs:
	//  1. Show "SCRAP MODS" [G] on the real ButtonBarMenu hint bar.
	//     Pushing a NEW BSButtonHintData into the live vectors does not
	//     reliably produce a ButtonHintDataWithClone entry (we logged
	//     hint SHOWN with never a hint vector OK). Instead we hijack
	//     ExamineMenu's existing AlternateButton — already present in
	//     InventoryButtonHints / ModSlotButtonHints / ModsListHints and
	//     already wired through the native clone path. Vanilla only shows
	//     it when strAlternateButtonText is non-empty (usually empty at
	//     the weapons bench); we reclaim it after each UpdateButtons.
	//  2. Wrap BGSCodeObj.ScrapItem for the pre-scrap recovery picker.
	public class BWSExamineMenu extends MovieClip
	{
		private var injected:Boolean = false;

		private var base:Object = null;
		private var bws:Object = null;
		private var bar:Object = null;

		// ExamineMenu.AlternateButton — host-domain object already in the
		// live hint vectors and known to ButtonHintDataWithClone.
		private var altHint:Object = null;
		private var loggedAltCapture:Boolean = false;

		private var origUpdate:Function = null;
		private var updateWrapper:Function = null;

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

		// AlternateButton is constructed as BSButtonHintData("", "R", ...).
		// ScrapButton is also PCKey "R" but starts with ButtonText "$SCRAP".
		// Capture happens at SetButtonHintData time, BEFORE UpdateButtons
		// mutates AlternateButton.ButtonText.
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
			// After we have claimed it, text is "SCRAP MODS" — keep identity
			// if we already hold altHint and it is still in this vector.
			if (altHint != null && v.indexOf(altHint) >= 0)
			{
				return altHint;
			}
			for (i = 0; i < v.length; i++)
			{
				h = v[i];
				if (h && String(h.ButtonText) == "SCRAP MODS")
				{
					return h;
				}
			}
			return null;
		}

		// Drive the hijacked AlternateButton from plugin state. Called after
		// every UpdateButtons (which resets Alternate to vanilla) and from
		// the per-frame sync when visibility/key changes.
		private function applyAltHint():void
		{
			if (!altHint || !bws)
			{
				return;
			}

			// Yield if the game itself has claimed the alternate slot
			// (non-empty text that is not our label).
			var curText:String = "";
			try
			{
				curText = String(altHint.ButtonText);
			}
			catch (err:Error)
			{
				curText = "";
			}
			if (altHint.ButtonVisible && curText.length > 0 && curText != "SCRAP MODS")
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
				// onTextClick is the public click entry BSButtonHint invokes;
				// replacing it routes bar clicks to our picker.
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

			// Wrap UpdateButtons: capture the live vector during its
			// SetButtonHintData call, then re-apply our AlternateButton
			// claim after vanilla resets it.
			origUpdate = base.UpdateButtons as Function;
			var self:BWSExamineMenu = this;
			updateWrapper = function():*
			{
				var nativeSet:Function = self.bar.SetButtonHintData as Function;
				var captured:* = null;
				self.bar.SetButtonHintData = function(v:*):void
				{
					captured = v;
					// Property-slot invoke of the real native/AS setter.
					self.bar.SetButtonHintData = nativeSet;
					try
					{
						self.bar.SetButtonHintData(v);
					}
					finally
					{
						// Leave restored; UpdateButtons only publishes once
						// per call, then mutates hint properties in place.
					}
				};

				var result:* = self.origUpdate.call(self.base);

				// Ensure the bar property is native again if UpdateButtons
				// never called SetButtonHintData (some modes).
				self.bar.SetButtonHintData = nativeSet;

				if (captured != null)
				{
					var found:Object = self.findAlternateButton(captured);
					if (found != null)
					{
						self.altHint = found;
					}
				}

				self.applyAltHint();
				return result;
			};
			base.UpdateButtons = updateWrapper;

			wrapScrapItem(codeObj);

			try
			{
				base.UpdateButtons();
			}
			catch (errUB:Error)
			{
				log("BWSExamineMenu.swf: initial UpdateButtons failed: " + errUB.message);
			}

			if (altHint)
			{
				log("BWSExamineMenu.swf: injection complete (AlternateButton hijack + ScrapItem wrap)");
			}
			else
			{
				log("BWSExamineMenu.swf: injection complete but AlternateButton NOT captured yet");
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
			if (!bws)
			{
				return;
			}

			// Reinstall UpdateButtons wrap if something stole it.
			if (base && updateWrapper != null && base.UpdateButtons != updateWrapper)
			{
				origUpdate = base.UpdateButtons as Function;
				base.UpdateButtons = updateWrapper;
				log("BWSExamineMenu.swf: reinstalled UpdateButtons wrapper");
			}

			if (!altHint)
			{
				// Keep trying to capture via a forced UpdateButtons.
				try
				{
					if (base)
					{
						base.UpdateButtons();
					}
				}
				catch (errCap:Error)
				{
				}
				if (altHint && !loggedAltCapture)
				{
					loggedAltCapture = true;
					log("BWSExamineMenu.swf: AlternateButton captured (deferred)");
				}
				return;
			}

			if (!loggedAltCapture)
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

			var key:String = String(bws.GetHintKey());
			var changed:Boolean = (vis != lastVisible) || (vis && key != lastKey);

			if (vis != lastVisible)
			{
				lastVisible = vis;
				log("BWSExamineMenu.swf: hint " + (vis ? "SHOWN" : "hidden"));
			}
			if (key != lastKey)
			{
				lastKey = key;
			}

			if (changed)
			{
				applyAltHint();
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
