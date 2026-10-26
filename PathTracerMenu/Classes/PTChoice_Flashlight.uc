//=============================================================================
// PTChoice_Flashlight
//=============================================================================

class PTChoice_Flashlight extends PTChoice;

defaultproperties
{
	settingName="Flashlight"
	bBoolSetting=True
	values(0)=0
	values(1)=1
	defaultSetting=1
	enumText(0)="Off"
	enumText(1)="On"
	actionText="F|&lashlight"
	helpText="The light augmentation as a real flashlight with traced shadows, in place of the game's patch of light."
}
