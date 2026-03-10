/*
 * SearchBarView.cpp - Search bar with attribute dropdown and action buttons
 * Distributed under the terms of the MIT License.
 */

#include "SearchBarView.h"
#include "EmailViews.h"

#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <Resources.h>
#include <Window.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SearchBarView"

// Message for backup dots animation
static const uint32 kMsgBackupDotsPulse = 'bkdp';
static const int32 kBackupDotCount = 3;


// ============================================================================
// SearchTextControl - Wrapper that draws a focus-aware border around a
// PlaceholderTextView. This mirrors BTextControl's two-view architecture
// (parent draws border, child is the text view) so we get native-looking
// focus rings while using our custom PlaceholderTextView.
// ============================================================================

SearchTextControl::SearchTextControl(const char* name, BMessage* invokeMessage)
	:
	BView(name, B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE_JUMP),
	fTextView(NULL)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	// Create the placeholder text view as child
	fTextView = new PlaceholderTextView("textview", B_WILL_DRAW | B_NAVIGABLE);
	fTextView->SetStylable(false);
	fTextView->SetWordWrap(false);
	fTextView->DisallowChar(B_ENTER);
	fTextView->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
	fTextView->SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
	fTextView->SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
	
	// Set the text color for typed text
	rgb_color textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
	fTextView->SetFontAndColor(be_plain_font, B_FONT_ALL, &textColor);
	
	fTextView->SetInvokeMessage(invokeMessage);
	AddChild(fTextView);
}


SearchTextControl::~SearchTextControl()
{
}


void
SearchTextControl::AllAttached()
{
	BView::AllAttached();
	_LayoutTextView();
}


void
SearchTextControl::Draw(BRect updateRect)
{
	// Match BTextControl::Draw() exactly
	BRect rect = fTextView->Frame();
	rect.InsetBy(-kFrameMargin, -kFrameMargin);

	rgb_color base = ViewColor();

	uint32 flags = 0;
	if (fTextView->IsFocus() && Window() != NULL && Window()->IsActive())
		flags |= BControlLook::B_FOCUSED;

	be_control_look->DrawTextControlBorder(this, rect, updateRect, base, flags);
}


void
SearchTextControl::MakeFocus(bool focus)
{
	fTextView->MakeFocus(focus);
}


void
SearchTextControl::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	_LayoutTextView();
	Invalidate();
}


BSize
SearchTextControl::MinSize()
{
	font_height fh;
	fTextView->GetFontHeight(&fh);
	float height = ceilf(fh.ascent + fh.descent + fh.leading) + 2 * kFrameMargin + 4;
	return BSize(50, height);
}


BSize
SearchTextControl::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, MinSize().height);
}


BSize
SearchTextControl::PreferredSize()
{
	return BSize(200, MinSize().height);
}


void
SearchTextControl::SetText(const char* text)
{
	fTextView->SetText(text);
}


const char*
SearchTextControl::Text() const
{
	return fTextView->Text();
}


int32
SearchTextControl::TextLength() const
{
	return fTextView->TextLength();
}


void
SearchTextControl::SetModificationMessage(BMessage* message)
{
	fTextView->SetModificationMessage(message);
}


void
SearchTextControl::SetPlaceholder(const char* placeholder)
{
	fTextView->SetPlaceholder(placeholder);
}


void
SearchTextControl::SetTarget(BHandler* target)
{
	if (fTextView != NULL) {
		BMessenger messenger(target);
		fTextView->SetTarget(messenger);
	}
}


void
SearchTextControl::_LayoutTextView()
{
	// Match BTextControl::_LayoutTextView()
	BRect frame = Bounds();
	frame.InsetBy(kFrameMargin, kFrameMargin);
	fTextView->MoveTo(frame.left, frame.top);
	fTextView->ResizeTo(frame.Width(), frame.Height());

	// Set text rect within the text view
	BRect textRect = fTextView->Bounds();
	textRect.InsetBy(2, 1);
	fTextView->SetTextRect(textRect);
}


// ============================================================================
// SearchBarView
// ============================================================================

SearchBarView::SearchBarView(BMessage* searchMessage, BMessage* clearMessage,
	BMessage* addQueryMessage, BMessage* backupMessage)
	:
	BView("search_bar", B_WILL_DRAW | B_FRAME_EVENTS),
	fAttributeMenu(NULL),
	fOperatorMenu(NULL),
	fTextControl(NULL),
	fClearButton(NULL),
	fClearMessage(clearMessage),
	fAddQueryMessage(addQueryMessage),
	fBackupMessage(backupMessage),
	fClearIcon(NULL),
	fStopIcon(NULL),
	fAddQueryIcon(NULL),
	fBackupIcon(NULL),
	fSearchDebounceRunner(NULL),
	fBackupDotsRunner(NULL),
	fButtonSize(20.0f),
	fSearchExecuted(false),
	fHasResults(false),
	fViewHasContent(false),
	fLoading(false),
	fSettingTextProgrammatically(false),
	fSearchAttribute(SEARCH_SUBJECT),
	fMatchesMode(false),
	fBackupActive(false),
	fBackupDot(0)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	// Derive button size from font metrics so the + and ZIP icons scale
	// with the system font size, matching the search bar's natural height.
	// The 1.4 multiplier gives ~20px at the default 12pt font, matching the
	// original hardcoded size while scaling proportionally at larger sizes.
	font_height fh0;
	be_plain_font->GetHeight(&fh0);
	fButtonSize = ceilf((fh0.ascent + fh0.descent) * 1.4f);

	// Create attribute popup menu
	BPopUpMenu* menu = new BPopUpMenu("attributes");
	menu->AddItem(new BMenuItem(B_TRANSLATE("Subject"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->AddItem(new BMenuItem(B_TRANSLATE("From"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->AddItem(new BMenuItem(B_TRANSLATE("To"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->AddItem(new BMenuItem(B_TRANSLATE("Account"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem(B_TRANSLATE("Body"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->AddItem(new BMenuItem(B_TRANSLATE("Full text"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->ItemAt(0)->SetMarked(true);  // Default to "Subject"

	// Create menu field with "Filter by" label
	fAttributeMenu = new BMenuField("attribute", B_TRANSLATE("Filter:"), menu);
	fAttributeMenu->SetToolTip(B_TRANSLATE("Select attribute(s) to query"));

	// Compute the minimum width that can display every menu item without
	// clipping.  PreferredSize() only accounts for the *currently marked*
	// item, so we derive the fixed overhead (label + divider + popup arrow +
	// frame margins) by subtracting the marked item's string width, then add
	// back the widest item's string width.  This is locale-safe and
	// font-size sensitive.
	{
		float markedWidth = menu->FindMarked()
			? be_plain_font->StringWidth(menu->FindMarked()->Label()) : 0;
		float overhead = fAttributeMenu->PreferredSize().width - markedWidth;

		float maxItemWidth = 0;
		for (int32 i = 0; i < menu->CountItems(); i++) {
			float w = be_plain_font->StringWidth(menu->ItemAt(i)->Label());
			if (w > maxItemWidth)
				maxItemWidth = w;
		}

		float menuWidth = ceilf(overhead + maxItemWidth);
		fAttributeMenu->SetExplicitMinSize(BSize(menuWidth, B_SIZE_UNSET));
		fAttributeMenu->SetExplicitMaxSize(BSize(menuWidth, B_SIZE_UNSET));
	}

	// Create operator popup menu
	BPopUpMenu* operatorMenu = new BPopUpMenu("operator");
	operatorMenu->AddItem(new BMenuItem(B_TRANSLATE("contains"), new BMessage(MSG_SEARCH_OPERATOR)));
	operatorMenu->AddItem(new BMenuItem(B_TRANSLATE("matches"), new BMessage(MSG_SEARCH_OPERATOR)));
	operatorMenu->ItemAt(0)->SetMarked(true);  // Default to "contains"

	// Create menu field without label (operator only)
	fOperatorMenu = new BMenuField("operator", NULL, operatorMenu);
	fOperatorMenu->SetToolTip(B_TRANSLATE("Select search operator"));

	// Same technique: size to the widest operator label
	{
		float markedWidth = operatorMenu->FindMarked()
			? be_plain_font->StringWidth(operatorMenu->FindMarked()->Label()) : 0;
		float overhead = fOperatorMenu->PreferredSize().width - markedWidth;

		float maxItemWidth = 0;
		for (int32 i = 0; i < operatorMenu->CountItems(); i++) {
			float w = be_plain_font->StringWidth(operatorMenu->ItemAt(i)->Label());
			if (w > maxItemWidth)
				maxItemWidth = w;
		}

		float operatorMenuWidth = ceilf(overhead + maxItemWidth);
		fOperatorMenu->SetExplicitMinSize(BSize(operatorMenuWidth, B_SIZE_UNSET));
		fOperatorMenu->SetExplicitMaxSize(BSize(operatorMenuWidth, B_SIZE_UNSET));
	}

	// Create text control with placeholder support
	fTextControl = new SearchTextControl("search", searchMessage);
	fTextControl->SetModificationMessage(new BMessage('_mod'));
	fTextControl->SetPlaceholder(B_TRANSLATE("Type text and press Enter" B_UTF8_ELLIPSIS));

	// Create clear button with icon, flush against text control
	fClearButton = new BButton("clear", "", new BMessage(MSG_CLEAR_BUTTON_CLICKED));
	fClearButton->SetToolTip(B_TRANSLATE("Clear search box"));
	fClearButton->SetEnabled(false);  // Disabled until there's text
	
	// Size the button to match text control height
	font_height fh;
	fTextControl->GetFontHeight(&fh);
	float textCtrlHeight = ceilf(fh.ascent + fh.descent + fh.leading) + 8;
	fClearButton->SetExplicitSize(BSize(textCtrlHeight, textCtrlHeight));

	// Use horizontal layout with right inset for icon buttons (2 buttons now: + and ZIP)
	float buttonsWidth = (fButtonSize + 4) * 2 + 4;
	BLayoutBuilder::Group<>(this, B_HORIZONTAL, 2)
		.Add(fAttributeMenu)
		.AddStrut(4)
		.Add(fOperatorMenu)
		.AddStrut(4)
		.AddGroup(B_HORIZONTAL, -2)  // Negative spacing for flush button
			.Add(fTextControl)
			.Add(fClearButton)
		.End()
		.AddStrut(6)
		.SetInsets(0, 0, buttonsWidth, 0);
}


SearchBarView::~SearchBarView()
{
	delete fSearchDebounceRunner;
	delete fBackupDotsRunner;
	delete fClearIcon;
	delete fStopIcon;
	delete fAddQueryIcon;
	delete fBackupIcon;
	// Note: searchMessage was passed to fTextControl which takes ownership
	// fClearMessage, fAddQueryMessage, and fBackupMessage are stored by us, so we delete them
	delete fClearMessage;
	delete fAddQueryMessage;
	delete fBackupMessage;
}


BSize
SearchBarView::MinSize()
{
	// Return a small minimum to allow the window to resize narrower.
	// The search bar will truncate/clip when narrower than preferred size.
	return BSize(100, 26);
}


void
SearchBarView::_LoadIcons()
{
	BResources* resources = be_app->AppResources();
	if (resources == NULL)
		return;

	size_t size;
	int iconSize = (int)fButtonSize - 2;

	// Load clear button icon and set it on the button
	const void* data = resources->LoadResource(B_VECTOR_ICON_TYPE, "ClearButton", &size);
	if (data != NULL) {
		// Use a slightly smaller icon for the button (to fit nicely)
		int clearIconSize = 16;
		fClearIcon = new BBitmap(BRect(0, 0, clearIconSize - 1, clearIconSize - 1), B_RGBA32);
		if (BIconUtils::GetVectorIcon((const uint8*)data, size, fClearIcon) == B_OK) {
			if (fClearButton != NULL)
				fClearButton->SetIcon(fClearIcon);
		} else {
			delete fClearIcon;
			fClearIcon = NULL;
		}
	}

	// Load stop/abort icon (same size as clear icon)
	data = resources->LoadResource(B_VECTOR_ICON_TYPE, "StopSearch", &size);
	if (data != NULL) {
		int clearIconSize = 16;
		fStopIcon = new BBitmap(BRect(0, 0, clearIconSize - 1, clearIconSize - 1), B_RGBA32);
		if (BIconUtils::GetVectorIcon((const uint8*)data, size, fStopIcon) != B_OK) {
			delete fStopIcon;
			fStopIcon = NULL;
		}
	}

	data = resources->LoadResource(B_VECTOR_ICON_TYPE, "SearchAddQuery", &size);
	if (data != NULL) {
		fAddQueryIcon = new BBitmap(BRect(0, 0, iconSize - 1, iconSize - 1), B_RGBA32);
		if (BIconUtils::GetVectorIcon((const uint8*)data, size, fAddQueryIcon) != B_OK) {
			delete fAddQueryIcon;
			fAddQueryIcon = NULL;
		}
	}

	data = resources->LoadResource(B_VECTOR_ICON_TYPE, "BackupEmails", &size);
	if (data != NULL) {
		fBackupIcon = new BBitmap(BRect(0, 0, iconSize - 1, iconSize - 1), B_RGBA32);
		if (BIconUtils::GetVectorIcon((const uint8*)data, size, fBackupIcon) != B_OK) {
			delete fBackupIcon;
			fBackupIcon = NULL;
		}
	}
}


void
SearchBarView::AttachedToWindow()
{
	BView::AttachedToWindow();

	// Set targets
	fTextControl->SetTarget(this);
	fAttributeMenu->Menu()->SetTargetForItems(this);
	fOperatorMenu->Menu()->SetTargetForItems(this);
	fClearButton->SetTarget(this);

	_LoadIcons();
}


void
SearchBarView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	Invalidate();
}


void
SearchBarView::MakeFocus(bool focus)
{
	if (fTextControl)
		fTextControl->MakeFocus(focus);
}


void
SearchBarView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_COLORS_UPDATED: {
			// System colors changed - update text color
			if (fTextControl != NULL && fTextControl->TextView() != NULL) {
				rgb_color textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
				fTextControl->TextView()->SetFontAndColor(NULL, B_FONT_ALL, &textColor);
				fTextControl->TextView()->Invalidate();
			}
			break;
		}
		
		case '_mod': {
			// Text modification callback from PlaceholderTextView.
			// fSettingTextProgrammatically distinguishes user typing from
			// programmatic SetText() calls (e.g., restoring a custom query's
			// search text). User typing clears "search executed" state so the
			// [+] button disables until Enter is pressed again.
			if (fSettingTextProgrammatically) {
				fSettingTextProgrammatically = false;  // Clear the flag here
				_UpdateClearButtonState();
				break;
			}
			
			bool wasSearchExecuted = fSearchExecuted;
			fSearchExecuted = false;
			Invalidate(_AddQueryButtonRect());
			_UpdateClearButtonState();

			// Auto-reset view when text becomes empty after a search was executed
			if (!HasText() && wasSearchExecuted && Window())
				Window()->PostMessage(new BMessage(MSG_SEARCH_MODIFIED));
			break;
		}

		case MSG_SEARCH_MODIFIED:
			// Enter pressed - search executed
			fSearchExecuted = HasText();
			Invalidate(_AddQueryButtonRect());
			_UpdateClearButtonState();
			if (Window())
				Window()->PostMessage(message);
			break;

		case MSG_SEARCH_ATTRIBUTE: {
			// Attribute selection changed
			BMenuItem* item = fAttributeMenu->Menu()->FindMarked();
			if (item) {
				int32 index = fAttributeMenu->Menu()->IndexOf(item);
				// Menu layout: Subject(0), From(1), To(2), Account(3),
				//              separator(4), Body(5), Full text(6)
				switch (index) {
					case 0: fSearchAttribute = SEARCH_SUBJECT;  break;
					case 1: fSearchAttribute = SEARCH_FROM;     break;
					case 2: fSearchAttribute = SEARCH_TO;       break;
					case 3: fSearchAttribute = SEARCH_ACCOUNT;  break;
					case 5: fSearchAttribute = SEARCH_BODY;     break;
					case 6: fSearchAttribute = SEARCH_FULLTEXT; break;
					default: break;
				}
				// Redraw + button immediately (disabled for body/full-text)
				Invalidate(_AddQueryButtonRect());
			}
			// Re-execute search if there's text - post MSG_SEARCH_MODIFIED
			if (HasText() && Window())
				Window()->PostMessage(new BMessage(MSG_SEARCH_MODIFIED));
			break;
		}

		case MSG_SEARCH_OPERATOR: {
			// Operator selection changed
			BMenuItem* item = fOperatorMenu->Menu()->FindMarked();
			if (item) {
				int32 index = fOperatorMenu->Menu()->IndexOf(item);
				// Index 0 = "that contains" (false), Index 1 = "matches" (true)
				fMatchesMode = (index == 1);
			}
			// Re-execute search if there's text - post MSG_SEARCH_MODIFIED
			if (HasText() && Window())
				Window()->PostMessage(new BMessage(MSG_SEARCH_MODIFIED));
			break;
		}

		case MSG_CLEAR_BUTTON_CLICKED:
			// Clear button pressed - forward to window as clear message
			if (fClearMessage && Window())
				Window()->PostMessage(fClearMessage);
			// Return focus to search box
			if (fTextControl)
				fTextControl->MakeFocus(true);
			break;

		case kMsgBackupDotsPulse:
			if (fBackupActive) {
				fBackupDot = (fBackupDot + 1) % kBackupDotCount;
				Invalidate(_BackupButtonRect());
			}
			break;

		default:
			BView::MessageReceived(message);
	}
}


BRect
SearchBarView::_AddQueryButtonRect() const
{
	BRect bounds = Bounds();
	float buttonSize = fButtonSize;
	float buttonsWidth = (buttonSize + 4) * 2;  // 2 icon buttons now (+ and ZIP)

	// Position the + button in the button area
	float left = bounds.right - buttonsWidth + 4;
	float centerY = bounds.top + bounds.Height() / 2;

	return BRect(left,
		centerY - buttonSize / 2,
		left + buttonSize,
		centerY + buttonSize / 2);
}


BRect
SearchBarView::_BackupButtonRect() const
{
	BRect addRect = _AddQueryButtonRect();
	if (!addRect.IsValid())
		return BRect();

	float buttonSize = addRect.Height();
	float left = addRect.right + 4;

	return BRect(left,
		addRect.top,
		left + buttonSize,
		addRect.bottom);
}


const char*
SearchBarView::Text() const
{
	return fTextControl ? fTextControl->Text() : "";
}


void
SearchBarView::SetText(const char* text)
{
	if (fTextControl) {
		// Set flag so _mod handler knows this is programmatic, not user typing
		// Flag is cleared in _mod handler (async) not here
		fSettingTextProgrammatically = true;
		fTextControl->SetText(text);
	}
}



BTextView*
SearchBarView::TextView() const
{
	return fTextControl ? fTextControl->TextView() : NULL;
}


bool
SearchBarView::HasText() const
{
	return fTextControl && fTextControl->TextLength() > 0;
}


void
SearchBarView::SetSearchExecuted(bool executed)
{
	if (fSearchExecuted != executed) {
		fSearchExecuted = executed;
		Invalidate(_AddQueryButtonRect());
		Invalidate(_BackupButtonRect());
	}
}


void
SearchBarView::SetHasResults(bool hasResults)
{
	if (fHasResults != hasResults) {
		fHasResults = hasResults;
		Invalidate(_AddQueryButtonRect());
		Invalidate(_BackupButtonRect());
	}
}


void
SearchBarView::SetViewHasContent(bool hasContent)
{
	if (fViewHasContent != hasContent) {
		fViewHasContent = hasContent;
		Invalidate(_BackupButtonRect());
	}
}


void
SearchBarView::SetLoading(bool loading)
{
	if (fLoading != loading) {
		fLoading = loading;
		Invalidate(_BackupButtonRect());
	}
}


void
SearchBarView::SetBackupActive(bool active)
{
	if (fBackupActive == active)
		return;

	fBackupActive = active;
	fBackupDot = 0;

	if (active) {
		delete fBackupDotsRunner;
		BMessage msg(kMsgBackupDotsPulse);
		fBackupDotsRunner = new BMessageRunner(BMessenger(this), &msg, 450000);
	} else {
		delete fBackupDotsRunner;
		fBackupDotsRunner = NULL;
	}

	Invalidate(_BackupButtonRect());
}


void
SearchBarView::SetBodySearchRunning(bool running)
{
	if (fClearButton == NULL)
		return;

	if (running) {
		if (fStopIcon != NULL)
			fClearButton->SetIcon(fStopIcon);
		fClearButton->SetToolTip(B_TRANSLATE("Abort search"));
		fClearButton->SetEnabled(true);
	} else {
		if (fClearIcon != NULL)
			fClearButton->SetIcon(fClearIcon);
		fClearButton->SetToolTip(B_TRANSLATE("Clear search box"));
		_UpdateClearButtonState();  // Re-evaluate enabled state based on text
	}
}


void
SearchBarView::Draw(BRect updateRect)
{
	BView::Draw(updateRect);

	// Draw add query button (+) - enabled when there's text and results,
	// but disabled for body/full-text search (can't be saved as a BFS query)
	bool addQueryEnabled = HasText() && fHasResults && !IsBodySearch();
	if (fAddQueryIcon != NULL) {
		BRect addRect = _AddQueryButtonRect();
		if (addRect.IsValid()) {
			SetDrawingMode(B_OP_ALPHA);
			if (!addQueryEnabled) {
				// Draw with reduced opacity when disabled
				SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
				SetHighColor(0, 0, 0, 64);  // 25% opacity for disabled state
			} else {
				// Reset to normal blending for enabled state
				SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			}
			DrawBitmap(fAddQueryIcon, addRect.LeftTop());
			SetDrawingMode(B_OP_COPY);
		}
	}

	// Draw backup button (ZIP) - enabled if view has any content
	bool backupEnabled = fViewHasContent && !fBackupActive && !fLoading;
	if (fBackupIcon != NULL) {
		BRect backupRect = _BackupButtonRect();
		if (backupRect.IsValid()) {
			SetDrawingMode(B_OP_ALPHA);
			if (fBackupActive) {
				// Backup in progress: draw dimmed icon
				SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
				SetHighColor(0, 0, 0, 80);  // ~30% opacity
			} else if (!backupEnabled) {
				// Draw with reduced opacity when disabled
				SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
				SetHighColor(0, 0, 0, 64);  // 25% opacity for disabled state
			} else {
				// Reset to normal blending for enabled state
				SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			}
			DrawBitmap(fBackupIcon, backupRect.LeftTop());
			SetDrawingMode(B_OP_COPY);

			// Draw animated dots over the dimmed icon during backup
			if (fBackupActive) {
				rgb_color bg = ui_color(B_PANEL_BACKGROUND_COLOR);
				rgb_color dimColor = tint_color(bg, B_DARKEN_2_TINT);
				rgb_color litColor = ui_color(B_STATUS_BAR_COLOR);

				float dotSize = floorf(backupRect.Height() * 0.18f);
				float spacing = dotSize * 2.0f;
				float totalWidth = kBackupDotCount * dotSize
					+ (kBackupDotCount - 1) * (spacing - dotSize);
				float startX = backupRect.left
					+ (backupRect.Width() - totalWidth) / 2.0f - 2.0f;
				float centerY = backupRect.top + backupRect.Height() / 2.0f;

				for (int32 i = 0; i < kBackupDotCount; i++) {
					float cx = startX + i * spacing + dotSize / 2.0f;
					SetHighColor(i == fBackupDot ? litColor : dimColor);
					FillEllipse(BPoint(cx, centerY),
						dotSize / 2.0f, dotSize / 2.0f);
				}
			}
		}
	}
}


void
SearchBarView::MouseDown(BPoint where)
{
	// Check add query button first - enabled when there's text and results
	// (disabled for body/full-text search — can't be saved as a BFS query)
	if (HasText() && fHasResults && !IsBodySearch()) {
		BRect addRect = _AddQueryButtonRect();
		if (addRect.Contains(where)) {
			if (fAddQueryMessage && Window()) {
				Window()->PostMessage(fAddQueryMessage);
			}
			return;
		}
	}

	// Check backup button - enabled if view has any content and not already backing up
	if (fViewHasContent && !fBackupActive && !fLoading) {
		BRect backupRect = _BackupButtonRect();
		if (backupRect.Contains(where)) {
			if (fBackupMessage && Window()) {
				Window()->PostMessage(fBackupMessage);
			}
			return;
		}
	}

	BView::MouseDown(where);
}


bool
SearchBarView::GetToolTipAt(BPoint point, BToolTip** _tip)
{
	// Check if over add query button - only show tooltip if enabled
	if (HasText() && fHasResults && !IsBodySearch()) {
		BRect addRect = _AddQueryButtonRect();
		if (addRect.Contains(point)) {
			SetToolTip(B_TRANSLATE("Create query based on search criteria"));
			return BView::GetToolTipAt(point, _tip);
		}
	}

	// Check if over backup button - show tooltip
	if (fViewHasContent || fBackupActive) {
		BRect backupRect = _BackupButtonRect();
		if (backupRect.Contains(point)) {
			if (fBackupActive)
				SetToolTip(B_TRANSLATE("Backup in progress" B_UTF8_ELLIPSIS));
			else
				SetToolTip(B_TRANSLATE("Backup emails to ZIP file"));
			return BView::GetToolTipAt(point, _tip);
		}
	}

	// Not over buttons - no tooltip from this view
	// (Clear button has its own tooltip set via BButton::SetToolTip)
	SetToolTip((const char*)NULL);
	return false;
}


void
SearchBarView::SetMatchesMode(bool matches)
{
	if (fMatchesMode != matches) {
		fMatchesMode = matches;
		_UpdateOperatorLabel();
	}
}


void
SearchBarView::SetSearchAttribute(SearchAttribute attr)
{
	fSearchAttribute = attr;

	// Update menu selection to match
	BMenu* menu = fAttributeMenu->Menu();
	if (menu == NULL)
		return;

	int32 index = -1;
	switch (attr) {
		case SEARCH_SUBJECT:  index = 0; break;
		case SEARCH_FROM:     index = 1; break;
		case SEARCH_TO:       index = 2; break;
		case SEARCH_ACCOUNT:  index = 3; break;
		// index 4 = separator
		case SEARCH_BODY:     index = 5; break;
		case SEARCH_FULLTEXT: index = 6; break;
		case SEARCH_THREAD:   index = 0; break;  // Show "Subject" for thread filtering
		default: break;
	}

	if (index >= 0 && index < menu->CountItems()) {
		BMenuItem* item = menu->ItemAt(index);
		if (item != NULL)
			item->SetMarked(true);
	}
}


void
SearchBarView::_UpdateOperatorLabel()
{
	if (fOperatorMenu != NULL) {
		BMenu* menu = fOperatorMenu->Menu();
		if (menu != NULL) {
			// Index 0 = "that contains", Index 1 = "matches"
			int32 index = fMatchesMode ? 1 : 0;
			BMenuItem* item = menu->ItemAt(index);
			if (item != NULL)
				item->SetMarked(true);
		}
	}
}


void
SearchBarView::_UpdateClearButtonState()
{
	if (fClearButton != NULL) {
		fClearButton->SetEnabled(HasText());
	}
}
