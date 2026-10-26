//=============================================================================
// PTMenuScreen
//
// The path tracer's settings, laid out as the game's Display settings are
// and on their picture. Each is the device's own ini setting, set with SET as
// the Display settings set theirs; the device takes them at its next frame.
//=============================================================================

class PTMenuScreen extends MenuScreenDisplay;

defaultproperties
{
	choices(0)=Class'PTChoice_Denoiser'
	choices(1)=Class'PTChoice_DLSSQuality'
	choices(2)=Class'PTChoice_FSR'
	choices(3)=Class'PTChoice_Bounces'
	choices(4)=Class'PTChoice_Exposure'
	choices(5)=Class'PTChoice_Flashlight'
	choices(6)=Class'PTChoice_BumpMapping'
	choices(7)=Class'PTChoice_Wetness'
	choices(8)=Class'PTChoice_HDR'
	Title="Path Tracer Settings"
}
