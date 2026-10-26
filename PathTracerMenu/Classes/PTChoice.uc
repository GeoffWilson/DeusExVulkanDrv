//=============================================================================
// PTChoice
//
// One of the path tracer's settings as a list of choices: each choice the
// value it sets (values), shown as its enumText. A value set other than by
// the menu - in the ini, or with SET - shows as the choice nearest it.
// Nothing is set until the screen's OK, as with the game's own settings.
//=============================================================================

class PTChoice extends MenuUIChoiceEnum
	abstract;

var string settingName;		// The device's ini setting
var bool bBoolSetting;		// True or False in the ini, 1 or 0 here
var int values[40];
var int defaultSetting;		// The device's own default

static function bool PathTracerRunning(PlayerPawn player)
{
	local string quality;

	// Only the path tracer has the setting, and the engine answers a GET of
	// a setting it does not have with words, not a number.
	quality = player.ConsoleCommand("get ini:Engine.Engine.GameRenderDevice DLSSQuality");
	return (quality == "0") || (int(quality) != 0);
}

function string GetSetting(string setting)
{
	return player.ConsoleCommand("get ini:Engine.Engine.GameRenderDevice " $ setting);
}

function SetSetting(string setting, coerce string value)
{
	player.ConsoleCommand("set ini:Engine.Engine.GameRenderDevice " $ setting $ " " $ value);
}

function int ReadSetting()
{
	if (bBoolSetting)
	{
		if (GetSetting(settingName) == "True")
			return 1;
		return 0;
	}
	return int(GetSetting(settingName));
}

function WriteSetting(int value)
{
	if (!bBoolSetting)
		SetSetting(settingName, value);
	else if (value != 0)
		SetSetting(settingName, "True");
	else
		SetSetting(settingName, "False");
}

function ShowNearest(int value)
{
	local int i, best;

	best = 0;
	for (i = 1; (i < arrayCount(enumText)) && (enumText[i] != ""); i++)
		if (Abs(values[i] - value) < Abs(values[best] - value))
			best = i;
	SetValue(best);
}

function LoadSetting()
{
	ShowNearest(ReadSetting());
}

function SaveSetting()
{
	WriteSetting(values[int(GetValue())]);
}

function ResetToDefault()
{
	ShowNearest(defaultSetting);
}

defaultproperties
{
	defaultInfoWidth=98
}
