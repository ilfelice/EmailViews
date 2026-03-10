/*
 * SearchBarView.h - Search bar with attribute dropdown and action buttons
 * Distributed under the terms of the MIT License.
 *
 * Layout: [Filter by ▾] [that ▾] [search text field][✕] [+] [ZIP]
 *
 * The search bar filters the currently loaded email list in-memory (no new
 * query). The attribute dropdown selects which field to search (Subject,
 * From, To, Account). The operator dropdown selects between "contains"
 * (substring) and "matches" (exact). The [+] button saves the current
 * search as a persistent custom query. The [ZIP] button exports the
 * currently visible emails to a ZIP backup file.
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

// Search attribute options
enum SearchAttribute {
	SEARCH_ALL = 0,
	SEARCH_SUBJECT,
	SEARCH_FROM,
	SEARCH_TO,
	SEARCH_ACCOUNT,
	SEARCH_THREAD,
	SEARCH_BODY,
	SEARCH_FULLTEXT
};

// Message for attribute menu selection
const uint32 MSG_SEARCH_ATTRIBUTE = 'srat';
const uint32 MSG_SEARCH_OPERATOR = 'srop';
const uint32 MSG_CLEAR_BUTTON_CLICKED = 'clbt';


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


// Search bar with attribute dropdown, text field, and clear/add query/backup buttons
class SearchBarView : public BView {
public:
	SearchBarView(BMessage* searchMessage, BMessage* clearMessage,
		BMessage* addQueryMessage, BMessage* backupMessage);
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
	bool IsBodySearch() const
		{ return fSearchAttribute == SEARCH_BODY
			|| fSearchAttribute == SEARCH_FULLTEXT; }

	// Exact match filter support
	void SetMatchesMode(bool matches);
	bool IsMatchesMode() const { return fMatchesMode; }
	void SetSearchAttribute(SearchAttribute attr);

	// Backup progress animation
	void SetBackupActive(bool active);
	bool IsBackupActive() const { return fBackupActive; }

	// Body search state — swaps clear button icon/tooltip
	void SetBodySearchRunning(bool running);

	// Email count status and loading indicator

private:
	void _LoadIcons();
	BRect _AddQueryButtonRect() const;
	BRect _BackupButtonRect() const;
	void _UpdateOperatorLabel();
	void _UpdateClearButtonState();

	BMenuField* fAttributeMenu;
	BMenuField* fOperatorMenu;
	SearchTextControl* fTextControl;
	BButton* fClearButton;
	BMessage* fClearMessage;
	BMessage* fAddQueryMessage;
	BMessage* fBackupMessage;
	BBitmap* fClearIcon;
	BBitmap* fStopIcon;
	BBitmap* fAddQueryIcon;
	BBitmap* fBackupIcon;
	BMessageRunner* fSearchDebounceRunner;
	BMessageRunner* fBackupDotsRunner;
	float fButtonSize;
	bool fSearchExecuted;
	bool fHasResults;
	bool fViewHasContent;  // True when email list has any content (for backup button)
	bool fLoading;         // True while email list is still loading
	bool fSettingTextProgrammatically;  // Suppress _mod reset during programmatic SetText
	SearchAttribute fSearchAttribute;
	bool fMatchesMode;
	bool fBackupActive;
	int32 fBackupDot;
};

#endif // SEARCH_BAR_VIEW_H
