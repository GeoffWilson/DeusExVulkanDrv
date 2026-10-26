//=============================================================================
// PTMenuSettings
//
// The game's Settings menu, with Path Tracer after Sound, in the gap the
// menu's picture leaves above Previous Menu. Only while the path tracer is
// the renderer: the others have none of its settings.
//=============================================================================

class PTMenuSettings extends MenuSettings;

var localized string PathTracerButtonName;
var int pathTracerButton;

event InitWindow()
{
	local int last;

	// Before Super.InitWindow, which is what sets player.
	if (Class'PTChoice'.static.PathTracerRunning(DeusExRootWindow(GetRootWindow()).parentPawn))
	{
		// Previous Menu is the last of the game's buttons; Path Tracer takes
		// its place, a button's step below the one before, and it moves down.
		for (last = 0; last < arrayCount(ButtonNames) - 1; last++)
			if (ButtonNames[last + 1] == "")
				break;

		if ((last >= 2) && (last < arrayCount(ButtonNames) - 1))
		{
			ButtonNames[last + 1] = ButtonNames[last];
			buttonDefaults[last + 1] = buttonDefaults[last];
			ButtonNames[last] = PathTracerButtonName;
			buttonDefaults[last].y = buttonDefaults[last - 1].y + (buttonDefaults[last - 1].y - buttonDefaults[last - 2].y);
			buttonDefaults[last].action = MA_MenuScreen;
			buttonDefaults[last].invoke = Class'PTMenuScreen';
			pathTracerButton = last;
		}
	}

	Super.InitWindow();
}

// The game's buttons, with ours in its own kind of button (PTMenuButtonWindow).
function CreateMenuButtons()
{
	Super.CreateMenuButtons();

	if (pathTracerButton >= 0)
	{
		winButtons[pathTracerButton].Destroy();
		winButtons[pathTracerButton] = MenuUIMenuButtonWindow(winClient.NewChild(Class'PTMenuButtonWindow'));
		winButtons[pathTracerButton].SetButtonText(ButtonNames[pathTracerButton]);
		winButtons[pathTracerButton].SetPos(buttonXPos, buttonDefaults[pathTracerButton].y);
		winButtons[pathTracerButton].SetWidth(buttonWidth);
	}
}

defaultproperties
{
	PathTracerButtonName="Path Tracer"
	pathTracerButton=-1
}
