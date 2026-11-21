//=============================================================================
// PTChoice_FSR
//
// When FSR upscales, and which: the ini's FSR (0 off, 1 where Ray
// Reconstruction cannot run, 2 always) and FSRVersion together, the screen
// having no room for another row. The plain choices leave FSRVersion at 4 -
// FSR 4 wherever AMD's DLLs offer it for the card, 3.1 elsewhere - and the
// 3.1 ones set it to 3; here the two are 1 or 2, and 11 or 12. Off leaves
// FSRVersion as it was.
//=============================================================================

class PTChoice_FSR extends PTChoice;

function int ReadSetting()
{
	local int mode;

	mode = int(GetSetting("FSR"));
	if (mode <= 0)
		return 0;
	if (GetSetting("FSRVersion") == "3")
		return Min(mode, 2) + 10;
	return Min(mode, 2);
}

function WriteSetting(int value)
{
	SetSetting("FSR", value % 10);
	if (value >= 10)
		SetSetting("FSRVersion", 3);
	else if (value > 0)
		SetSetting("FSRVersion", 4);
}

defaultproperties
{
	values(0)=0
	values(1)=1
	values(2)=2
	values(3)=11
	values(4)=12
	defaultSetting=1
	enumText(0)="Off"
	enumText(1)="Fallback"
	enumText(2)="Always"
	enumText(3)="Fallback 3.1"
	enumText(4)="Always 3.1"
	actionText="|&FSR Upscaling"
	helpText="Fallback: where DLSS RR can't run. Always: with NRD. FSR 4 on AMD RDNA 2+ cards, else FSR 3.1."
}
