/*
 * SearchBarView.h - Filter bar and body search field
 * Distributed under the terms of the MIT License.
 *
 * Layout: [Filter: ▾] [operator ▾] [text field][×] [+] | Search: [text field         ]
 *
 * The filter bar narrows the currently loaded email list using BFS attribute
 * queries. The attribute dropdown selects which field to filter (Subject,
 * From, To, Account). The operator dropdown selects between "contains"
 * (substring) and "matches" (exact). The [+] button saves the current
 * filter as a persistent custom query.
 *
 * The body search field on the right searches email body content. It always
 * includes headers in the search (full-text mode). The search runs as a
 * second pass over whatever the current BFS query loaded.
 */

#ifndef SEARCH_BAR_VIEW_H
#define SEARCH_BAR_VIEW_H

#include <MenuField.h>
#include <StringView.h>
#include <View.h>

#include "PlaceholderTextView.h"

class BBitmap;
class BButton;
class BMessageRunner;

// Search attribute options (filter field only — no body/fulltext)
enum SearchAttribute {
	SEARCH_ALL = 0,
	SEARCH_SUBJECT,
	SEARCH_FROM,
	SEARCH_TO,
	SEARCH_ACCOUNT,
	SEARCH_THREAD
};

// Message for attribute menu selection
const uint32 MSG_SEARCH_ATTRIBUTE = 'srat';
const uint32 MSG_SEARCH_OPERATOR = 'srop';
const uint32 MSG_CLEAR_BUTTON_CLICKED = 'clbt';

// Messages for body search field
const uint32 MSG_BODY_SEARCH_INVOKED = 'bsiv';
const uint32 MSG_BODY_CLEAR_CLICKED = 'bscl';


// Custom text control wrapper that draws border around PlaceholderTextView
// (mimics BTextControl's architecture)
class SearchTextControl : public BView {
public:
	SearchTextControl(const char* name, BMessage* invokeMessage);
	virtual ~SearchTextControl();

	virtual void AllAttached();
	virtual void Draw(BRect updateRect);
	virtual void MakeFocus(bool focus = true);
	virtual void FrameResized(float width, float height);
	virtual BSize MinSize();
	virtual BSize MaxSize();
	virtual BSize PreferredSize();

	void SetText(const char* text);
	const char* Text() const;
	int32 TextLength() const;
	PlaceholderTextView* TextView() const { return fTextView; }
	void SetModificationMessage(BMessage* message);
	void SetPlaceholder(const char* placeholder);
	void SetTarget(BHandler* target);

private:
	void _LayoutTextView();

	PlaceholderTextView* fTextView;

	static const int32 kFrameMargin = 2;
};


// Search bar with attribute filter on the left and body search on the right
class SearchBarView : public BView {
public:
	SearchBarView(BMessage* searchMessage, BMessage* clearMessage,
		BMessage* addQueryMessage,
		BMessage* bodySearchMessage, BMessage* bodyClearMessage);
	virtual ~SearchBarView();

	virtual void AttachedToWindow();
	virtual void Draw(BRect updateRect);
	virtual void MouseDown(BPoint where);
	virtual void FrameResized(float width, float height);
	virtual void MessageReceived(BMessage* message);
	virtual void MakeFocus(bool focus = true);
	virtual bool GetToolTipAt(BPoint point, BToolTip** _tip);

	// Override to allow window to resize narrower
	virtual BSize MinSize() override;

	// Filter field accessors
	const char* Text() const;
	void SetText(const char* text);
	BTextView* TextView() const;

	bool HasText() const;
	void SetSearchExecuted(bool executed);
	bool IsSearchExecuted() const { return fSearchExecuted; }
	void SetHasResults(bool hasResults);
	void SetViewHasContent(bool hasContent);
	void SetLoading(bool loading);
	SearchAttribute GetSearchAttribute() const { return fSearchAttribute; }

	// Exact match filter support
	void SetMatchesMode(bool matches);
	bool IsMatchesMode() const { return fMatchesMode; }
	void SetSearchAttribute(SearchAttribute attr);

	// Body search field accessors
	const char* BodySearchText() const;
	void SetBodySearchText(const char* text);
	BTextView* BodySearchTextView() const;
	bool HasBodySearchText() const;

	// Body search state — swaps clear button icon/tooltip
	void SetBodySearchRunning(bool running);

	// Body search active — results are still filtered even if text was cleared
	void SetBodySearchActive(bool active);

	// Focus the body search field
	void MakeFocusBodySearch();

private:
	void _LoadIcons();
	BRect _AddQueryButtonRect() const;
	void _UpdateOperatorLabel();
	void _UpdateClearButtonState();
	void _UpdateBodyClearButtonState();

	BMenuField* fAttributeMenu;
	BMenuField* fOperatorMenu;
	SearchTextControl* fTextControl;
	BButton* fClearButton;
	BMessage* fClearMessage;
	BMessage* fAddQueryMessage;
	BBitmap* fClearIcon;
	BBitmap* fStopIcon;
	BBitmap* fAddQueryIcon;
	BMessageRunner* fSearchDebounceRunner;
	float fButtonSize;
	bool fSearchExecuted;
	bool fHasResults;
	bool fViewHasContent;  // True when email list has any content
	bool fLoading;         // True while email list is still loading
	bool fSettingTextProgrammatically;  // Suppress _mod reset during programmatic SetText
	SearchAttribute fSearchAttribute;
	bool fMatchesMode;

	// Body search field (right side)
	SearchTextControl* fBodySearchControl;
	BButton* fBodyClearButton;
	BMessage* fBodySearchMessage;
	BMessage* fBodyClearMessage;
	BBitmap* fBodyClearIcon;
	BBitmap* fBodyStopIcon;
	bool fBodySearchActive;  // True while results are filtered by body search
};

#endif // SEARCH_BAR_VIEW_H
