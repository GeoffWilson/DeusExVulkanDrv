//=============================================================================
// PTMenuButtonWindow
//
// A menu button that brings its own slot. The game's buttons are drawn
// translucent over dark slots in the menu's picture, and the Path Tracer
// button stands where the picture has none: on the bare metal it washed out.
// The button's shape is drawn in black beneath it first, as a slot.
//=============================================================================

class PTMenuButtonWindow extends MenuUIMenuButtonWindow;

event DrawWindow(GC gc)
{
	local int leftWidth, rightWidth;

	SetButtonMetrics();
	leftWidth = Left_Textures[textureIndex].width;
	rightWidth = Right_Textures[textureIndex].width;

	gc.SetStyle(DSTY_Masked);
	gc.SetTileColorRGB(0, 0, 0);
	gc.DrawTexture(leftOffset, buttonVerticalOffset, leftWidth, buttonHeight, 0, 0, Left_Textures[textureIndex].tex);
	gc.DrawPattern(leftOffset + leftWidth, buttonVerticalOffset, width - leftWidth - rightWidth - leftOffset, buttonHeight, 0, 0, Center_Textures[textureIndex].tex);
	gc.DrawTexture(width - rightWidth, buttonVerticalOffset, rightWidth, buttonHeight, 0, 0, Right_Textures[textureIndex].tex);

	Super.DrawWindow(gc);
}
