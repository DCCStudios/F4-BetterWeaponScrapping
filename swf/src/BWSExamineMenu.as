package
{
	import flash.display.MovieClip;
	import flash.display.Sprite;
	import flash.events.Event;
	import flash.events.MouseEvent;
	import flash.text.AntiAliasType;
	import flash.text.TextField;
	import flash.text.TextFieldAutoSize;
	import flash.text.TextFormat;

	// Document class for BWSExamineMenu.swf — injected into ExamineMenu.swf.
	//
	// ButtonBarMenu / ButtonHintDataWithClone has repeatedly refused both a
	// new BSButtonHintData push and an AlternateButton hijack (log: "not in
	// vector yet" forever). The reliable cue is a Scaleform prompt we draw
	// ourselves on the host movie root, sitting on the workbench UI next to
	// the native button bar. ScrapItem wrap still intercepts SCRAP.
	public class BWSExamineMenu extends MovieClip
	{
		private var injected:Boolean = false;

		private var base:Object = null;
		private var bws:Object = null;
		private var bar:Object = null;

		private var origSetNative:Function = null;
		private var setWrapper:Function = null;
		private var dumpedVector:Boolean = false;

		// On-screen cue drawn into the host ExamineMenu root.
		private var cueRoot:Sprite = null;
		private var cueBracketL:Sprite = null;
		private var cueBracketR:Sprite = null;
		private var cueText:TextField = null;
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
				syncCue();
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

			// Optional: keep wrapping SetButtonHintData only to dump the live
			// vector once for diagnostics (why AlternateButton hijack failed).
			origSetNative = bar.SetButtonHintData as Function;
			var self:BWSExamineMenu = this;
			setWrapper = function(v:*):void
			{
				if (v != null && !self.dumpedVector)
				{
					self.dumpedVector = true;
					self.dumpHintVector(v);
				}
				self.bar.SetButtonHintData = self.origSetNative;
				try
				{
					self.bar.SetButtonHintData(v);
				}
				finally
				{
					self.bar.SetButtonHintData = self.setWrapper;
				}
			};
			bar.SetButtonHintData = setWrapper;

			wrapScrapItem(codeObj);
			ensureCue(host);

			try
			{
				base.UpdateButtons();
			}
			catch (errUB:Error)
			{
				log("BWSExamineMenu.swf: UpdateButtons failed: " + errUB.message);
			}

			log("BWSExamineMenu.swf: injection complete (Scaleform cue + ScrapItem wrap)");
			injected = true;
		}

		private function dumpHintVector(v:*):void
		{
			try
			{
				var parts:Array = [];
				var i:int = 0;
				for (i = 0; i < v.length; i++)
				{
					var h:Object = v[i];
					if (!h)
					{
						parts.push("[" + i + "]=null");
						continue;
					}
					var key:String = "?";
					var text:String = "?";
					var vis:String = "?";
					try { key = String(h.PCKey); } catch (e1:Error) {}
					try { text = String(h.ButtonText); } catch (e2:Error) {}
					try { vis = String(h.ButtonVisible); } catch (e3:Error) {}
					parts.push("[" + i + "] key='" + key + "' text='" + text + "' vis=" + vis);
				}
				log("BWSExamineMenu.swf: hint vector len=" + v.length + " " + parts.join(" | "));
			}
			catch (err:Error)
			{
				log("BWSExamineMenu.swf: dumpHintVector failed: " + err.message);
			}
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

		// Build a Pip-Boy-style bracketed prompt on the HOST root so it sits
		// in the workbench movie (not buried inside the Loader content).
		private function ensureCue(host:Object):void
		{
			if (cueRoot)
			{
				return;
			}

			cueRoot = new Sprite();
			cueRoot.mouseChildren = true;
			cueRoot.buttonMode = true;
			cueRoot.mouseEnabled = true;

			cueText = new TextField();
			cueText.autoSize = TextFieldAutoSize.LEFT;
			cueText.selectable = false;
			cueText.mouseEnabled = false;
			cueText.antiAliasType = AntiAliasType.ADVANCED;
			// Prefer the game's Pip-Boy font (registered on the host movie).
			// If embed fails and text measures empty, fall back to device fonts.
			cueText.embedFonts = true;
			var fmt:TextFormat = new TextFormat("$MAIN_Font_Bold", 20, 0x12FF15);
			cueText.defaultTextFormat = fmt;
			cueText.setTextFormat(fmt);
			cueText.text = "G  SCRAP MODS";
			if (cueText.textWidth < 2)
			{
				cueText.embedFonts = false;
				fmt.font = "_sans";
				cueText.defaultTextFormat = fmt;
				cueText.setTextFormat(fmt);
				cueText.text = "G  SCRAP MODS";
				log("BWSExamineMenu.swf: $MAIN_Font_Bold unavailable — using device _sans");
			}

			cueBracketL = makeBracket(true);
			cueBracketR = makeBracket(false);

			cueRoot.addChild(cueBracketL);
			cueRoot.addChild(cueText);
			cueRoot.addChild(cueBracketR);
			cueRoot.addEventListener(MouseEvent.CLICK, onCueClick);

			cueRoot.visible = false;
			host.addChild(cueRoot);
			log("BWSExamineMenu.swf: Scaleform SCRAP MODS cue attached to host root");
		}

		private function makeBracket(left:Boolean):Sprite
		{
			var s:Sprite = new Sprite();
			s.mouseEnabled = false;
			var c:uint = 0x12FF15;
			s.graphics.lineStyle(2, c, 1.0, true, "normal", "square", "miter");
			if (left)
			{
				s.graphics.moveTo(8, 0);
				s.graphics.lineTo(0, 0);
				s.graphics.lineTo(0, 28);
				s.graphics.lineTo(8, 28);
			}
			else
			{
				s.graphics.moveTo(0, 0);
				s.graphics.lineTo(8, 0);
				s.graphics.lineTo(8, 28);
				s.graphics.lineTo(0, 28);
			}
			return s;
		}

		private function layoutCue():void
		{
			if (!cueRoot || !cueText || !stage)
			{
				return;
			}

			var padX:Number = 10;
			var textW:Number = cueText.textWidth + 4;
			var textH:Number = Math.max(cueText.textHeight + 4, 22);
			cueText.x = 12;
			cueText.y = (28 - textH) * 0.5;

			cueBracketL.x = 0;
			cueBracketL.y = 0;
			cueBracketR.x = cueText.x + textW + padX;
			cueBracketR.y = 0;

			var totalW:Number = cueBracketR.x + 8;
			// Bottom-right, just above the native ButtonBarMenu strip (near BACK).
			var marginR:Number = 56;
			var marginB:Number = 72;
			cueRoot.x = Math.round(stage.stageWidth - totalW - marginR);
			cueRoot.y = Math.round(stage.stageHeight - marginB);
		}

		private function syncCue():void
		{
			if (!bws || !cueRoot)
			{
				return;
			}

			// Keep SetButtonHintData wrap alive for diagnostics.
			if (bar && setWrapper != null && bar.SetButtonHintData != setWrapper)
			{
				origSetNative = bar.SetButtonHintData as Function;
				bar.SetButtonHintData = setWrapper;
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
				log("BWSExamineMenu.swf: cue " + (vis ? "SHOWN" : "hidden"));
			}

			if (vis && key != lastKey)
			{
				lastKey = key;
			}

			cueRoot.visible = vis;
			if (vis)
			{
				cueText.text = key + "  SCRAP MODS";
				layoutCue();
			}
		}

		private function onCueClick(e:MouseEvent):void
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
