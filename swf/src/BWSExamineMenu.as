package
{
	import flash.display.MovieClip;
	import flash.display.Sprite;
	import flash.events.Event;
	import flash.events.MouseEvent;
	import flash.filters.BlurFilter;
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
		private var cueText:TextField = null;
		private var lastVisible:Boolean = false;
		private var lastKey:String = "";

		private static const FONT_SIZE:Number = 18;
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
		// behind the UI — not a flat fill. We have no access to that shader
		// from a separately-loaded utility SWF, so this is a deliberate
		// approximation: a soft-edged translucent plate (BlurFilter below)
		// rather than a hard-edged rectangle, which is the closest a plain
		// Sprite fill can get to that look.
		private static const BG_ALPHA:Number = 0.15;
		private static const BG_BLUR:Number = 9;

		private static const MARGIN_R:Number = 48;
		private static const MARGIN_B:Number = 40;

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
			cueBg.filters = [new BlurFilter(BG_BLUR, BG_BLUR, 2)];
			cueRoot.addChild(cueBg);
			// The hit area must stay crisp/rectangular for reliable clicks —
			// hitArea ignores filters anyway, but keep it explicit that this
			// is independent of cueBg's blurred visual.
			cueRoot.hitArea = cueBg;

			cueBrackets = new Sprite();
			cueBrackets.mouseEnabled = false;
			cueRoot.addChild(cueBrackets);

			cueText = new TextField();
			cueText.autoSize = TextFieldAutoSize.LEFT;
			cueText.selectable = false;
			cueText.mouseEnabled = false;
			// Matches BSButtonHint.SetUpTextFields — vanilla hints use NORMAL,
			// not ADVANCED, anti-aliasing.
			cueText.antiAliasType = AntiAliasType.NORMAL;
			cueText.embedFonts = true;
			var fmt:TextFormat = new TextFormat("$MAIN_Font_Bold", FONT_SIZE, HUD_GREEN);
			fmt.align = TextFormatAlign.LEFT;
			cueText.defaultTextFormat = fmt;
			cueText.setTextFormat(fmt);
			cueText.text = "G SCRAP MODS";
			if (cueText.textWidth < 2)
			{
				cueText.embedFonts = false;
				fmt.font = "_sans";
				cueText.defaultTextFormat = fmt;
				cueText.setTextFormat(fmt);
				cueText.text = "G SCRAP MODS";
				log("BWSExamineMenu.swf: $MAIN_Font_Bold unavailable — using device _sans");
			}

			cueRoot.addChild(cueText);
			cueRoot.addEventListener(MouseEvent.CLICK, onCueClick);
			cueRoot.visible = false;
			host.addChild(cueRoot);
			log("BWSExamineMenu.swf: vector-bracket SCRAP MODS cue attached");
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
			if (!cueRoot || !cueText || !cueBg || !cueBrackets || !stage)
			{
				return;
			}

			// Leave room for the vertical bracket line + gap before the text.
			cueText.x = PAD_X + BRACKET_LINE_WIDTH + 4;
			cueText.y = 0;

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

			if (vis != lastVisible)
			{
				lastVisible = vis;
				log("BWSExamineMenu.swf: cue " + (vis ? "SHOWN" : "hidden"));
			}

			cueRoot.visible = vis;
			if (vis)
			{
				var label:String = key + " SCRAP MODS";
				if (key != lastKey || cueText.text != label)
				{
					lastKey = key;
					cueText.text = label;
				}
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
