//=============================================================================
// PTChoice_BumpMapping
//
// The ini's BumpMapping is a percentage of the materials' relief.
//=============================================================================

class PTChoice_BumpMapping extends PTChoice;

defaultproperties
{
	settingName="BumpMapping"
	values(0)=0
	values(1)=50
	values(2)=100
	values(3)=200
	defaultSetting=0
	enumText(0)="Off"
	enumText(1)="Low"
	enumText(2)="Medium"
	enumText(3)="High"
	actionText="Bump |&Mapping"
	helpText="Walls and floors lit by the relief of their textures. Off leaves them flat, as the game draws them."
}
