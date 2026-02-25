/*
 * ToolBarView.cpp - Custom toolbar with label-below-icon buttons
 * Distributed under the terms of the MIT License.
 *
 * ToolBarButton draws a 32×32 icon centered above a text label, using
 * BControlLook::DrawButtonFrame/DrawButtonBackground for native appearance.
 * Buttons are flat when idle, show a subtle frame on hover (B_HOVER), and
 * a full pressed look on click (B_ACTIVATED).  Disabled buttons blend
 * label color 50/50 with background for correct light/dark theme support
 * and draw icons at reduced opacity.
 *
 * ToolBarView is a BGroupView that manages a horizontal row of
 * ToolBarButtons, separators, and glue.
 */

#include "ToolBarView.h"

#include <ControlLook.h>
#include <Font.h>
#include <LayoutUtils.h>
#include <SeparatorView.h>
#include <SpaceLayoutItem.h>
#include <Window.h>

#include <algorithm>


static const float kIconSize = 32.0f;
static const float kPaddingH = 8.0f;	// horizontal padding around content
static const float kPaddingTop = 4.0f;	// above icon
static const float kPaddingBottom = 6.0f;	// below label (extra to compensate for icon's built-in top margin)
static const float kIconLabelGap = 1.0f;	// between icon and label

// #pragma mark - ToobarScale

static float
CalcScale()
{
	BFont font{};
	return font.Size() / 12.f;
}


// #pragma mark - ToolBarButton


ToolBarButton::ToolBarButton(const char* name, const BBitmap* icon,
	BMessage* message, BHandler* target)
	:
	BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fIcon(NULL),
	fLabel(),
	fMessage(message),
	fTarget(target),
	fEnabled(true),
	fInside(false),
	fPressed(false),
	fScale(CalcScale()),
	fCachedSize(-1, -1)
{
	if (icon != NULL) {
		fIcon = new BBitmap(icon->Bounds(), icon->ColorSpace());
		memcpy(fIcon->Bits(), icon->Bits(), icon->BitsLength());
	}
	SetViewUIColor(B_MENU_BACKGROUND_COLOR);
	SetLowUIColor(B_MENU_BACKGROUND_COLOR);
}


ToolBarButton::~ToolBarButton()
{
	delete fIcon;
	delete fMessage;
}


void
ToolBarButton::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	rgb_color base = LowColor();
	rgb_color background = ViewColor();

	// Build BControlLook flags — same approach as BButton::Draw()
	uint32 flags = 0;
	if (!fEnabled)
		flags |= BControlLook::B_DISABLED;
	if (fPressed && fInside)
		flags |= BControlLook::B_ACTIVATED;
	// Flat when not interacting, hover when mouse is inside
	if (!fPressed && !fInside)
		flags |= BControlLook::B_FLAT;
	if (fInside)
		flags |= BControlLook::B_HOVER;

	// Draw native button frame and background via BControlLook
	// Note: DrawButtonFrame and DrawButtonBackground modify bounds by
	// reference (insetting it). After both calls, bounds represents the
	// content area inside the frame — use it for centering content.
	be_control_look->DrawButtonFrame(this, bounds, updateRect, base,
		background, flags);
	be_control_look->DrawButtonBackground(this, bounds, updateRect, base,
		flags);

	// Calculate icon position (centered within the content area)
	float iconX = bounds.left + floorf((bounds.Width() - fScale * kIconSize) / 2);
	float iconY = bounds.top + fScale * kPaddingTop;

	// Draw icon
	if (fIcon != NULL) {
		if (!fEnabled) {
			// Draw at reduced opacity for disabled state.
			// B_CONSTANT_ALPHA uses only the alpha channel of the high color
			// to scale the source — RGB values are irrelevant, so this is
			// theme-independent.
			SetDrawingMode(B_OP_ALPHA);
			SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
			SetHighColor(0, 0, 0, 80);
		} else {
			SetDrawingMode(B_OP_ALPHA);
			SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
		}
		DrawBitmap(fIcon, BPoint(iconX, iconY));
		SetDrawingMode(B_OP_COPY);
	}

	// Draw label below icon
	if (fLabel.Length() > 0) {
		font_height fh;
		GetFontHeight(&fh);

		float labelY = iconY + fScale * (kIconSize + kIconLabelGap) + fh.ascent;
		float labelWidth = StringWidth(fLabel.String());
		float labelX = bounds.left + floorf((bounds.Width() - labelWidth) / 2);

		if (fEnabled)
			SetHighColor(ui_color(B_CONTROL_TEXT_COLOR));
		else {
			// Blend text color toward background for a theme-aware disabled look.
			// This works on both light and dark themes, unlike B_DISABLED_LABEL_TINT
			// which always lightens.
			rgb_color textColor = ui_color(B_CONTROL_TEXT_COLOR);
			rgb_color bgColor = LowColor();
			rgb_color disabled;
			disabled.red = (uint8)((textColor.red + bgColor.red) / 2);
			disabled.green = (uint8)((textColor.green + bgColor.green) / 2);
			disabled.blue = (uint8)((textColor.blue + bgColor.blue) / 2);
			disabled.alpha = 255;
			SetHighColor(disabled);
		}
		DrawString(fLabel.String(), BPoint(labelX, labelY));
	}
}


void
ToolBarButton::MouseDown(BPoint where)
{
	if (!fEnabled)
		return;

	fPressed = true;
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	Invalidate();
}


void
ToolBarButton::MouseUp(BPoint where)
{
	if (!fPressed)
		return;

	fPressed = false;

	if (fInside && fEnabled && fMessage != NULL) {
		BMessenger messenger(fTarget);
		messenger.SendMessage(fMessage);
	}

	Invalidate();
}


void
ToolBarButton::MouseMoved(BPoint where, uint32 code, const BMessage* dragMessage)
{
	if (!fEnabled) {
		if (fInside) {
			fInside = false;
			Invalidate();
		}
		return;
	}

	bool inside = (code != B_EXITED_VIEW) && Bounds().Contains(where);
	if (inside != fInside) {
		fInside = inside;
		Invalidate();
	}
}


void
ToolBarButton::AttachedToWindow()
{
	BView::AttachedToWindow();

	// Use UI color constants so the button adapts to theme changes
	SetViewUIColor(B_MENU_BACKGROUND_COLOR);
	SetLowUIColor(B_MENU_BACKGROUND_COLOR);
	SetHighUIColor(B_CONTROL_TEXT_COLOR);
}


BSize
ToolBarButton::MinSize()
{
	return BLayoutUtils::ComposeSize(ExplicitMinSize(), _CalculateSize());
}


BSize
ToolBarButton::MaxSize()
{
	return BLayoutUtils::ComposeSize(ExplicitMaxSize(), _CalculateSize());
}


BSize
ToolBarButton::PreferredSize()
{
	return BLayoutUtils::ComposeSize(ExplicitPreferredSize(), _CalculateSize());
}


void
ToolBarButton::SetLabel(const char* label)
{
	if (label != NULL)
		fLabel = label;
	else
		fLabel = "";
	fCachedSize.Set(-1, -1);
	InvalidateLayout();
	Invalidate();
}


void
ToolBarButton::SetEnabled(bool enabled)
{
	if (fEnabled == enabled)
		return;
	fEnabled = enabled;
	if (!fEnabled) {
		fInside = false;
		fPressed = false;
	}
	Invalidate();
}


void
ToolBarButton::SetToolTip(const char* text)
{
	BView::SetToolTip(text);
}


uint32
ToolBarButton::Command() const
{
	if (fMessage != NULL)
		return fMessage->what;
	return 0;
}


void
ToolBarButton::SetTarget(BHandler* target)
{
	fTarget = target;
}


BSize
ToolBarButton::_CalculateSize()
{
	if (fCachedSize.width >= 0)
		return fCachedSize;

	float width = fScale * (kIconSize + kPaddingH * 2);
	float height = fScale * (kPaddingTop + kIconSize + kPaddingBottom);

	if (fLabel.Length() > 0) {
		font_height fh;
		GetFontHeight(&fh);

		float labelWidth = StringWidth(fLabel.String())
			+ fScale * kPaddingH * 2;

		width = std::max(width, labelWidth);
		height = fScale * (kPaddingTop + kIconSize + kIconLabelGap
			+ kPaddingBottom) + ceilf(fh.ascent + fh.descent);
	}

	fCachedSize.Set(ceilf(width), ceilf(height));
	return fCachedSize;
}


// #pragma mark - ToolBarView


ToolBarView::ToolBarView()
	:
	BGroupView(B_HORIZONTAL)
{
	float inset = ceilf(be_control_look->DefaultItemSpacing() / 2);
	GroupLayout()->SetInsets(inset, 0, inset, 0);
	GroupLayout()->SetSpacing(1);

	SetFlags(Flags() | B_FRAME_EVENTS);

	SetLowUIColor(B_MENU_BACKGROUND_COLOR);
	SetViewUIColor(B_MENU_BACKGROUND_COLOR);
}


ToolBarView::~ToolBarView()
{
}


void
ToolBarView::Hide()
{
	BView::Hide();
	_HideToolTips();
}


void
ToolBarView::AllAttached()
{
	BGroupView::AllAttached();

	// Find the maximum preferred width among all ToolBarButtons
	float maxWidth = 0;
	for (int32 i = 0; BView* view = ChildAt(i); i++) {
		ToolBarButton* button = dynamic_cast<ToolBarButton*>(view);
		if (button == NULL)
			continue;
		float width = button->PreferredSize().width;
		if (width > maxWidth)
			maxWidth = width;
	}

	// Apply uniform width to all buttons
	if (maxWidth > 0) {
		for (int32 i = 0; BView* view = ChildAt(i); i++) {
			ToolBarButton* button = dynamic_cast<ToolBarButton*>(view);
			if (button == NULL)
				continue;
			button->SetExplicitMinSize(BSize(maxWidth, B_SIZE_UNSET));
			button->SetExplicitMaxSize(BSize(maxWidth, B_SIZE_UNSET));
		}
	}
}


void
ToolBarView::AddAction(uint32 command, BHandler* target, const BBitmap* icon,
	const char* toolTipText, const char* text)
{
	ToolBarButton* button = new ToolBarButton(NULL, icon,
		new BMessage(command), target);
	if (toolTipText != NULL)
		button->SetToolTip(toolTipText);
	if (text != NULL)
		button->SetLabel(text);
	GroupLayout()->AddView(button);
}


void
ToolBarView::AddSeparator()
{
	GroupLayout()->AddView(new BSeparatorView(B_VERTICAL, B_PLAIN_BORDER));
}


void
ToolBarView::AddGlue()
{
	GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
}


void
ToolBarView::AddView(BView* view)
{
	GroupLayout()->AddView(view);
}


void
ToolBarView::SetActionEnabled(uint32 command, bool enabled)
{
	if (ToolBarButton* button = FindButton(command))
		button->SetEnabled(enabled);
}


void
ToolBarView::SetActionVisible(uint32 command, bool visible)
{
	ToolBarButton* button = FindButton(command);
	if (button == NULL)
		return;
	for (int32 i = 0; BLayoutItem* item = GroupLayout()->ItemAt(i); i++) {
		if (item->View() != button)
			continue;
		item->SetVisible(visible);
		break;
	}
}


ToolBarButton*
ToolBarView::FindButton(uint32 command) const
{
	for (int32 i = 0; BView* view = ChildAt(i); i++) {
		ToolBarButton* button = dynamic_cast<ToolBarButton*>(view);
		if (button == NULL)
			continue;
		if (button->Command() == command)
			return button;
	}
	return NULL;
}


void
ToolBarView::_HideToolTips() const
{
	for (int32 i = 0; BView* view = ChildAt(i); i++)
		view->HideToolTip();
}
