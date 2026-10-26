//=============================================================================
// PTChoice_Wetness
//
// The ini's Wetness is a percentage.
//=============================================================================

class PTChoice_Wetness extends PTChoice;

defaultproperties
{
	settingName="Wetness"
	values(0)=0
	values(1)=25
	values(2)=50
	values(3)=75
	values(4)=100
	defaultSetting=0
	enumText(0)="Dry"
	enumText(1)="Damp"
	enumText(2)="Wet"
	enumText(3)="Wetter"
	enumText(4)="Soaked"
	actionText="Wet |&Streets"
	helpText="Ground open to the sky shining as after rain, with puddles reflecting the neon."
}
