/*
 * PlaceholderTextView.cpp - BTextView with placeholder text support
 * Distributed under the terms of the MIT License.
 */

#include "PlaceholderTextView.h"

#include <Window.h>


PlaceholderTextView::PlaceholderTextView(const char* name, uint32 flags)
	:
	BTextView(name, flags),
	fPlaceholder(),
	fTarget(),
	fModificationMessage(NULL),
	fInvokeMessage(NULL)
{
}


PlaceholderTextView::~PlaceholderTextView()
{
	delete fModificationMessage;
	delete fInvokeMessage;
}


void
PlaceholderTextView::Draw(BRect updateRect)
{
	BTextView::Draw(updateRect);
	_DrawPlaceholder();
}


void
PlaceholderTextView::MakeFocus(bool focus)
{
	BTextView::MakeFocus(focus);
	// Redraw to show/hide placeholder text. Also invalidate parent
	// (SearchTextControl) so it redraws the focus ring around us.
	Invalidate();
	if (Parent())
		Parent()->Invalidate();
}


// Recursively find the first PlaceholderTextView under `root` that is
// not `exclude`.  Used by Tab handling to jump between the two search
// fields inside the SearchBarView ancestor without stopping at menu
// internals or buttons that also carry B_NAVIGABLE.
static PlaceholderTextView*
_FindOtherTextView(BView* root, BView* exclude)
{
	for (int32 i = 0; i < root->CountChildren(); i++) {
		BView* child = root->ChildAt(i);
		if (child == exclude)
			continue;
		PlaceholderTextView* ptv = dynamic_cast<PlaceholderTextView*>(child);
		if (ptv != NULL)
			return ptv;
		PlaceholderTextView* found = _FindOtherTextView(child, exclude);
		if (found != NULL)
			return found;
	}
	return NULL;
}


void
PlaceholderTextView::KeyDown(const char* bytes, int32 numBytes)
{
	if (numBytes == 1 && bytes[0] == B_TAB) {
		// Navigate to the other search field within our SearchBarView
		// ancestor instead of inserting a tab character.
		// Commit the current field first (same as pressing Enter) so the
		// search executes before focus moves away.
		if (TextLength() > 0 && fInvokeMessage != NULL && fTarget.IsValid()) {
			BMessage message(*fInvokeMessage);
			fTarget.SendMessage(&message);
		}
		BView* ancestor = Parent();
		while (ancestor != NULL
			&& strcmp(ancestor->Name(), "search_bar") != 0)
			ancestor = ancestor->Parent();
		if (ancestor != NULL) {
			BView* other = _FindOtherTextView(ancestor, this);
			if (other != NULL)
				other->MakeFocus(true);
		}
		return;
	}
	if (numBytes == 1 && bytes[0] == B_ENTER) {
		// Send invoke message on Enter
		if (fInvokeMessage != NULL && fTarget.IsValid()) {
			BMessage message(*fInvokeMessage);
			fTarget.SendMessage(&message);
		}
		return;  // Don't insert newline
	}
	BTextView::KeyDown(bytes, numBytes);
}


void
PlaceholderTextView::InsertText(const char* text, int32 length,
	int32 offset, const text_run_array* runs)
{
	BTextView::InsertText(text, length, offset, runs);
	_SendModificationMessage();
	Invalidate();  // Redraw for placeholder
}


void
PlaceholderTextView::DeleteText(int32 fromOffset, int32 toOffset)
{
	BTextView::DeleteText(fromOffset, toOffset);
	_SendModificationMessage();
	Invalidate();  // Redraw for placeholder
}


void
PlaceholderTextView::SetPlaceholder(const char* placeholder)
{
	fPlaceholder = placeholder;
	Invalidate();
}


void
PlaceholderTextView::SetTarget(BMessenger messenger)
{
	fTarget = messenger;
}


void
PlaceholderTextView::SetModificationMessage(BMessage* message)
{
	delete fModificationMessage;
	fModificationMessage = message;
}


void
PlaceholderTextView::SetInvokeMessage(BMessage* message)
{
	delete fInvokeMessage;
	fInvokeMessage = message;
}


void
PlaceholderTextView::_DrawPlaceholder()
{
	// Only draw placeholder when empty
	if (TextLength() > 0 || fPlaceholder.IsEmpty())
		return;

	// Get text rect and font metrics
	BRect textRect = TextRect();
	font_height fh;
	GetFontHeight(&fh);
	
	// If text rect looks wrong (too narrow), use bounds instead
	// (can happen before layout is fully complete)
	if (textRect.Width() < 10) {
		textRect = Bounds();
		textRect.InsetBy(2, 1);
	}

	// Calculate baseline position
	float y = textRect.top + fh.ascent;

	// Use disabled text color for placeholder
	rgb_color viewColor = ViewColor();
	rgb_color placeholderColor = tint_color(viewColor, B_DISABLED_LABEL_TINT);

	// Draw placeholder text
	SetHighColor(placeholderColor);
	DrawString(fPlaceholder.String(), BPoint(textRect.left, y));
}


void
PlaceholderTextView::_SendModificationMessage()
{
	if (fModificationMessage != NULL && fTarget.IsValid()) {
		BMessage message(*fModificationMessage);
		fTarget.SendMessage(&message);
	}
}
