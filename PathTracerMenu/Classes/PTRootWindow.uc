//=============================================================================
// PTRootWindow
//
// The game's root window, but for the Settings menu it opens: ours, which is
// the game's with a Path Tracer button added. DeusEx.ini names it as the
// root ([Engine.Engine] Root=PathTracerMenu.PTRootWindow), so nothing of the
// game's own needs changing.
//=============================================================================

class PTRootWindow extends DeusExRootWindow;

function DeusExBaseWindow PushWindow(
	Class<DeusExBaseWindow> newWindowClass,
	optional Bool hideCurrentWin,
	optional Bool bNoPause)
{
	if (newWindowClass == Class'MenuSettings')
		newWindowClass = Class'PTMenuSettings';

	return Super.PushWindow(newWindowClass, hideCurrentWin, bNoPause);
}
