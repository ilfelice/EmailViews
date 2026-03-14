/*
 * PlaceholderTextView.h - BTextView with placeholder text support
 * Distributed under the terms of the MIT License.
 *
 * BTextView doesn't natively support placeholder text (greyed-out hint
 * shown when empty). This subclass draws it manually in Draw() and hooks
 * InsertText/DeleteText to trigger redraws when content changes.
 */

#ifndef PLACEHOLDER_TEXT_VIEW_H
#define PLACEHOLDER_TEXT_VIEW_H

#include <Messenger.h>
#include <String.h>
#include <TextView.h>

class PlaceholderTextView : public BTextView {
public:
	explicit			PlaceholderTextView(const char* name,
							uint32 flags = B_WILL_DRAW | B_NAVIGABLE);
	virtual				~PlaceholderTextView() override;

	virtual void		Draw(BRect updateRect) override;
	virtual void		MakeFocus(bool focus = true) override;
	virtual void		KeyDown(const char* bytes, int32 numBytes) override;
	virtual void		InsertText(const char* text, int32 length,
							int32 offset, const text_run_array* runs) override;
	virtual void		DeleteText(int32 fromOffset, int32 toOffset) override;

	void				SetPlaceholder(const char* placeholder);

	void				SetTarget(BMessenger messenger);
	void				SetModificationMessage(BMessage* message);
	void				SetInvokeMessage(BMessage* message);
	void				ResetCommittedText()
							{ fLastCommittedText.SetTo(""); }

private:
	void				_DrawPlaceholder();
	void				_SendModificationMessage();

	BString				fPlaceholder;
	BMessenger			fTarget;
	BMessage*			fModificationMessage;
	BMessage*			fInvokeMessage;
	BString				fLastCommittedText;  // Text at last invoke (Enter or Tab)
};

#endif // PLACEHOLDER_TEXT_VIEW_H
