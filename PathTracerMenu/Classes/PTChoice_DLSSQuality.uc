//=============================================================================
// PTChoice_DLSSQuality
//=============================================================================

class PTChoice_DLSSQuality extends PTChoice;

defaultproperties
{
	settingName="DLSSQuality"
	values(0)=0
	values(1)=1
	values(2)=2
	values(3)=3
	values(4)=4
	defaultSetting=1
	enumText(0)="DLAA"
	enumText(1)="Quality"
	enumText(2)="Balanced"
	enumText(3)="Performance"
	enumText(4)="Ultra Perf."
	actionText="DLSS / FSR |&Quality"
	helpText="How far below the screen's resolution DLSS or FSR traces before upscaling. DLAA traces at full size."
}
