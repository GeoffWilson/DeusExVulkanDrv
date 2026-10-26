//=============================================================================
// PTChoice_FSR
//=============================================================================

class PTChoice_FSR extends PTChoice;

defaultproperties
{
	settingName="FSR"
	values(0)=0
	values(1)=1
	values(2)=2
	defaultSetting=1
	enumText(0)="Off"
	enumText(1)="Fallback"
	enumText(2)="Always"
	actionText="|&FSR 3.1 Upscaling"
	helpText="FSR 3.1 upscales NRD where DLSS Ray Reconstruction cannot run (Fallback), or always in its place."
}
