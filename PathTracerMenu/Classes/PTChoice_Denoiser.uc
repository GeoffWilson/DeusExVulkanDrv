//=============================================================================
// PTChoice_Denoiser
//
// Off, NRD, or DLSS Ray Reconstruction: the ini's Denoise and DLSS together.
//=============================================================================

class PTChoice_Denoiser extends PTChoice;

function int ReadSetting()
{
	if (GetSetting("Denoise") != "True")
		return 0;
	if (GetSetting("DLSS") != "True")
		return 1;
	return 2;
}

function WriteSetting(int value)
{
	if (value > 0)
		SetSetting("Denoise", "True");
	else
		SetSetting("Denoise", "False");
	if (value == 2)
		SetSetting("DLSS", "True");
	else
		SetSetting("DLSS", "False");
}

defaultproperties
{
	values(0)=0
	values(1)=1
	values(2)=2
	defaultSetting=2
	enumText(0)="Off"
	enumText(1)="NRD"
	enumText(2)="DLSS RR"
	actionText="|&Denoiser"
	helpText="DLSS Ray Reconstruction on NVIDIA RTX cards, or NRD on any card. Off shows the raw, noisy trace."
}
