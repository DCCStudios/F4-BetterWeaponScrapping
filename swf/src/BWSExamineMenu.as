package
{
	import flash.display.MovieClip;
	import flash.display.Sprite;
	import flash.events.Event;
	import flash.events.MouseEvent;
	import flash.geom.Point;
	import flash.text.AntiAliasType;
	import flash.text.TextField;
	import flash.text.TextFieldAutoSize;
	import flash.text.TextFormat;
	import flash.text.TextFormatAlign;

	// Document class for BWSExamineMenu.swf — injected into ExamineMenu.swf.
	//
	// Matches the native hint look (e.g. "[ R SCRAP ]"), which is NOT typed
	// bracket characters — it is Shared.AS3.BSBracketClip's BR_HORIZONTAL
	// vector shape (a horizontal tick, a full-height vertical line, another
	// horizontal tick — like a hand-drawn "[") plus a per-hint translucent
	// plate (BSUIComponent.bUseShadedBackground). We reproduce that geometry
	// with our own graphics instead of the host class (different app
	// domain). Does NOT wrap SetButtonHintData.
	public class BWSExamineMenu extends MovieClip
	{
		private var injected:Boolean = false;

		private var base:Object = null;
		private var bws:Object = null;

		private var cueRoot:Sprite = null;
		private var cueBg:Sprite = null;
		private var cueBrackets:Sprite = null;
		private var keyCircle:Sprite = null;   // pad-mode button ring around the key
		private var keyText:TextField = null;  // hotkey ("G" / "L3")
		private var cueText:TextField = null;  // "SCRAP MODS" label
		private var lastVisible:Boolean = false;
		private var lastKey:String = "";
		private var padActive:Boolean = false;

		// The native hint bar's text size is baked into a Flash-authored
		// TextFormat inside the compiled examine_menu.swf (a DefineEditText
		// tag), which isn't visible in the decompiled ActionScript source —
		// there's no numeric constant to read here. 18 read too large, 13
		// read too small; 16 is a visual middle-ground estimate against the
		// reference screenshots, not a re-derived value.
		private static const FONT_SIZE:Number = 16;
		private static const HUD_GREEN:uint = 0x12FF15;

		// BSBracketClip / MessageHolder defaults (see PromptMenuPanel_5.as:
		// bracketCornerLength=6, bracketLineWidth=2, bracketPaddingX=6,
		// bracketPaddingY=2, bUseShadedBackground=true).
		private static const BRACKET_CORNER:Number = 7;
		private static const BRACKET_LINE_WIDTH:Number = 2;
		private static const PAD_X:Number = 7;
		private static const PAD_Y:Number = 3;
		// The native plate (ButtonHintBar_mc.ShadedBackgroundMethod="Shader")
		// is a real-time engine shader that blurs/darkens the game world
		// behind the UI — not a flat fill. Every flat-fill approximation
		// rendered as near-solid black in game regardless of alpha, so the
		// plate is now invisible: alpha 0. The rectangle is still drawn
		// because cueBg doubles as the click hit-area (alpha-0 fills keep
		// their hit-test geometry in Flash).
		private static const BG_ALPHA:Number = 0.0;

		private static const MARGIN_R:Number = 48;
		private static const MARGIN_B:Number = 40;

		// Pad-mode button ring: vanilla controller prompts render the button
		// inside a circle. drawEllipse around the key text approximates the
		// controller-button art (the real art is a font glyph in the host's
		// controller-button font, unreachable from this app domain).
		private static const KEY_GAP:Number = 6;       // key -> label spacing
		private static const CIRCLE_PAD_X:Number = 3;  // ring padding around key text
		private static const CIRCLE_PAD_Y:Number = -1;
		private static const CIRCLE_LINE:Number = 2;
		// Pad-mode key glyph is smaller than the "SCRAP MODS" label; the
		// ring is still measured at FONT_SIZE so shrinking L3 doesn't
		// shrink the circle.
		private static const KEY_FONT_SIZE:Number = 13;
		// Pad-mode key sits a hair above the ring's optical center —
		// TextField glyphs hang slightly low relative to the ellipse.
		private static const KEY_NUDGE_Y:Number = -0.7;

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

			var bar:Object = base["ButtonHintBar_mc"];
			if (!bar || !bar["bAcquiredByNativeCode"])
			{
				return;
			}

			// Feature bitmask from the plugin INI (bit0 = wrap ScrapItem,
			// bit1 = attach cue) — lets the broken-exit bug be bisected
			// between the two injection behaviors without SWF rebuilds.
			var flags:uint = 3;
			try
			{
				flags = uint(bws.GetConfigFlags());
			}
			catch (errFlags:Error)
			{
				flags = 3;
			}

			log("BWSExamineMenu.swf: begin inject (config flags=" + flags + ")");

			if ((flags & 1) != 0)
			{
				wrapScrapItem(codeObj);
				log("BWSExamineMenu.swf: ScrapItem wrapped");
			}
			else
			{
				log("BWSExamineMenu.swf: ScrapItem wrap SKIPPED (bisect)");
			}

			if ((flags & 2) != 0)
			{
				ensureCue(host);
			}
			else
			{
				log("BWSExamineMenu.swf: cue attach SKIPPED (bisect)");
			}

			log("BWSExamineMenu.swf: injection complete");
			injected = true;
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

		private function ensureCue(host:Object):void
		{
			if (cueRoot)
			{
				return;
			}

			cueRoot = new Sprite();
			cueRoot.buttonMode = true;
			cueRoot.mouseEnabled = true;
			cueRoot.mouseChildren = false;

			cueBg = new Sprite();
			cueBg.mouseEnabled = false;
			cueRoot.addChild(cueBg);
			// cueBg is invisible (alpha-0 fill) but still defines the
			// rectangular click hit-area for the whole cue.
			cueRoot.hitArea = cueBg;

			cueBrackets = new Sprite();
			cueBrackets.mouseEnabled = false;
			cueRoot.addChild(cueBrackets);

			keyCircle = new Sprite();
			keyCircle.mouseEnabled = false;
			cueRoot.addChild(keyCircle);

			var fmt:TextFormat = new TextFormat("$MAIN_Font_Bold", FONT_SIZE, HUD_GREEN);
			fmt.align = TextFormatAlign.LEFT;

			keyText = makeCueField(fmt);
			keyText.text = "G";
			cueText = makeCueField(fmt);
			cueText.text = "SCRAP MODS";
			if (cueText.textWidth < 2)
			{
				// $MAIN_Font_Bold unavailable in this app domain — fall back
				// to the device font for BOTH fields so they stay matched.
				fmt.font = "_sans";
				keyText.embedFonts = false;
				keyText.defaultTextFormat = fmt;
				keyText.setTextFormat(fmt);
				keyText.text = "G";
				cueText.embedFonts = false;
				cueText.defaultTextFormat = fmt;
				cueText.setTextFormat(fmt);
				cueText.text = "SCRAP MODS";
				log("BWSExamineMenu.swf: $MAIN_Font_Bold unavailable — using device _sans");
			}

			cueRoot.addChild(keyText);
			cueRoot.addChild(cueText);
			cueRoot.addEventListener(MouseEvent.CLICK, onCueClick);
			cueRoot.visible = false;
			host.addChild(cueRoot);
			log("BWSExamineMenu.swf: vector-bracket SCRAP MODS cue attached");
		}

		// Shared construction for the two cue text fields (key + label) so
		// they always use identical font/AA settings.
		private function makeCueField(fmt:TextFormat):TextField
		{
			var tf:TextField = new TextField();
			tf.autoSize = TextFieldAutoSize.LEFT;
			tf.selectable = false;
			tf.mouseEnabled = false;
			// Matches BSButtonHint.SetUpTextFields — vanilla hints use NORMAL,
			// not ADVANCED, anti-aliasing.
			tf.antiAliasType = AntiAliasType.NORMAL;
			tf.embedFonts = true;
			tf.defaultTextFormat = fmt;
			tf.setTextFormat(fmt);
			return tf;
		}

		private function applyKeySize(size:Number):void
		{
			if (!keyText)
			{
				return;
			}
			var fmt:TextFormat = keyText.defaultTextFormat;
			if (Number(fmt.size) == size)
			{
				return;
			}
			fmt.size = size;
			keyText.defaultTextFormat = fmt;
			keyText.setTextFormat(fmt);
		}

		// Reproduces Shared.AS3.BSBracketClip's BR_HORIZONTAL path: a short
		// horizontal tick at each corner joined by a full-height vertical
		// line on each side — NOT a "[" / "]" font glyph.
		private function drawBrackets(left:Number, top:Number, right:Number, bottom:Number):void
		{
			cueBrackets.graphics.clear();
			cueBrackets.graphics.lineStyle(
				BRACKET_LINE_WIDTH, HUD_GREEN, 1.0, true, "normal", "square", "miter", 3);

			cueBrackets.graphics.moveTo(left + BRACKET_CORNER, top);
			cueBrackets.graphics.lineTo(left, top);
			cueBrackets.graphics.lineTo(left, bottom);
			cueBrackets.graphics.lineTo(left + BRACKET_CORNER, bottom);

			cueBrackets.graphics.moveTo(right - BRACKET_CORNER, bottom);
			cueBrackets.graphics.lineTo(right, bottom);
			cueBrackets.graphics.lineTo(right, top);
			cueBrackets.graphics.lineTo(right - BRACKET_CORNER, top);
		}

		private function layoutCue():void
		{
			if (!cueRoot || !cueText || !keyText || !cueBg || !cueBrackets || !keyCircle || !stage)
			{
				return;
			}

			// Leave room for the vertical bracket line + gap before the key.
			// In pad mode the button ring needs extra clearance on both sides
			// of the key text.
			var ringPad:Number = padActive ? (CIRCLE_PAD_X + CIRCLE_LINE) : 0;
			keyText.x = PAD_X + BRACKET_LINE_WIDTH + 4 + ringPad;
			cueText.y = 0;

			// TextField metrics: text is inset ~2px from the field origin on
			// each axis (the standard gutter), hence the +2 offsets below.
			keyCircle.graphics.clear();
			if (padActive)
			{
				// Measure the ring at FONT_SIZE first so shrinking the L3
				// glyph does not shrink the circle, then center the smaller
				// key inside that fixed ring.
				applyKeySize(FONT_SIZE);
				var kw:Number = keyText.textWidth + CIRCLE_PAD_X * 2;
				var kh:Number = keyText.textHeight + CIRCLE_PAD_Y * 2;
				// Single letters ("A", "B") should sit in a true circle, not
				// a squashed oval — widen to at least the height.
				if (kw < kh)
				{
					kw = kh;
				}
				var kcx:Number = keyText.x + 2 + keyText.textWidth * 0.5;
				var kcy:Number = 2 + keyText.textHeight * 0.5;

				applyKeySize(KEY_FONT_SIZE);
				keyText.x = kcx - 2 - keyText.textWidth * 0.5;
				keyText.y = kcy + KEY_NUDGE_Y - 2 - keyText.textHeight * 0.5;

				keyCircle.graphics.lineStyle(
					CIRCLE_LINE, HUD_GREEN, 1.0, true, "normal", "round");
				keyCircle.graphics.drawEllipse(kcx - kw * 0.5, kcy - kh * 0.5, kw, kh);

				cueText.x = kcx + kw * 0.5 + KEY_GAP;
			}
			else
			{
				applyKeySize(FONT_SIZE);
				keyText.y = 0;
				cueText.x = keyText.x + keyText.textWidth + 4 + KEY_GAP;
			}

			var textW:Number = cueText.textWidth + 4;
			var textH:Number = cueText.textHeight + 4;

			var left:Number = 0;
			var top:Number = -PAD_Y;
			var right:Number = cueText.x + textW + PAD_X;
			var bottom:Number = textH + PAD_Y;

			cueBg.graphics.clear();
			cueBg.graphics.beginFill(0x000000, BG_ALPHA);
			cueBg.graphics.drawRect(left, top, right - left, bottom - top);
			cueBg.graphics.endFill();

			drawBrackets(left, top, right, bottom);

			// cueRoot is a child of `host` (ExamineMenu's root clip), which
			// may itself be scaled/offset relative to the stage (safe-zone
			// insets, aspect-ratio scaling, etc). Using stage.stageWidth
			// directly as a local-space coordinate ignored that transform
			// and let the plate's right bracket run past the visible edge.
			// Converting the desired global anchor point into host's local
			// space via globalToLocal accounts for any such transform.
			var anchorGlobal:Point = new Point(
				stage.stageWidth - MARGIN_R, stage.stageHeight - MARGIN_B);
			var anchorLocal:Point = cueRoot.parent.globalToLocal(anchorGlobal);

			cueRoot.x = Math.round(anchorLocal.x - right);
			cueRoot.y = Math.round(anchorLocal.y - bottom);
		}

		private function syncCue():void
		{
			if (!bws || !cueRoot)
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

			var key:String = "G";
			try
			{
				key = String(bws.GetHintKey());
			}
			catch (errKey:Error)
			{
				key = "G";
			}

			var pad:Boolean = false;
			try
			{
				pad = Boolean(bws.IsGamepadActive());
			}
			catch (errPad:Error)
			{
				pad = false;
			}

			if (vis != lastVisible)
			{
				lastVisible = vis;
				log("BWSExamineMenu.swf: cue " + (vis ? "SHOWN" : "hidden"));
			}

			cueRoot.visible = vis;
			if (vis)
			{
				if (key != lastKey)
				{
					lastKey = key;
					keyText.text = key;
				}
				padActive = pad;
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
