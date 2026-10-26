//=============================================================================
// PTChoice_Bounces
//=============================================================================

class PTChoice_Bounces extends PTChoice;

defaultproperties
{
	settingName="Bounces"
	values(0)=1
	values(1)=2
	values(2)=3
	values(3)=4
	values(4)=5
	values(5)=6
	values(6)=8
	defaultSetting=3
	enumText(0)="1"
	enumText(1)="2"
	enumText(2)="3"
	enumText(3)="4"
	enumText(4)="5"
	enumText(5)="6"
	enumText(6)="8"
	actionText="Light |&Bounces"
	helpText="How many times light bounces. More fills the shadows more fully, at a cost to the frame rate."
}
