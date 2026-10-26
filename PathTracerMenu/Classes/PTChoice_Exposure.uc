//=============================================================================
// PTChoice_Exposure
//
// The ini's Exposure is 0 to 255 for 0.2 to 2.2; shown as the factor.
//=============================================================================

class PTChoice_Exposure extends PTChoice;

defaultproperties
{
	settingName="Exposure"
	values(0)=51
	values(1)=64
	values(2)=76
	values(3)=90
	values(4)=102
	values(5)=115
	values(6)=127
	values(7)=140
	values(8)=153
	values(9)=166
	defaultSetting=90
	enumText(0)="0.6"
	enumText(1)="0.7"
	enumText(2)="0.8"
	enumText(3)="0.9"
	enumText(4)="1.0"
	enumText(5)="1.1"
	enumText(6)="1.2"
	enumText(7)="1.3"
	enumText(8)="1.4"
	enumText(9)="1.5"
	actionText="|&Exposure"
	helpText="How bright the traced picture is made before its tone curve. 0.9 is the default."
}
