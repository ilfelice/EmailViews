/*
 * SearchBarView.cpp - Filter bar and body search field
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
#include <SeparatorView.h>
#include <Window.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SearchBarView"


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
	BMessage* addQueryMessage,
	BMessage* bodySearchMessage, BMessage* bodyClearMessage)
	:
	BView("search_bar", B_WILL_DRAW | B_FRAME_EVENTS),
	fAttributeMenu(NULL),
	fOperatorMenu(NULL),
	fTextControl(NULL),
	fClearButton(NULL),
	fClearMessage(clearMessage),
	fAddQueryMessage(addQueryMessage),
	fClearIcon(NULL),
	fStopIcon(NULL),
	fAddQueryIcon(NULL),
	fSearchDebounceRunner(NULL),
	fButtonSize(20.0f),
	fSearchExecuted(false),
	fHasResults(false),
	fViewHasContent(false),
	fLoading(false),
	fSettingTextProgrammatically(false),
	fSearchAttribute(SEARCH_SUBJECT),
	fMatchesMode(false),
	fBodySearchControl(NULL),
	fBodyClearButton(NULL),
	fBodySearchMessage(bodySearchMessage),
	fBodyClearMessage(bodyClearMessage),
	fBodyClearIcon(NULL),
	fBodyStopIcon(NULL),
	fBodySearchActive(false)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	// Derive button size from font metrics so the + icon scales
	// with the system font size, matching the search bar's natural height.
	font_height fh0;
	be_plain_font->GetHeight(&fh0);
	fButtonSize = ceilf((fh0.ascent + fh0.descent) * 1.4f);

	// Create attribute popup menu (filter attributes only — no body/fulltext)
	BPopUpMenu* menu = new BPopUpMenu("attributes");
	menu->AddItem(new BMenuItem(B_TRANSLATE("Subject"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->AddItem(new BMenuItem(B_TRANSLATE("From"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->AddItem(new BMenuItem(B_TRANSLATE("To"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->AddItem(new BMenuItem(B_TRANSLATE("Account"), new BMessage(MSG_SEARCH_ATTRIBUTE)));
	menu->ItemAt(0)->SetMarked(true);  // Default to "Subject"

	// Create menu field with "Filter:" label
	fAttributeMenu = new BMenuField("attribute", B_TRANSLATE("Filter:"), menu);
	fAttributeMenu->SetToolTip(B_TRANSLATE("Select attribute to filter by"));

	// Compute the minimum width that can display every menu item without
	// clipping.
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

	// Size to the widest operator label
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

	// Create filter text control with placeholder support
	fTextControl = new SearchTextControl("filter", searchMessage);
	fTextControl->SetModificationMessage(new BMessage('_mod'));
	fTextControl->SetPlaceholder(B_TRANSLATE("Type text and press Enter" B_UTF8_ELLIPSIS));

	// Create filter clear button with icon, flush against text control
	fClearButton = new BButton("clear", "", new BMessage(MSG_CLEAR_BUTTON_CLICKED));
	fClearButton->SetToolTip(B_TRANSLATE("Clear filter"));
	fClearButton->SetEnabled(false);  // Disabled until there's text
	
	// Size the button to match text control height
	font_height fh;
	fTextControl->GetFontHeight(&fh);
	float textCtrlHeight = ceilf(fh.ascent + fh.descent + fh.leading) + 8;
	fClearButton->SetExplicitSize(BSize(textCtrlHeight, textCtrlHeight));

	// Create body search text control
	fBodySearchControl = new SearchTextControl("bodysearch",
		bodySearchMessage);
	fBodySearchControl->SetModificationMessage(new BMessage('_bmd'));
	fBodySearchControl->SetPlaceholder(
		B_TRANSLATE("Type text and press Enter" B_UTF8_ELLIPSIS));

	// Create body search clear button
	fBodyClearButton = new BButton("bodyclear", "",
		new BMessage(MSG_BODY_CLEAR_CLICKED));
	fBodyClearButton->SetToolTip(B_TRANSLATE("Clear search"));
	fBodyClearButton->SetEnabled(false);
	fBodyClearButton->SetExplicitSize(BSize(textCtrlHeight, textCtrlHeight));

	// Create "Search:" label for the body search section
	BStringView* searchLabel = new BStringView("searchLabel",
		B_TRANSLATE("Search:"));

	// Build layout: filter section | separator | search section
	// Right inset reserves space for the + button drawn in Draw()
	float addButtonWidth = fButtonSize + 8;
	BLayoutBuilder::Group<>(this, B_HORIZONTAL, 2)
		// Filter section
		.Add(fAttributeMenu)
		.AddStrut(4)
		.Add(fOperatorMenu)
		.AddStrut(4)
		.AddGroup(B_HORIZONTAL, -2)  // Negative spacing for flush button
			.Add(fTextControl)
			.Add(fClearButton)
		.End()
		.AddStrut(addButtonWidth)
		// Separator
		.Add(new BSeparatorView(B_VERTICAL, B_PLAIN_BORDER))
		// Body search section
		.AddStrut(4)
		.Add(searchLabel)
		.AddStrut(4)
		.AddGroup(B_HORIZONTAL, -2)
			.Add(fBodySearchControl, 1.0f)
			.Add(fBodyClearButton)
		.End();
}


SearchBarView::~SearchBarView()
{
	delete fSearchDebounceRunner;
	delete fClearIcon;
	delete fStopIcon;
	delete fAddQueryIcon;
	delete fBodyClearIcon;
	delete fBodyStopIcon;
	// Note: searchMessage and bodySearchMessage were passed to their
	// respective SearchTextControl/PlaceholderTextView which take ownership.
	// Do NOT delete them here.
	delete fClearMessage;
	delete fAddQueryMessage;
	delete fBodyClearMessage;
}


BSize
SearchBarView::MinSize()
{
	return BSize(100, 26);
}


void
SearchBarView::_LoadIcons()
{
	BResources* resources = be_app->AppResources();
	if (resources == NULL)
		return;

	size_t size;

	// Load clear button icon and set it on the filter clear button
	const void* data = resources->LoadResource(B_VECTOR_ICON_TYPE,
		"ClearButton", &size);
	if (data != NULL) {
		int clearIconSize = 16;
		fClearIcon = new BBitmap(BRect(0, 0, clearIconSize - 1,
			clearIconSize - 1), B_RGBA32);
		if (BIconUtils::GetVectorIcon((const uint8*)data, size,
			fClearIcon) == B_OK) {
			if (fClearButton != NULL)
				fClearButton->SetIcon(fClearIcon);
		} else {
			delete fClearIcon;
			fClearIcon = NULL;
		}
	}

	// Load stop/abort icon (for body search abort)
	data = resources->LoadResource(B_VECTOR_ICON_TYPE, "StopSearch", &size);
	if (data != NULL) {
		int clearIconSize = 16;
		fStopIcon = new BBitmap(BRect(0, 0, clearIconSize - 1,
			clearIconSize - 1), B_RGBA32);
		if (BIconUtils::GetVectorIcon((const uint8*)data, size,
			fStopIcon) != B_OK) {
			delete fStopIcon;
			fStopIcon = NULL;
		}
	}

	// Load add query icon (+)
	int iconSize = (int)fButtonSize - 2;
	data = resources->LoadResource(B_VECTOR_ICON_TYPE, "SearchAddQuery",
		&size);
	if (data != NULL) {
		fAddQueryIcon = new BBitmap(BRect(0, 0, iconSize - 1,
			iconSize - 1), B_RGBA32);
		if (BIconUtils::GetVectorIcon((const uint8*)data, size,
			fAddQueryIcon) != B_OK) {
			delete fAddQueryIcon;
			fAddQueryIcon = NULL;
		}
	}

	// Clone clear icon for body search clear button
	if (fClearIcon != NULL) {
		fBodyClearIcon = new BBitmap(fClearIcon);
		if (fBodyClearButton != NULL)
			fBodyClearButton->SetIcon(fBodyClearIcon);
	}

	// Clone stop icon for body search stop
	if (fStopIcon != NULL)
		fBodyStopIcon = new BBitmap(fStopIcon);
}


void
SearchBarView::AttachedToWindow()
{
	BView::AttachedToWindow();

	fTextControl->SetTarget(this);
	fAttributeMenu->Menu()->SetTargetForItems(this);
	fOperatorMenu->Menu()->SetTargetForItems(this);
	fClearButton->SetTarget(this);
	fBodySearchControl->SetTarget(this);
	fBodyClearButton->SetTarget(this);

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
SearchBarView::MakeFocusBodySearch()
{
	if (fBodySearchControl)
		fBodySearchControl->MakeFocus(true);
}


void
SearchBarView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_COLORS_UPDATED: {
			rgb_color textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
			if (fTextControl != NULL && fTextControl->TextView() != NULL) {
				fTextControl->TextView()->SetFontAndColor(NULL,
					B_FONT_ALL, &textColor);
				fTextControl->TextView()->Invalidate();
			}
			if (fBodySearchControl != NULL
				&& fBodySearchControl->TextView() != NULL) {
				fBodySearchControl->TextView()->SetFontAndColor(NULL,
					B_FONT_ALL, &textColor);
				fBodySearchControl->TextView()->Invalidate();
			}
			break;
		}
		
		case '_mod': {
			// Filter text modification callback
			if (fSettingTextProgrammatically) {
				fSettingTextProgrammatically = false;
				_UpdateClearButtonState();
				break;
			}
			
			Invalidate(_AddQueryButtonRect());
			_UpdateClearButtonState();
			break;
		}

		case '_bmd':
			// Body search text modification callback
			_UpdateBodyClearButtonState();
			break;

		case MSG_SEARCH_MODIFIED:
			// Filter Enter pressed
			fSearchExecuted = HasText();
			Invalidate(_AddQueryButtonRect());
			_UpdateClearButtonState();
			if (Window())
				Window()->PostMessage(message);
			break;

		case MSG_BODY_SEARCH_INVOKED:
			// Body search Enter pressed — forward to window
			if (Window())
				Window()->PostMessage(message);
			break;

		case MSG_SEARCH_ATTRIBUTE: {
			BMenuItem* item = fAttributeMenu->Menu()->FindMarked();
			if (item) {
				int32 index = fAttributeMenu->Menu()->IndexOf(item);
				switch (index) {
					case 0: fSearchAttribute = SEARCH_SUBJECT;  break;
					case 1: fSearchAttribute = SEARCH_FROM;     break;
					case 2: fSearchAttribute = SEARCH_TO;       break;
					case 3: fSearchAttribute = SEARCH_ACCOUNT;  break;
					default: break;
				}
				Invalidate(_AddQueryButtonRect());
			}
			if (HasText() && Window())
				Window()->PostMessage(new BMessage(MSG_SEARCH_MODIFIED));
			break;
		}

		case MSG_SEARCH_OPERATOR: {
			BMenuItem* item = fOperatorMenu->Menu()->FindMarked();
			if (item) {
				int32 index = fOperatorMenu->Menu()->IndexOf(item);
				fMatchesMode = (index == 1);
			}
			if (HasText() && Window())
				Window()->PostMessage(new BMessage(MSG_SEARCH_MODIFIED));
			break;
		}

		case MSG_CLEAR_BUTTON_CLICKED:
			if (fClearMessage && Window())
				Window()->PostMessage(fClearMessage);
			if (fTextControl)
				fTextControl->MakeFocus(true);
			break;

		case MSG_BODY_CLEAR_CLICKED:
			if (fBodyClearMessage && Window())
				Window()->PostMessage(fBodyClearMessage);
			if (fBodySearchControl)
				fBodySearchControl->MakeFocus(true);
			break;

		default:
			BView::MessageReceived(message);
	}
}


BRect
SearchBarView::_AddQueryButtonRect() const
{
	if (fClearButton == NULL)
		return BRect();

	BRect clearFrame = fClearButton->Frame();
	float buttonSize = fButtonSize;
	float left = clearFrame.right + 6;
	BRect bounds = Bounds();
	float centerY = bounds.top + bounds.Height() / 2;

	return BRect(left,
		centerY - buttonSize / 2,
		left + buttonSize,
		centerY + buttonSize / 2);
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


const char*
SearchBarView::BodySearchText() const
{
	return fBodySearchControl ? fBodySearchControl->Text() : "";
}


void
SearchBarView::SetBodySearchText(const char* text)
{
	if (fBodySearchControl)
		fBodySearchControl->SetText(text);
}


BTextView*
SearchBarView::BodySearchTextView() const
{
	return fBodySearchControl ? fBodySearchControl->TextView() : NULL;
}


bool
SearchBarView::HasBodySearchText() const
{
	return fBodySearchControl && fBodySearchControl->TextLength() > 0;
}


void
SearchBarView::SetSearchExecuted(bool executed)
{
	if (fSearchExecuted != executed) {
		fSearchExecuted = executed;
		Invalidate(_AddQueryButtonRect());
		_UpdateClearButtonState();
	}
}


void
SearchBarView::SetHasResults(bool hasResults)
{
	if (fHasResults != hasResults) {
		fHasResults = hasResults;
		Invalidate(_AddQueryButtonRect());
	}
}


void
SearchBarView::SetViewHasContent(bool hasContent)
{
	if (fViewHasContent != hasContent)
		fViewHasContent = hasContent;
}


void
SearchBarView::SetLoading(bool loading)
{
	if (fLoading != loading)
		fLoading = loading;
}


void
SearchBarView::SetBodySearchRunning(bool running)
{
	if (fBodyClearButton == NULL)
		return;

	if (running) {
		fBodySearchActive = true;
		if (fBodyStopIcon != NULL)
			fBodyClearButton->SetIcon(fBodyStopIcon);
		fBodyClearButton->SetToolTip(B_TRANSLATE("Abort search"));
		fBodyClearButton->SetEnabled(true);
	} else {
		if (fBodyClearIcon != NULL)
			fBodyClearButton->SetIcon(fBodyClearIcon);
		fBodyClearButton->SetToolTip(B_TRANSLATE("Clear search"));
		_UpdateBodyClearButtonState();
	}
}


void
SearchBarView::SetBodySearchActive(bool active)
{
	fBodySearchActive = active;
	_UpdateBodyClearButtonState();
}


void
SearchBarView::Draw(BRect updateRect)
{
	BView::Draw(updateRect);

	// Draw add query button (+) - enabled when there's filter text and results
	bool addQueryEnabled = HasText() && fHasResults;
	if (fAddQueryIcon != NULL) {
		BRect addRect = _AddQueryButtonRect();
		if (addRect.IsValid()) {
			SetDrawingMode(B_OP_ALPHA);
			if (!addQueryEnabled) {
				SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
				SetHighColor(0, 0, 0, 64);
			} else {
				SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			}
			DrawBitmap(fAddQueryIcon, addRect.LeftTop());
			SetDrawingMode(B_OP_COPY);
		}
	}
}


void
SearchBarView::MouseDown(BPoint where)
{
	if (HasText() && fHasResults) {
		BRect addRect = _AddQueryButtonRect();
		if (addRect.Contains(where)) {
			if (fAddQueryMessage && Window())
				Window()->PostMessage(fAddQueryMessage);
			return;
		}
	}

	BView::MouseDown(where);
}


bool
SearchBarView::GetToolTipAt(BPoint point, BToolTip** _tip)
{
	if (HasText() && fHasResults) {
		BRect addRect = _AddQueryButtonRect();
		if (addRect.Contains(point)) {
			SetToolTip(B_TRANSLATE("Create query based on filter criteria"));
			return BView::GetToolTipAt(point, _tip);
		}
	}

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

	BMenu* menu = fAttributeMenu->Menu();
	if (menu == NULL)
		return;

	int32 index = -1;
	switch (attr) {
		case SEARCH_SUBJECT:  index = 0; break;
		case SEARCH_FROM:     index = 1; break;
		case SEARCH_TO:       index = 2; break;
		case SEARCH_ACCOUNT:  index = 3; break;
		case SEARCH_THREAD:   index = 0; break;
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
	if (fClearButton != NULL)
		fClearButton->SetEnabled(HasText() || fSearchExecuted);
}


void
SearchBarView::_UpdateBodyClearButtonState()
{
	if (fBodyClearButton != NULL)
		fBodyClearButton->SetEnabled(HasBodySearchText() || fBodySearchActive);
}
