/*
Open Tracker License

Terms and Conditions

Copyright (c) 1991-2001, Be Incorporated. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice applies to all licensees
and shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF TITLE, MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
BE INCORPORATED BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF, OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of Be Incorporated shall not be
used in advertising or otherwise to promote the sale, use or other dealings in
this Software without prior written authorization from Be Incorporated.

BeMail(TM), Tracker(TM), Be(R), BeOS(R), and BeIA(TM) are trademarks or
registered trademarks of Be Incorporated in the United States and other
countries. Other brand product names are registered trademarks or trademarks
of their respective holders. All rights reserved.
*/


#include "EmailReaderWindow.h"

#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <unistd.h>

#include <AppFileInfo.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <Button.h>
#include <Catalog.h>
#include <private/textencoding/CharacterSet.h>
#include <private/textencoding/CharacterSetRoster.h>
#include <Clipboard.h>
#include <ControlLook.h>
#include <DateTimeFormat.h>
#include <Debug.h>
#include <Directory.h>
#include <E-mail.h>
#include <File.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Locale.h>
#include <MimeType.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <private/storage/PathMonitor.h>
#include <PrintJob.h>
#include <Query.h>
#include <Resources.h>
#include <Roster.h>
#include <Screen.h>
#include <String.h>
#include <StringList.h>
#include <StringView.h>
#include <TextView.h>
#include <UTF8.h>
#include <VolumeRoster.h>

#include <fs_index.h>
#include <fs_info.h>

#include <MailMessage.h>
#include <MailSettings.h>
#include <MailDaemon.h>
#include <MailAttachment.h>
#include <mail_util.h>

#include "ReaderSupport.h"
#include "Content.h"
#include "Enclosures.h"
#include "FieldMsg.h"
#include "FindWindow.h"
#include "Header.h"
#include "Messages.h"
#include "MailPopUpMenu.h"
#include "Prefs.h"
#include "Signature.h"
#include "AttachmentStripView.h"
#include "String.h"
#include "Utilities.h"
#include "ReaderSettings.h"

#include "EmailViews.h"


#define B_TRANSLATION_CONTEXT "EmailReaderWindow"


using namespace BPrivate;


const char* kUndoStrings[] = {
	"Undo",
	"Undo typing",
	"Undo cut",
	"Undo paste",
	"Undo clear",
	"Undo drop"
};

const char* kRedoStrings[] = {
	"Redo",
	"Redo typing",
	"Redo cut",
	"Redo paste",
	"Redo clear",
	"Redo drop"
};


// Text for both the main menu and the pop-up menu.
static const char* kSpamMenuItemTextArray[] = {
	"Mark as spam and move to trash",		// M_TRAIN_SPAM_AND_DELETE
	"Mark as spam",							// M_TRAIN_SPAM
	"Unmark this message",					// M_UNTRAIN
	"Mark as genuine"						// M_TRAIN_GENUINE
};


static const int kCopyBufferSize = 64 * 1024;	// 64 KB


// Extract timezone offset (e.g. "+0900", "-0500") from an RFC 2822 Date header.
// Returns an empty string if no offset is found.
static BString
_ExtractTimezoneOffset(const char* rawDate)
{
	if (rawDate == NULL)
		return BString();

	// Scan backwards for a +HHMM or -HHMM pattern
	int32 len = strlen(rawDate);
	for (int32 i = len - 1; i >= 4; i--) {
		if ((rawDate[i - 4] == '+' || rawDate[i - 4] == '-')
			&& isdigit(rawDate[i - 3]) && isdigit(rawDate[i - 2])
			&& isdigit(rawDate[i - 1]) && isdigit(rawDate[i])) {
			BString tz;
			tz.SetTo(rawDate + i - 4, 5);
			return tz;
		}
	}
	return BString();
}


// static bitmap cache
BObjectList<EmailReaderWindow::BitmapItem> EmailReaderWindow::sBitmapCache;
BLocker EmailReaderWindow::sBitmapCacheLock;

// static list for tracking of Windows
BList EmailReaderWindow::sWindowList;
BLocker EmailReaderWindow::sWindowListLock;


class HorizontalLine : public BView {
public:
	HorizontalLine(BRect rect)
		:
		BView (rect, NULL, B_FOLLOW_ALL, B_WILL_DRAW)
	{
	}

	virtual void Draw(BRect rect)
	{
		FillRect(rect, B_SOLID_HIGH);
	}
};


//	#pragma mark -


EmailReaderWindow::EmailReaderWindow(BRect rect, const char* title,
	const entry_ref* ref, const char* to, const BFont* font, bool resending,
	EmailViewsWindow* emailViews)
	:
	BWindow(rect, title, B_DOCUMENT_WINDOW, 0),  // NO B_AUTO_UPDATE_SIZE_LIMITS

	fMail(NULL),
	fRef(NULL),
	fFieldState(0),
	fPanel(NULL),
	fSaveAddrMenu(NULL),
	fEncodingMenu(NULL),
	fZoom(rect),
	fEnclosuresView(NULL),
	fAttachmentStrip(NULL),
	fAttachmentSeparator(NULL),
	fHtmlVersionButton(NULL),
	fHtmlBodyContent(NULL),
	fHtmlBodyContentSize(0),
	fEmailViewsWindow(emailViews),
	fSigAdded(false),
	fReplying(false),
	fResending(resending),
	fSent(false),
	fDraft(false),
	fChanged(false),
	fOriginatingWindow(NULL),
	fSourceMail(NULL),

	fDownloading(false)
{
	fKeepStatusOnClose = false;

	BFile file(ref, B_READ_ONLY);
	if (ref) {
		fRef = new entry_ref(*ref);
		fIncoming = true;
	} else
		fIncoming = false;

	fAutoMarkRead = gReaderSettings->AutoMarkRead();
	fMenuBar = new BMenuBar("menuBar");

	// File Menu

	BMenu* menu = new BMenu(B_TRANSLATE("File"));

	BMessage* msg = new BMessage(M_NEW);
	msg->AddInt32("type", M_NEW);
	BMenuItem* item = new BMenuItem(B_TRANSLATE("New mail message"), msg, 'N');
	menu->AddItem(item);
	item->SetTarget(be_app);

	// Cheap hack - only show the drafts menu when composing messages.  Insert
	// a "true || " in the following IF statement if you want the old BeMail
	// behaviour.  The difference is that without live draft menu updating you
	// can open around 100 e-mails (the BeOS maximum number of open files)
	// rather than merely around 20, since each open draft-monitoring query
	// sucks up one file handle per mounted BFS disk volume.  Plus mail file
	// opening speed is noticably improved!  ToDo: change this to populate the
	// Draft menu with the file names on demand - when the user clicks on it;
	// don't need a live query since the menu isn't staying up for more than a
	// few seconds.

	if (!fIncoming || resending) {
		menu->AddItem(fSendLater = new BMenuItem(B_TRANSLATE("Save as draft"),
			new BMessage(M_SAVE_AS_DRAFT), 'S'));
	}

	if (!resending && fIncoming) {
		menu->AddSeparatorItem();

		// Add simple Close
		menu->AddItem(new BMenuItem(B_TRANSLATE("Close"),
			new BMessage(B_QUIT_REQUESTED), 'W'));
		menu->AddItem(new BMenuItem(
			B_TRANSLATE("Close and keep status"),
			new BMessage(M_CLOSE_KEEP_STATUS), 'W', B_SHIFT_KEY));
	} else {
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem(B_TRANSLATE("Close"),
			new BMessage(B_CLOSE_REQUESTED), 'W'));
	}

	menu->AddSeparatorItem();
	menu->AddItem(fPrint = new BMenuItem(
		B_TRANSLATE("Page setup" B_UTF8_ELLIPSIS),
		new BMessage(M_PRINT_SETUP)));
	menu->AddItem(fPrint = new BMenuItem(
		B_TRANSLATE("Print" B_UTF8_ELLIPSIS),
		new BMessage(M_PRINT), 'P'));
	fMenuBar->AddItem(menu);

	// Edit Menu

	menu = new BMenu(B_TRANSLATE("Edit"));
	menu->AddItem(fUndo = new BMenuItem(B_TRANSLATE("Undo"),
		new BMessage(B_UNDO), 'Z', 0));
	fUndo->SetTarget(NULL, this);
	menu->AddItem(fRedo = new BMenuItem(B_TRANSLATE("Redo"),
		new BMessage(M_REDO), 'Z', B_SHIFT_KEY));
	fRedo->SetTarget(NULL, this);
	menu->AddSeparatorItem();
	menu->AddItem(fCut = new BMenuItem(B_TRANSLATE("Cut"),
		new BMessage(B_CUT), 'X'));
	fCut->SetTarget(NULL, this);
	menu->AddItem(fCopy = new BMenuItem(B_TRANSLATE("Copy"),
		new BMessage(B_COPY), 'C'));
	fCopy->SetTarget(NULL, this);
	menu->AddItem(fPaste = new BMenuItem(B_TRANSLATE("Paste"),
		new BMessage(B_PASTE),
		'V'));
	fPaste->SetTarget(NULL, this);
	menu->AddSeparatorItem();
	menu->AddItem(item = new BMenuItem(B_TRANSLATE("Select all"),
		new BMessage(M_SELECT), 'A'));
	menu->AddSeparatorItem();
	item->SetTarget(NULL, this);
	menu->AddItem(new BMenuItem(B_TRANSLATE("Find" B_UTF8_ELLIPSIS),
		new BMessage(M_FIND), 'F'));
	menu->AddItem(new BMenuItem(B_TRANSLATE("Find again"),
		new BMessage(M_FIND_AGAIN), 'G'));
	if (!fIncoming) {
		menu->AddSeparatorItem();
		fQuote = new BMenuItem(B_TRANSLATE("Increase quote level"),
			new BMessage(M_ADD_QUOTE_LEVEL), '+');
		menu->AddItem(fQuote);
		fRemoveQuote = new BMenuItem(B_TRANSLATE("Decrease quote level"),
			new BMessage(M_SUB_QUOTE_LEVEL), '-');
		menu->AddItem(fRemoveQuote);

		menu->AddSeparatorItem();
		fSpelling = new BMenuItem(B_TRANSLATE("Check spelling"),
			new BMessage(M_CHECK_SPELLING), ';');
		menu->AddItem(fSpelling);
		if (gReaderSettings->StartWithSpellCheckOn())
			PostMessage(M_CHECK_SPELLING);
	}
	menu->AddSeparatorItem();
	menu->AddItem(item = new BMenuItem(
		B_TRANSLATE("Email preferences" B_UTF8_ELLIPSIS),
		new BMessage(M_PREFS), ','));
	item->SetTarget(be_app);
	fMenuBar->AddItem(menu);
	menu->AddItem(item = new BMenuItem(
		B_TRANSLATE("Email accounts" B_UTF8_ELLIPSIS),
		new BMessage(M_ACCOUNTS)));
	item->SetTarget(be_app);

	// View Menu

	if (!resending && fIncoming) {
		menu = new BMenu(B_TRANSLATE("View"));
		menu->AddItem(fHeader = new BMenuItem(B_TRANSLATE("Show header"),
			new BMessage(M_HEADER), 'H'));
		menu->AddItem(fRaw = new BMenuItem(B_TRANSLATE("Show raw message"),
			new BMessage(M_RAW)));
		fMenuBar->AddItem(menu);
	}

	// Message Menu

	menu = new BMenu(B_TRANSLATE("Message"));

	if (!resending && fIncoming) {
		menu->AddItem(new BMenuItem(B_TRANSLATE("Reply"),
			new BMessage(M_REPLY),'R'));
		menu->AddItem(new BMenuItem(B_TRANSLATE("Reply to sender"),
			new BMessage(M_REPLY_TO_SENDER),'R',B_OPTION_KEY));
		menu->AddItem(new BMenuItem(B_TRANSLATE("Reply to all"),
			new BMessage(M_REPLY_ALL), 'R', B_SHIFT_KEY));

		menu->AddSeparatorItem();

		menu->AddItem(new BMenuItem(B_TRANSLATE("Forward"),
			new BMessage(M_FORWARD), 'J'));
		menu->AddItem(new BMenuItem(B_TRANSLATE("Forward without attachments"),
			new BMessage(M_FORWARD_WITHOUT_ATTACHMENTS)));
		menu->AddItem(new BMenuItem(B_TRANSLATE("Resend"),
			new BMessage(M_RESEND)));
		menu->AddItem(new BMenuItem(B_TRANSLATE("Copy to new"),
			new BMessage(M_COPY_TO_NEW), 'D'));

		menu->AddSeparatorItem();
		fDeleteNext = new BMenuItem(B_TRANSLATE("Move to trash"),
			new BMessage(M_DELETE_NEXT), 'T');
		menu->AddItem(fDeleteNext);
		menu->AddSeparatorItem();

		BMessage* prevMsg = new BMessage(M_PREVMSG);
		prevMsg->AddBool("keepStatus", false);
		fPrevMsg = new BMenuItem(B_TRANSLATE("Previous message"), prevMsg, B_UP_ARROW);
		menu->AddItem(fPrevMsg);

		BMessage* nextMsg = new BMessage(M_NEXTMSG);
		nextMsg->AddBool("keepStatus", false);
		fNextMsg = new BMenuItem(B_TRANSLATE("Next message"), nextMsg, B_DOWN_ARROW);
		menu->AddItem(fNextMsg);
	} else {
		menu->AddItem(fSendNow = new BMenuItem(B_TRANSLATE("Send message"),
			new BMessage(M_SEND_NOW), 'M'));

		if (!fIncoming) {
			menu->AddSeparatorItem();
			fSignature = new TMenu(B_TRANSLATE("Add signature"),
				INDEX_SIGNATURE, M_SIGNATURE);
			menu->AddItem(new BMenuItem(fSignature));
			menu->AddItem(new BMenuItem(
				B_TRANSLATE("Edit signatures" B_UTF8_ELLIPSIS),
				new BMessage(M_EDIT_SIGNATURE)));
			menu->AddSeparatorItem();
			menu->AddItem(fAdd = new BMenuItem(
				B_TRANSLATE("Add attachment" B_UTF8_ELLIPSIS),
				new BMessage(M_ADD), 'E'));
			menu->AddItem(fRemove = new BMenuItem(
				B_TRANSLATE("Remove attachment"),
				new BMessage(M_REMOVE), 'T'));
		}
	}
	if (fIncoming) {
		menu->AddSeparatorItem();
		fSaveAddrMenu = new BMenu(B_TRANSLATE("Create Person file for"));
		menu->AddItem(fSaveAddrMenu);
	}

	// Encoding menu

	fEncodingMenu = new BMenu(B_TRANSLATE("Encoding"));

	BMenuItem* automaticItem = NULL;
	if (!resending && fIncoming) {
		// Reading a message, display the Automatic item
		msg = new BMessage(CHARSET_CHOICE_MADE);
		msg->AddInt32("charset", B_MAIL_NULL_CONVERSION);
		automaticItem = new BMenuItem(B_TRANSLATE("Automatic"), msg);
		fEncodingMenu->AddItem(automaticItem);
		fEncodingMenu->AddSeparatorItem();
	}

	uint32 defaultCharSet = resending || !fIncoming
		? gReaderSettings->MailCharacterSet() : B_MAIL_NULL_CONVERSION;
	bool markedCharSet = false;

	BCharacterSetRoster roster;
	BCharacterSet charSet;
	while (roster.GetNextCharacterSet(&charSet) == B_OK) {
		BString name(charSet.GetPrintName());
		const char* mime = charSet.GetMIMEName();
		if (mime != NULL)
			name << " (" << mime << ")";

		uint32 convertID;
		if (mime == NULL || strcasecmp(mime, "UTF-8") != 0)
			convertID = charSet.GetConversionID();
		else
			convertID = B_MAIL_UTF8_CONVERSION;

		msg = new BMessage(CHARSET_CHOICE_MADE);
		msg->AddInt32("charset", convertID);
		fEncodingMenu->AddItem(item = new BMenuItem(name.String(), msg));
		if (convertID == defaultCharSet && !markedCharSet) {
			item->SetMarked(true);
			markedCharSet = true;
		}
	}

	msg = new BMessage(CHARSET_CHOICE_MADE);
	msg->AddInt32("charset", B_MAIL_US_ASCII_CONVERSION);
	fEncodingMenu->AddItem(item = new BMenuItem("US-ASCII", msg));
	if (defaultCharSet == B_MAIL_US_ASCII_CONVERSION && !markedCharSet) {
		item->SetMarked(true);
		markedCharSet = true;
	}

	if (automaticItem != NULL && !markedCharSet)
		automaticItem->SetMarked(true);

	menu->AddSeparatorItem();
	menu->AddItem(fEncodingMenu);
	fMenuBar->AddItem(menu);
	fEncodingMenu->SetRadioMode(true);
	fEncodingMenu->SetTargetForItems(this);

	// Spam Menu

	if (!resending && fIncoming && gReaderSettings->ShowSpamGUI()) {
		menu = new BMenu("Spam filtering");
		menu->AddItem(new BMenuItem("Mark as spam and move to trash",
			new BMessage(M_TRAIN_SPAM_AND_DELETE), 'K'));
		menu->AddItem(new BMenuItem("Mark as spam",
			new BMessage(M_TRAIN_SPAM), 'K', B_OPTION_KEY));
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem("Unmark this message",
			new BMessage(M_UNTRAIN)));
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem("Mark as genuine",
			new BMessage(M_TRAIN_GENUINE), 'K', B_SHIFT_KEY));
		fMenuBar->AddItem(menu);
	}

	// Button Bar

	BuildToolBar();

	if (!gReaderSettings->ShowToolBar())
		fToolBar->Hide();

	fHeaderView = new THeaderView(fIncoming, resending,
		gReaderSettings->DefaultAccount());

	fContentView = new TContentView(fIncoming, const_cast<BFont*>(font),
		false, gReaderSettings->ColoredQuotes());
		// TContentView needs to be properly const, for now cast away constness

	// Create attachment strip and separator
	// For incoming emails: display-only mode
	// For compose emails: editable mode
	fAttachmentSeparator = new BSeparatorView(B_HORIZONTAL, B_PLAIN_BORDER);
	fAttachmentSeparator->Hide();  // Hidden until we have attachments
	fAttachmentStrip = new AttachmentStripView(!fIncoming);  // compose mode if not incoming
	fAttachmentStrip->Hide();  // Hidden until we have attachments

	// Create HTML version button (only for incoming emails)
	fHtmlVersionButton = new BButton("htmlVersionButton",
		B_TRANSLATE("View HTML version"), new BMessage(M_VIEW_HTML_VERSION));
	fHtmlVersionButton->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fHtmlVersionButton->Hide();  // Hidden until we detect HTML content

	BLayoutBuilder::Group<> builder(this, B_VERTICAL, 0);
	builder.Add(fMenuBar)
		.Add(fToolBar)
		.Add(new BSeparatorView(B_HORIZONTAL, B_PLAIN_BORDER))
		.AddGroup(B_VERTICAL, 0)
			.Add(fHeaderView)
			.SetInsets(B_USE_WINDOW_SPACING, B_USE_DEFAULT_SPACING)
		.End();
	
	// Add HTML version button (shown only for HTML emails)
	builder.Add(fHtmlVersionButton);
	
	// Add attachment strip for both modes
	builder.Add(fAttachmentSeparator)
		.Add(fAttachmentStrip);
	
	builder.Add(fContentView);

	if (to != NULL)
		fHeaderView->SetTo(to);

	AddShortcut('n', B_COMMAND_KEY, new BMessage(M_NEW));

	OpenMessage(ref, _CurrentCharacterSet());

	// Add to window list for FindWindow
	BAutolock locker(sWindowListLock);
	sWindowList.AddItem(this);
	
	// Also add to global settings window list
	if (gReaderSettings != NULL)
		gReaderSettings->AddWindow(this);

	// Set window size limits based on toolbar width
	float minWidth = fToolBar->PreferredSize().Width();
	if (minWidth < 300)
		minWidth = 300;  // Absolute minimum fallback
	SetSizeLimits(minWidth, 32768, 300, 32768);
}


BBitmap*
EmailReaderWindow::_RetrieveVectorIcon(int32 id)
{
	// Lock access to the list
	BAutolock lock(sBitmapCacheLock);
	if (!lock.IsLocked())
		return NULL;

	// Check for the bitmap in the cache first
	BitmapItem* item;
	for (int32 i = 0; (item = sBitmapCache.ItemAt(i)) != NULL; i++) {
		if (item->id == id)
			return item->bm;
	}

	// If it's not in the cache, try to load it
	BResources* res = BApplication::AppResources();
	if (res == NULL)
		return NULL;
	size_t size;
	const void* data = res->LoadResource(B_VECTOR_ICON_TYPE, id, &size);

	if (!data)
		return NULL;

	BBitmap* bitmap = new BBitmap(BRect(BPoint(0, 0),
		be_control_look->ComposeIconSize(31)), B_RGBA32);
	status_t status = BIconUtils::GetVectorIcon((uint8*)data, size, bitmap);
	if (status == B_OK) {
		item = (BitmapItem*)malloc(sizeof(BitmapItem));
		item->bm = bitmap;
		item->id = id;
		sBitmapCache.AddItem(item);
		return bitmap;
	}

	return NULL;
}


void
EmailReaderWindow::BuildToolBar()
{
	// Resource IDs for toolbar icons (from EmailViews.rdef)
	const int32 kIconNew = 201;          // NewEmail (shared)
	const int32 kIconSend = 1001;        // ReaderSend
	const int32 kIconSignature = 1002;   // ReaderSignature
	const int32 kIconSave = 1003;        // ReaderSave
	const int32 kIconAttachment = 1004;  // ReaderAttachment
	const int32 kIconPrint = 1005;       // ReaderPrint
	const int32 kIconTrash = 204;        // Delete (shared)
	const int32 kIconReply = 1008;       // ReaderReply
	const int32 kIconReplyAll = 1010;    // ReaderReplyAll
	const int32 kIconForward = 1009;     // ReaderForward
	const int32 kIconNext = 1013;        // ReaderNext
	const int32 kIconPrevious = 1012;    // ReaderPrevious
	const int32 kIconUnread = 203;       // MarkAsUnread (shared)
	const int32 kIconRead = 202;         // MarkAsRead (shared)

	fToolBar = new ToolBarView();
	#undef B_TRANSLATION_CONTEXT
	#define B_TRANSLATION_CONTEXT "Toolbar"
	fToolBar->AddAction(M_NEW, this, _RetrieveVectorIcon(kIconNew), NULL,
		B_TRANSLATE_COMMENT("New", "As short as possible"));
	fToolBar->AddView(new BSeparatorView(B_VERTICAL, B_PLAIN_BORDER));

	if (fResending) {
		fToolBar->AddAction(M_SEND_NOW, this, _RetrieveVectorIcon(kIconSend), NULL,
			B_TRANSLATE_COMMENT("Send", "As short as possible"));
	} else if (!fIncoming) {
		fToolBar->AddAction(M_SEND_NOW, this, _RetrieveVectorIcon(kIconSend), NULL,
			B_TRANSLATE_COMMENT("Send", "As short as possible"));
		fToolBar->SetActionEnabled(M_SEND_NOW, false);
		fToolBar->AddAction(M_SIG_MENU, this, _RetrieveVectorIcon(kIconSignature), NULL,
			B_TRANSLATE_COMMENT("Signature", "As short as possible"));
		fToolBar->AddAction(M_ADD, this, _RetrieveVectorIcon(kIconAttachment), NULL,
			B_TRANSLATE_COMMENT("Attach", "As short as possible"));
		fToolBar->AddAction(M_SAVE_AS_DRAFT, this, _RetrieveVectorIcon(kIconSave), NULL,
			B_TRANSLATE_COMMENT("Save", "As short as possible"));
		fToolBar->SetActionEnabled(M_SAVE_AS_DRAFT, false);
		fToolBar->AddAction(M_PRINT, this, _RetrieveVectorIcon(kIconPrint), NULL,
			B_TRANSLATE_COMMENT("Print", "As short as possible"));
		fToolBar->SetActionEnabled(M_PRINT, false);
		fToolBar->AddAction(M_DELETE, this, _RetrieveVectorIcon(kIconTrash), NULL,
			B_TRANSLATE_COMMENT("Trash", "As short as possible"));
	} else {
		fToolBar->AddAction(M_REPLY, this, _RetrieveVectorIcon(kIconReply), NULL,
			B_TRANSLATE_COMMENT("Reply", "As short as possible"));
		fToolBar->AddAction(M_REPLY_ALL, this, _RetrieveVectorIcon(kIconReplyAll), NULL,
			B_TRANSLATE_COMMENT("Reply all", "As short as possible"));
		fToolBar->AddAction(M_FORWARD, this, _RetrieveVectorIcon(kIconForward), NULL,
			B_TRANSLATE_COMMENT("Forward", "As short as possible"));
		fToolBar->AddAction(M_PRINT, this, _RetrieveVectorIcon(kIconPrint), NULL,
			B_TRANSLATE_COMMENT("Print", "As short as possible"));
		fToolBar->AddAction(M_DELETE_NEXT, this, _RetrieveVectorIcon(kIconTrash), NULL,
			B_TRANSLATE_COMMENT("Trash", "As short as possible"));
		if (gReaderSettings->ShowSpamGUI()) {
			fToolBar->AddAction(M_SPAM_BUTTON, this, _RetrieveVectorIcon(kIconTrash),
				NULL, B_TRANSLATE_COMMENT("Spam", "As short as possible"));
		}
		fToolBar->AddView(new BSeparatorView(B_VERTICAL, B_PLAIN_BORDER));
		fToolBar->AddAction(M_NEXTMSG, this, _RetrieveVectorIcon(kIconNext), NULL,
			B_TRANSLATE_COMMENT("Next", "As short as possible"));
		fToolBar->AddAction(M_UNREAD, this, _RetrieveVectorIcon(kIconUnread), NULL,
			B_TRANSLATE_COMMENT("Unread", "As short as possible"));
		fToolBar->SetActionVisible(M_UNREAD, false);
		fToolBar->AddAction(M_READ, this, _RetrieveVectorIcon(kIconRead), NULL,
			B_TRANSLATE_COMMENT(" Read ", "As short as possible"));
		fToolBar->SetActionVisible(M_READ, false);
		fToolBar->AddAction(M_PREVMSG, this, _RetrieveVectorIcon(kIconPrevious), NULL,
			B_TRANSLATE_COMMENT("Previous", "As short as possible"));

		if (fEmailViewsWindow == NULL) {
			fToolBar->SetActionEnabled(M_NEXTMSG, false);
			fToolBar->SetActionEnabled(M_PREVMSG, false);
		}

		if (!fAutoMarkRead)
			_AddReadButton();
	}
	fToolBar->AddGlue();
	#undef B_TRANSLATION_CONTEXT
	#define B_TRANSLATION_CONTEXT "EmailReaderWindow"
}


void
EmailReaderWindow::UpdateViews()
{
	uint8 showToolBar = gReaderSettings->ShowToolBar();

	// Show/Hide Button Bar
	if (showToolBar) {
		if (fToolBar->IsHidden())
			fToolBar->Show();

		bool showLabel = showToolBar == kShowToolBar;
		static const uint32 kCommands[] = {
			M_NEW, M_SEND_NOW, M_SIG_MENU, M_ADD, M_SAVE_AS_DRAFT,
			M_PRINT, M_DELETE, M_REPLY, M_REPLY_ALL, M_FORWARD,
			M_DELETE_NEXT, M_SPAM_BUTTON, M_NEXTMSG, M_UNREAD,
			M_READ, M_PREVMSG
		};
		for (uint32 i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
			ToolBarButton* button = fToolBar->FindButton(kCommands[i]);
			if (button != NULL) {
				button->SetLabelVisible(showLabel);
				button->SetToolTip(showLabel ? NULL : button->Label());
			}
		}
		fToolBar->UpdateLayout();
	} else if (!fToolBar->IsHidden())
		fToolBar->Hide();
}


void
EmailReaderWindow::UpdatePreferences()
{
	fAutoMarkRead = gReaderSettings->AutoMarkRead();

	UpdateViews();
	_UpdateReadButton();
}


EmailReaderWindow::~EmailReaderWindow()
{
	// Save window frame to global settings
	if (gReaderSettings != NULL)
		gReaderSettings->SetLastWindowFrame(Frame());

	delete fMail;
	delete fPanel;
	delete fOriginatingWindow;
	delete fRef;
	delete fSourceMail;
	free(fHtmlBodyContent);

	BAutolock locker(sWindowListLock);
	sWindowList.RemoveItem(this);
	
	// Also remove from global settings window list
	if (gReaderSettings != NULL)
		gReaderSettings->RemoveWindow(this);
}


status_t
EmailReaderWindow::GetMailNodeRef(node_ref& nodeRef) const
{
	if (fRef == NULL)
		return B_ERROR;

	BNode node(fRef);
	return node.GetNodeRef(&nodeRef);
}


bool
EmailReaderWindow::GetTrackerWindowFile(entry_ref* ref, bool next) const
{
	// Use direct call to EmailViewsWindow for navigation
	if (fEmailViewsWindow == NULL || fRef == NULL)
		return false;
	
	if (next)
		return fEmailViewsWindow->GetNextEmailRef(fRef, ref);
	else
		return fEmailViewsWindow->GetPrevEmailRef(fRef, ref);
}


void
EmailReaderWindow::SaveTrackerPosition(entry_ref* ref)
{
	// No longer needed - navigation is direct via EmailViewsWindow
}


void
EmailReaderWindow::SetOriginatingWindow(BWindow* window)
{
	delete fOriginatingWindow;
	fOriginatingWindow = new BMessenger(window);
}


void
EmailReaderWindow::SetTrackerSelectionToCurrent()
{
	// Update selection in EmailViewsWindow
	if (fEmailViewsWindow != NULL && fRef != NULL)
		fEmailViewsWindow->SelectEmailByRef(fRef);
}


void
EmailReaderWindow::PreserveReadingPos(bool save)
{
	BScrollBar* scroll = fContentView->TextView()->ScrollBar(B_VERTICAL);
	if (scroll == NULL || fRef == NULL)
		return;

	BNode node(fRef);
	float pos = scroll->Value();

	const char* name = "MAIL:read_pos";
	if (save) {
		node.WriteAttr(name, B_FLOAT_TYPE, 0, &pos, sizeof(pos));
		return;
	}

	if (node.ReadAttr(name, B_FLOAT_TYPE, 0, &pos, sizeof(pos)) == sizeof(pos)) {
		Lock();
		scroll->SetValue(pos);
		Unlock();
	}
}


void
EmailReaderWindow::MarkMessageRead(entry_ref* message, read_flags flag)
{
	BNode node(message);
	status_t status = node.InitCheck();
	if (status != B_OK)
		return;

	int32 account;
	if (node.ReadAttr(B_MAIL_ATTR_ACCOUNT_ID, B_INT32_TYPE, 0, &account,
		sizeof(account)) < 0)
		account = -1;

	// don't wait for the server write the attribute directly
	write_read_attr(node, flag);

	// preserve the read position in the node attribute
	PreserveReadingPos(true);

	BMailDaemon().MarkAsRead(account, *message, flag);
}


void
EmailReaderWindow::DispatchMessage(BMessage* message, BHandler* handler)
{
	if (message->what == B_KEY_DOWN) {
		const char* bytes;
		if (message->FindString("bytes", &bytes) == B_OK && bytes[0] == B_ESCAPE) {
			// Capture Shift state now, before posting quit request
			if (modifiers() & B_SHIFT_KEY)
				fKeepStatusOnClose = true;
			PostMessage(B_QUIT_REQUESTED);
			return;
		}
	}
	BWindow::DispatchMessage(message, handler);
}


void
EmailReaderWindow::FrameResized(float width, float height)
{
	BWindow::FrameResized(width, height);
	
	// Manually resize child views to match window width
	// This forces the layout to reflow when making window narrower
	for (int i = 0; i < CountChildren(); i++) {
		BView* child = ChildAt(i);
		if (child != NULL) {
			BRect frame = child->Frame();
			child->ResizeTo(width, frame.Height());
		}
	}
	
	fContentView->FrameResized(width, height);
}


void
EmailReaderWindow::MenusBeginning()
{
	int32 finish = 0;
	int32 start = 0;

	if (!fIncoming) {
		bool gotToField = !fHeaderView->IsToEmpty();
		bool gotCcField = !fHeaderView->IsCcEmpty();
		bool gotBccField = !fHeaderView->IsBccEmpty();
		bool gotSubjectField = !fHeaderView->IsSubjectEmpty();
		bool gotText = fContentView->TextView()->Text()[0] != 0;
		fSendNow->SetEnabled(gotToField || gotBccField);
		fSendLater->SetEnabled(fChanged && (gotToField || gotCcField
			|| gotBccField || gotSubjectField || gotText));

		be_clipboard->Lock();
		fPaste->SetEnabled(be_clipboard->Data()->HasData("text/plain",
				B_MIME_TYPE));
		be_clipboard->Unlock();

		fQuote->SetEnabled(false);
		fRemoveQuote->SetEnabled(false);

		fAdd->SetEnabled(true);
		fRemove->SetEnabled(false);  // Remove handled by attachment strip context menu
	} else {
		if (fResending) {
			bool enable = !fHeaderView->IsToEmpty();
			fSendNow->SetEnabled(enable);
			//fSendLater->SetEnabled(enable);

			if (fHeaderView->ToControl()->HasFocus()) {
				fHeaderView->ToControl()->GetSelection(&start, &finish);

				fCut->SetEnabled(start != finish);
				be_clipboard->Lock();
				fPaste->SetEnabled(be_clipboard->Data()->HasData(
					"text/plain", B_MIME_TYPE));
				be_clipboard->Unlock();
			} else {
				fCut->SetEnabled(false);
				fPaste->SetEnabled(false);

				if (modifiers() & B_SHIFT_KEY) {
					fPrevMsg->SetLabel(B_TRANSLATE("Previous message, keep status"));
					fPrevMsg->SetShortcut(B_UP_ARROW, B_SHIFT_KEY);
					BMessage* prevMsg = new BMessage(M_PREVMSG);
					prevMsg->AddBool("keepStatus", true);
					fPrevMsg->SetMessage(prevMsg);

					fNextMsg->SetLabel(B_TRANSLATE("Next message, keep status"));
					fNextMsg->SetShortcut(B_DOWN_ARROW, B_SHIFT_KEY);
					BMessage* nextMsg = new BMessage(M_NEXTMSG);
					nextMsg->AddBool("keepStatus", true);
					fNextMsg->SetMessage(nextMsg);
				} else {
					fPrevMsg->SetLabel(B_TRANSLATE("Previous message"));
					fPrevMsg->SetShortcut(B_UP_ARROW, 0);
					BMessage* prevMsg = new BMessage(M_PREVMSG);
					prevMsg->AddBool("keepStatus", false);
					fPrevMsg->SetMessage(prevMsg);

					fNextMsg->SetLabel(B_TRANSLATE("Next message"));
					fNextMsg->SetShortcut(B_DOWN_ARROW, 0);
					BMessage* nextMsg = new BMessage(M_NEXTMSG);
					nextMsg->AddBool("keepStatus", false);
					fNextMsg->SetMessage(nextMsg);
				}
			}
		} else {
			fCut->SetEnabled(false);
			fPaste->SetEnabled(false);
		}
	}

	fPrint->SetEnabled(fContentView->TextView()->TextLength());

	BTextView* textView = dynamic_cast<BTextView*>(CurrentFocus());
	if (textView != NULL
		&& (dynamic_cast<AddressTextControl*>(textView->Parent()) != NULL
			|| dynamic_cast<BTextControl*>(textView->Parent()) != NULL)) {
		// one of To:, Subject:, Account:, Cc:, Bcc:
		textView->GetSelection(&start, &finish);
	} else if (fContentView->TextView()->IsFocus()) {
		fContentView->TextView()->GetSelection(&start, &finish);
		if (!fIncoming) {
			fQuote->SetEnabled(true);
			fRemoveQuote->SetEnabled(true);
		}
	}

	fCopy->SetEnabled(start != finish);
	if (!fIncoming)
		fCut->SetEnabled(start != finish);

	// Undo stuff
	bool isRedo = false;
	undo_state undoState = B_UNDO_UNAVAILABLE;

	BTextView* focusTextView = dynamic_cast<BTextView*>(CurrentFocus());
	if (focusTextView != NULL)
		undoState = focusTextView->UndoState(&isRedo);

//	fUndo->SetLabel((isRedo)
//	? kRedoStrings[undoState] : kUndoStrings[undoState]);
	fUndo->SetEnabled(undoState != B_UNDO_UNAVAILABLE);
}


void
EmailReaderWindow::MessageReceived(BMessage* msg)
{
	bool wasReadMsg = false;
	switch (msg->what) {
		case B_MAIL_BODY_FETCHED:
		{
			status_t status = msg->FindInt32("status");
			if (status != B_OK) {
				fprintf(stderr, "Body could not be fetched: %s\n", strerror(status));
				PostMessage(B_QUIT_REQUESTED);
				break;
			}

			entry_ref ref;
			if (msg->FindRef("ref", &ref) != B_OK)
				break;
			if (ref != *fRef)
				break;

			// reload the current message
			OpenMessage(&ref, _CurrentCharacterSet());
			break;
		}

		case FIELD_CHANGED:
		{
			int32 prevState = fFieldState;
			int32 fieldMask = msg->FindInt32("bitmask");
			void* source;

			if (msg->FindPointer("source", &source) == B_OK) {
				int32 length;

				if (fieldMask == FIELD_BODY)
					length = ((TTextView*)source)->TextLength();
				else
					length = ((AddressTextControl*)source)->TextLength();

				if (length)
					fFieldState |= fieldMask;
				else
					fFieldState &= ~fieldMask;
			}

			// Has anything changed?
			if (prevState != fFieldState || !fChanged) {
				// Change Buttons to reflect this
				fToolBar->SetActionEnabled(M_SAVE_AS_DRAFT, fFieldState);
				fToolBar->SetActionEnabled(M_PRINT, fFieldState);
				fToolBar->SetActionEnabled(M_SEND_NOW, (fFieldState & FIELD_TO)
					|| (fFieldState & FIELD_BCC));
			}
			fChanged = true;

			// Update title bar if "subject" has changed
			if (!fIncoming && (fieldMask & FIELD_SUBJECT) != 0) {
				// If no subject, set to "Composer"
				if (fHeaderView->IsSubjectEmpty())
					SetTitle(B_TRANSLATE("Composer"));
				else {
					BString title(B_TRANSLATE("Composer"));
					title << " - " << fHeaderView->Subject();
					SetTitle(title.String());
				}
			}
			break;
		}

		case CHANGE_FONT:
			PostMessage(msg, fContentView);
			break;

		case M_NEW:
		{
			BMessage message(M_NEW);
			message.AddInt32("type", msg->what);
			be_app->PostMessage(&message);
			break;
		}

		case M_SPAM_BUTTON:
		{
			/*
				A popup from a button is good only when the behavior has some
				consistency and there is some visual indication that a menu
				will be shown when clicked. A workable implementation would
				have an extra button attached to the main one which has a
				downward-pointing arrow. Mozilla Thunderbird's 'Get Mail'
				button is a good example of this.

				TODO: Replace this code with a split toolbar button
			*/
			uint32 buttons;
			if (msg->FindInt32("buttons", (int32*)&buttons) == B_OK
				&& buttons == B_SECONDARY_MOUSE_BUTTON) {
				BPopUpMenu menu("Spam Actions", false, false);
				for (int i = 0; i < 4; i++)
					menu.AddItem(new BMenuItem(kSpamMenuItemTextArray[i],
						new BMessage(M_TRAIN_SPAM_AND_DELETE + i)));

				BPoint where;
				msg->FindPoint("where", &where);
				BMenuItem* item;
				if ((item = menu.Go(where, false, false)) != NULL)
					PostMessage(item->Message());
				break;
			} else {
				// Default action for left clicking on the spam button.
				PostMessage(new BMessage(M_TRAIN_SPAM_AND_DELETE));
			}
			break;
		}

		case M_TRAIN_SPAM_AND_DELETE:
			PostMessage(M_DELETE_NEXT);
		case M_TRAIN_SPAM:
			TrainMessageAs("Spam");
			break;

		case M_UNTRAIN:
			TrainMessageAs("Uncertain");
			break;

		case M_TRAIN_GENUINE:
			TrainMessageAs("Genuine");
			break;

		case M_REPLY:
		{
			// TODO: This needs removed in favor of a split toolbar button.
			// See comments for Spam button
			uint32 buttons;
			if (msg->FindInt32("buttons", (int32*)&buttons) == B_OK
				&& buttons == B_SECONDARY_MOUSE_BUTTON) {
				BPopUpMenu menu("Reply To", false, false);
				menu.AddItem(new BMenuItem(B_TRANSLATE("Reply"),
					new BMessage(M_REPLY)));
				menu.AddItem(new BMenuItem(B_TRANSLATE("Reply to sender"),
					new BMessage(M_REPLY_TO_SENDER)));
				menu.AddItem(new BMenuItem(B_TRANSLATE("Reply to all"),
					new BMessage(M_REPLY_ALL)));

				BPoint where;
				msg->FindPoint("where", &where);

				BMenuItem* item;
				if ((item = menu.Go(where, false, false)) != NULL) {
					item->SetTarget(this);
					PostMessage(item->Message());
				}
				break;
			}
			// Fall through
		}
		case M_FORWARD:
		{
			// TODO: This needs removed in favor of a split toolbar button.
			// See comments for Spam button
			uint32 buttons;
			if (msg->FindInt32("buttons", (int32*)&buttons) == B_OK
				&& buttons == B_SECONDARY_MOUSE_BUTTON) {
				BPopUpMenu menu("Forward", false, false);
				menu.AddItem(new BMenuItem(B_TRANSLATE("Forward"),
					new BMessage(M_FORWARD)));
				menu.AddItem(new BMenuItem(
					B_TRANSLATE("Forward without attachments"),
					new BMessage(M_FORWARD_WITHOUT_ATTACHMENTS)));

				BPoint where;
				msg->FindPoint("where", &where);

				BMenuItem* item;
				if ((item = menu.Go(where, false, false)) != NULL) {
					item->SetTarget(this);
					PostMessage(item->Message());
				}
				break;
			}
		}

		// Fall Through
		case M_REPLY_ALL:
		case M_REPLY_TO_SENDER:
		case M_FORWARD_WITHOUT_ATTACHMENTS:
		case M_RESEND:
		case M_COPY_TO_NEW:
		{
			BMessage message(M_NEW);
			message.AddRef("ref", fRef);
			message.AddPointer("window", this);
			message.AddInt32("type", msg->what);
			be_app->PostMessage(&message);
			break;
		}
		case M_CLOSE_KEEP_STATUS:
			fKeepStatusOnClose = true;
			PostMessage(B_QUIT_REQUESTED);
			break;

		case M_DELETE:
		case M_DELETE_PREV:
		case M_DELETE_NEXT:
		{
			if (msg->what == M_DELETE_NEXT && (modifiers() & B_SHIFT_KEY) != 0)
				msg->what = M_DELETE_PREV;

			bool foundRef = false;
			entry_ref nextRef;
			if ((msg->what == M_DELETE_PREV || msg->what == M_DELETE_NEXT)
				&& fRef != NULL) {
				// Find the next message that should be displayed
				nextRef = *fRef;
				foundRef = GetTrackerWindowFile(&nextRef,
					msg->what == M_DELETE_NEXT);
			}
			if (fIncoming) {
				read_flags flag = (fAutoMarkRead == true) ? B_READ : B_SEEN;
				MarkMessageRead(fRef, flag);
			}

			// Move to trash via Tracker
			if (fDraft || fIncoming) {
				BMessenger tracker("application/x-vnd.Be-TRAK");
				if (tracker.IsValid()) {
					BMessage msg('Ttrs');
					msg.AddRef("refs", fRef);
					tracker.SendMessage(&msg);

					// Notify main window to refresh query counts
					if (fEmailViewsWindow != NULL)
						fEmailViewsWindow->PostMessage(MSG_BACKGROUND_QUERY_UPDATE);
				} else {
					BAlert* alert = new BAlert("",
						B_TRANSLATE("Need Tracker to move items to trash"),
						B_TRANSLATE("Sorry"));
					alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
					alert->Go();
				}
			}

			// 	If the next file was found, open it.  If it was not,
			//	we have no choice but to close this window.
			if (foundRef) {
				EmailReaderWindow* window = FindWindow(nextRef);
				if (window == NULL)
					OpenMessage(&nextRef, _CurrentCharacterSet());
				else
					window->Activate();

				SetTrackerSelectionToCurrent();

				if (window == NULL)
					break;
			}

			fSent = true;
			BMessage msg(B_CLOSE_REQUESTED);
			PostMessage(&msg);
			break;
		}

		case M_HEADER:
		{
			bool showHeader = !fHeader->IsMarked();
			fHeader->SetMarked(showHeader);

			BMessage message(M_HEADER);
			message.AddBool("header", showHeader);
			PostMessage(&message, fContentView->TextView());
			break;
		}
		case M_RAW:
		{
			bool raw = !(fRaw->IsMarked());
			fRaw->SetMarked(raw);
			BMessage message(M_RAW);
			message.AddBool("raw", raw);
			PostMessage(&message, fContentView->TextView());
			break;
		}
		case M_SEND_NOW:
		case M_SAVE_AS_DRAFT:
			// Check for empty subject when sending (not for drafts)
			if (msg->what == M_SEND_NOW && fHeaderView->IsSubjectEmpty()) {
				BAlert* alert = new BAlert(
					B_TRANSLATE("Empty subject"),
					B_TRANSLATE("The subject is empty. Do you want to send anyway?"),
					B_TRANSLATE("Cancel"),
					B_TRANSLATE("Send"),
					NULL,
					B_WIDTH_AS_USUAL,
					B_WARNING_ALERT);
				alert->SetShortcut(0, B_ESCAPE);
				if (alert->Go() == 0)
					break;  // User cancelled
			}
			Send(msg->what == M_SEND_NOW);
			break;

		case M_SAVE:
		{
			const char* address;
			const char* name;
			if (msg->FindString("address", (const char**)&address) != B_OK)
				break;
			if (msg->FindString("name", (const char**)&name) != B_OK)
				break;

			BVolumeRoster volumeRoster;
			BVolume volume;
			BQuery query;
			BEntry entry;
			bool foundEntry = false;

			char* arg = (char*)malloc(strlen("META:email=")
				+ strlen(address) + 1);
			sprintf(arg, "META:email=%s", address);

			// Search a Person file with this email address
			while (volumeRoster.GetNextVolume(&volume) == B_NO_ERROR) {
				if (!volume.KnowsQuery())
					continue;

				query.SetVolume(&volume);
				query.SetPredicate(arg);
				query.Fetch();

				if (query.GetNextEntry(&entry) == B_NO_ERROR) {
					BMessenger tracker("application/x-vnd.Be-TRAK");
					if (tracker.IsValid()) {
						entry_ref ref;
						entry.GetRef(&ref);

						BMessage open(B_REFS_RECEIVED);
						open.AddRef("refs", &ref);
						tracker.SendMessage(&open);
						foundEntry = true;
						break;
					}
				}
				// Try next volume, if any
				query.Clear();
			}

			if (!foundEntry) {
				// None found.
				// Ask to open a new Person file with this address + name pre-filled
				_CreateNewPerson(address, name);
			}
			free(arg);
			break;
		}

		case M_READ_POS:
			PreserveReadingPos(false);
			break;

		case M_PRINT_SETUP:
			PrintSetup();
			break;

		case M_PRINT:
			Print();
			break;

		case M_SELECT:
			break;

		case M_FIND:
			FindWindow::Find(this);
			break;

		case M_FIND_AGAIN:
			FindWindow::FindAgain(this);
			break;

		case M_ADD_QUOTE_LEVEL:
		case M_SUB_QUOTE_LEVEL:
			PostMessage(msg->what, fContentView);
			break;

		case M_SIGNATURE:
		{
			BMessage message(*msg);
			PostMessage(&message, fContentView);
			fSigAdded = true;
			break;
		}
		case M_RESET_CHANGED:
			// Reset fChanged after auto-signature insertion completes
			// This is called via BMessageRunner with a delay to ensure
			// all FIELD_CHANGED messages have been processed
			fChanged = false;
			break;
		case M_EDIT_SIGNATURE:
		{
			if (gReaderSettings != NULL)
				gReaderSettings->ShowSignatureWindow();
			break;
		}
		case M_SIG_MENU:
		{
			TMenu* menu;
			BMenuItem* item;
			menu = new TMenu("Add Signature", INDEX_SIGNATURE, M_SIGNATURE,
				true);

			BPoint where;
			if (msg->FindPoint("where", &where) != B_OK) {
				BRect rect;
				ToolBarButton* button = fToolBar->FindButton(M_SIG_MENU);
				if (button != NULL)
					rect = button->Frame();
				else
					rect = fToolBar->Bounds();

				where = button->ConvertToScreen(BPoint(
					((rect.right - rect.left) / 2) - 16,
					(rect.bottom - rect.top) / 2));
			}

			if ((item = menu->Go(where, false, true)) != NULL) {
				item->SetTarget(this);
				(dynamic_cast<BInvoker*>(item))->Invoke();
			}
			delete menu;
			break;
		}

		case M_ADD:
			if (!fPanel) {
				BMessenger me(this);
				BMessage msg(REFS_RECEIVED);
				fPanel = new BFilePanel(B_OPEN_PANEL, &me, &fOpenFolder, false,
					true, &msg);
			} else if (!fPanel->Window()->IsHidden()) {
				fPanel->Window()->Activate();
			}

			if (fPanel->Window()->IsHidden())
				fPanel->Window()->Show();
			break;

		case M_REMOVE:
			// Attachment removal now handled by AttachmentStripView context menu
			break;

		case CHARSET_CHOICE_MADE:
		{
			int32 charSet;
			if (msg->FindInt32("charset", &charSet) != B_OK)
				break;

			BMessage update(FIELD_CHANGED);
			update.AddInt32("bitmask", 0);
				// just enable the save button
			PostMessage(&update);

			if (fIncoming && !fResending) {
				// The user wants to see the message they are reading (not
				// composing) displayed with a different kind of character set
				// for decoding.  Reload the whole message and redisplay.  For
				// messages which are being composed, the character set is
				// retrieved from the header view when it is needed.

				entry_ref fileRef = *fRef;
				OpenMessage(&fileRef, charSet);
			}
			break;
		}

		case B_SIMPLE_DATA:
		case REFS_RECEIVED:
			AddEnclosure(msg);
			break;

		//
		//	Navigation Messages
		//
		case M_UNREAD:
			MarkMessageRead(fRef, B_SEEN);
			_UpdateReadButton();
			PostMessage(M_NEXTMSG);
			break;
		case M_READ:
			wasReadMsg = true;
			_UpdateReadButton();
			msg->what = M_NEXTMSG;
		case M_PREVMSG:
		case M_NEXTMSG:
		{
			if (fRef == NULL)
				break;

			bool keepStatus;
			if (msg->FindBool("keepStatus", &keepStatus) != B_OK)
				keepStatus = false;
			// When SHIFT-clicking toolbar icon, don't change mail's status
			if (modifiers() & B_SHIFT_KEY)
				keepStatus = true;

			entry_ref orgRef = *fRef;
			entry_ref nextRef = *fRef;
			if (GetTrackerWindowFile(&nextRef, (msg->what == M_NEXTMSG))) {
				EmailReaderWindow* window = FindWindow(nextRef);
				if (window == NULL) {
					BNode node(fRef);
					read_flags currentFlag;
					if (!keepStatus) {
						if (read_read_attr(node, currentFlag) != B_OK)
							currentFlag = B_UNREAD;
						if (fAutoMarkRead == true)
							MarkMessageRead(fRef, B_READ);
						else if (currentFlag != B_READ && !wasReadMsg)
							MarkMessageRead(fRef, B_SEEN);
					}
					OpenMessage(&nextRef, _CurrentCharacterSet());
				} else {
					window->Activate();
					//fSent = true;
					PostMessage(B_CLOSE_REQUESTED);
				}

				SetTrackerSelectionToCurrent();
			} else {
				if (wasReadMsg)
					PostMessage(B_CLOSE_REQUESTED);

				beep();
			}
			if (wasReadMsg)
				MarkMessageRead(&orgRef, B_READ);
			break;
		}

		case M_SAVE_POSITION:
			if (fRef != NULL)
				SaveTrackerPosition(fRef);
			break;

		case RESET_BUTTONS:
			fChanged = false;
			fFieldState = 0;
			if (!fHeaderView->IsToEmpty())
				fFieldState |= FIELD_TO;
			if (!fHeaderView->IsSubjectEmpty())
				fFieldState |= FIELD_SUBJECT;
			if (!fHeaderView->IsCcEmpty())
				fFieldState |= FIELD_CC;
			if (!fHeaderView->IsBccEmpty())
				fFieldState |= FIELD_BCC;
			if (fContentView->TextView()->TextLength() != 0)
				fFieldState |= FIELD_BODY;

			fToolBar->SetActionEnabled(M_SAVE_AS_DRAFT, false);
			fToolBar->SetActionEnabled(M_PRINT, fFieldState);
			fToolBar->SetActionEnabled(M_SEND_NOW, (fFieldState & FIELD_TO)
				|| (fFieldState & FIELD_BCC));
			break;

		case M_CHECK_SPELLING:
			if (!gDictCount) {
				beep();
				BAlert* alert = new BAlert("",
					B_TRANSLATE("No spell check dictionary was found.\n\n"
						"Please install a \"words\" file in:\n"
						"/boot/system/data/spell_check/word_dictionary/"),
					B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL,
					B_OFFSET_SPACING, B_STOP_ALERT);
				alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
				alert->Go();
			} else {
				fSpelling->SetMarked(!fSpelling->IsMarked());
				fContentView->TextView()->EnableSpellCheck(
					fSpelling->IsMarked());
			}
			break;

		case 'atch':
			// Attachment strip changed (add/remove)
			fChanged = true;
			break;

		case B_COLORS_UPDATED:
		{
			// System colors changed - update text view colors
			if (fContentView != NULL && fContentView->TextView() != NULL) {
				TTextView* textView = fContentView->TextView();
				rgb_color textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
				int32 textLength = textView->TextLength();
				if (textLength > 0) {
					textView->SetFontAndColor(0, textLength, NULL, B_FONT_ALL, &textColor);
				}
				textView->Invalidate();
			}
			break;
		}

		case M_VIEW_HTML_VERSION:
		{
			// Open stored HTML content in browser
			if (fHtmlBodyContent != NULL && fHtmlBodyContentSize > 0) {
				BPath tempPath;
				if (find_directory(B_SYSTEM_TEMP_DIRECTORY, &tempPath) == B_OK) {
					tempPath.Append("EmailViews_attachments");
					create_directory(tempPath.Path(), 0755);
					tempPath.Append("email_body.html");
					
					BFile file(tempPath.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
					if (file.InitCheck() == B_OK) {
						file.Write(fHtmlBodyContent, fHtmlBodyContentSize);
						file.Sync();
						
						// Set MIME type
						BNodeInfo nodeInfo(&file);
						nodeInfo.SetType("text/html");
						
						// Open in default browser
						entry_ref ref;
						BEntry entry(tempPath.Path());
						if (entry.GetRef(&ref) == B_OK) {
							be_roster->Launch(&ref);
						}
					}
				}
			}
			break;
		}

		default:
			BWindow::MessageReceived(msg);
	}
}


void
EmailReaderWindow::AddEnclosure(BMessage* msg)
{
	if (fIncoming || fAttachmentStrip == NULL)
		return;

	if (msg && msg->HasRef("refs")) {
		entry_ref ref;
		int32 index = 0;
		while (msg->FindRef("refs", index++, &ref) == B_OK) {
			fAttachmentStrip->AddAttachment(&ref);
		}
		
		// Show the attachment strip if we added attachments
		if (fAttachmentStrip->HasAttachments()) {
			if (fAttachmentSeparator->IsHidden())
				fAttachmentSeparator->Show();
			if (fAttachmentStrip->IsHidden())
				fAttachmentStrip->Show();
		}

		fChanged = true;
		
		// Remember folder for next time
		msg->FindRef("refs", &ref);
		BEntry entry(&ref);
		entry.GetParent(&entry);
		entry.GetRef(&fOpenFolder);
	}
}


bool
EmailReaderWindow::QuitRequested()
{
	int32 result;

	// Hold SHIFT when closing to keep the email's status unchanged
	if (modifiers() & B_SHIFT_KEY)
		fKeepStatusOnClose = true;

	if ((!fIncoming || (fIncoming && fResending)) && fChanged && !fSent
		&& (!fHeaderView->IsToEmpty()
			|| !fHeaderView->IsSubjectEmpty()
			|| !fHeaderView->IsCcEmpty()
			|| !fHeaderView->IsBccEmpty()
			|| (fContentView->TextView() != NULL
				&& strlen(fContentView->TextView()->Text()))
			|| (fAttachmentStrip != NULL
				&& fAttachmentStrip->HasAttachments()))) {
		if (fResending) {
			BAlert* alert = new BAlert("", B_TRANSLATE(
					"Send this message before closing?"),
				B_TRANSLATE("Cancel"),
				B_TRANSLATE("Don't send"),
				B_TRANSLATE("Send"),
				B_WIDTH_AS_USUAL, B_OFFSET_SPACING, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			alert->SetShortcut(1, 'd');
			alert->SetShortcut(2, 's');
			result = alert->Go();

			switch (result) {
				case 0:	// Cancel
					return false;
				case 1:	// Don't send
					break;
				case 2:	// Send
					Send(true);
					break;
			}
		} else {
			BAlert* alert = new BAlert("",
				B_TRANSLATE("Save this message as a draft before closing?"),
				B_TRANSLATE("Cancel"),
				B_TRANSLATE("Don't save"),
				B_TRANSLATE("Save"),
				B_WIDTH_AS_USUAL, B_OFFSET_SPACING, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			alert->SetShortcut(1, 'd');
			alert->SetShortcut(2, 's');
			result = alert->Go();
			switch (result) {
				case 0:	// Cancel
					return false;
				case 1:	// Don't Save
					break;
				case 2:	// Save
					Send(false);
					break;
			}
		}
	}

	BMessage message(WINDOW_CLOSED);
	message.AddInt32("kind", MAIL_WINDOW);
	message.AddPointer("window", this);
	be_app->PostMessage(&message);

	if (CurrentMessage() && CurrentMessage()->HasString("status")) {
		// User explicitly requests a status to set this message to.
		if (!CurrentMessage()->HasString("same")) {
			const char* status = CurrentMessage()->FindString("status");
			if (status != NULL) {
				BNode node(fRef);
				if (node.InitCheck() == B_NO_ERROR) {
					node.RemoveAttr(B_MAIL_ATTR_STATUS);
					WriteAttrString(&node, B_MAIL_ATTR_STATUS, status);
				}
			}
		}
	} else if (fRef != NULL && !fKeepStatusOnClose) {
		// ...Otherwise just set the message read
		if (fAutoMarkRead == true)
			MarkMessageRead(fRef, B_READ);
		else {
			BNode node(fRef);
			read_flags currentFlag;
			if (read_read_attr(node, currentFlag) != B_OK)
				currentFlag = B_UNREAD;
			if (currentFlag == B_UNREAD)
				MarkMessageRead(fRef, B_SEEN);
		}
	}

	BPrivate::BPathMonitor::StopWatching(BMessenger(this, this));

	return true;
}


void
EmailReaderWindow::Show()
{
	if (Lock()) {
		if (!fResending && (fIncoming || fReplying)) {
			fContentView->TextView()->MakeFocus(true);
		} else {
			fHeaderView->ToControl()->MakeFocus(true);
			fHeaderView->ToControl()->SelectAll();
		}
		Unlock();
	}
	BWindow::Show();
}


void
EmailReaderWindow::Zoom(BPoint /*pos*/, float /*x*/, float /*y*/)
{
	float		height;
	float		width;

	BRect rect = Frame();
	width = 80 * gReaderSettings->ContentFont().StringWidth("M")
		+ (rect.Width() - fContentView->TextView()->Bounds().Width() + 6);

	BScreen screen(this);
	BRect screenFrame = screen.Frame();
	if (width > (screenFrame.Width() - 8))
		width = screenFrame.Width() - 8;

	height = max_c(fContentView->TextView()->CountLines(), 20)
		* fContentView->TextView()->LineHeight(0)
		+ (rect.Height() - fContentView->TextView()->Bounds().Height());
	if (height > (screenFrame.Height() - 29))
		height = screenFrame.Height() - 29;

	rect.right = rect.left + width;
	rect.bottom = rect.top + height;

	if (abs((int)(Frame().Width() - rect.Width())) < 5
		&& abs((int)(Frame().Height() - rect.Height())) < 5) {
		rect = fZoom;
	} else {
		fZoom = Frame();
		screenFrame.InsetBy(6, 6);

		if (rect.Width() > screenFrame.Width())
			rect.right = rect.left + screenFrame.Width();
		if (rect.Height() > screenFrame.Height())
			rect.bottom = rect.top + screenFrame.Height();

		if (rect.right > screenFrame.right) {
			rect.left -= rect.right - screenFrame.right;
			rect.right = screenFrame.right;
		}
		if (rect.bottom > screenFrame.bottom) {
			rect.top -= rect.bottom - screenFrame.bottom;
			rect.bottom = screenFrame.bottom;
		}
		if (rect.left < screenFrame.left) {
			rect.right += screenFrame.left - rect.left;
			rect.left = screenFrame.left;
		}
		if (rect.top < screenFrame.top) {
			rect.bottom += screenFrame.top - rect.top;
			rect.top = screenFrame.top;
		}
	}

	ResizeTo(rect.Width(), rect.Height());
	MoveTo(rect.LeftTop());
}


void
EmailReaderWindow::WindowActivated(bool status)
{
	if (status) {
		BAutolock locker(sWindowListLock);
		sWindowList.RemoveItem(this);
		sWindowList.AddItem(this, 0);
	}
}


void
EmailReaderWindow::Forward(entry_ref* ref, EmailReaderWindow* window,
	bool includeAttachments)
{
	BEmailMessage* mail = window->Mail();
	if (mail == NULL)
		return;

	uint32 useAccountFrom = gReaderSettings->UseAccountFrom();

	fMail = mail->ForwardMessage(useAccountFrom == ACCOUNT_FROM_MAIL,
		includeAttachments);

	BFile file(ref, O_RDONLY);
	if (file.InitCheck() < B_NO_ERROR)
		return;

	fHeaderView->SetSubject(fMail->Subject());

	// set mail account

	if (useAccountFrom == ACCOUNT_FROM_MAIL)
		fHeaderView->SetAccount(fMail->Account());

	if (fMail->CountComponents() > 1) {
		// Add attachments from the forwarded email to the attachment strip
		if (fAttachmentStrip != NULL) {
			fAttachmentStrip->AddEnclosuresFromMail(fMail);
			if (fAttachmentStrip->HasAttachments()) {
				if (fAttachmentSeparator->IsHidden())
					fAttachmentSeparator->Show();
				if (fAttachmentStrip->IsHidden())
					fAttachmentStrip->Show();
			}
		}
	}

	fContentView->TextView()->LoadMessage(fMail, false, NULL);
	fChanged = false;
	fFieldState = 0;

	// Add auto-signature before the forwarded text
	AddAutoSignature(true);
}


void
EmailReaderWindow::Print()
{
	BPrintJob print(Title());

	if (!gReaderSettings->HasPrintSettings()) {
		if (print.Settings()) {
			gReaderSettings->SetPrintSettings(print.Settings());
		} else {
			PrintSetup();
			if (!gReaderSettings->HasPrintSettings())
				return;
		}
	}

	print.SetSettings(new BMessage(gReaderSettings->PrintSettings()));

	if (print.ConfigJob() == B_OK) {
		int32 curPage = 1;
		int32 lastLine = 0;
		BTextView header_view(print.PrintableRect(), "header",
			print.PrintableRect().OffsetByCopy(BPoint(
				-print.PrintableRect().left, -print.PrintableRect().top)),
			B_FOLLOW_ALL_SIDES);

		//---------Init the header fields
		#define add_header_field(label, field) { \
			/*header_view.SetFontAndColor(be_bold_font);*/ \
			header_view.Insert(label); \
			header_view.Insert(" "); \
			/*header_view.SetFontAndColor(be_plain_font);*/ \
			header_view.Insert(field); \
			header_view.Insert("\n"); \
		}

		add_header_field("Subject:", fHeaderView->Subject());
		add_header_field("To:", fHeaderView->To());
		if (!fHeaderView->IsCcEmpty())
			add_header_field(B_TRANSLATE("Cc:"), fHeaderView->Cc());

		if (!fHeaderView->IsDateEmpty())
			header_view.Insert(fHeaderView->Date());

		int32 maxLine = fContentView->TextView()->CountLines();
		BRect pageRect = print.PrintableRect();
		BRect curPageRect = pageRect;

		print.BeginJob();
		float header_height = header_view.TextHeight(0,
			header_view.CountLines());

		BRect rect(0, 0, pageRect.Width(), header_height);
		BBitmap bmap(rect, B_BITMAP_ACCEPTS_VIEWS, B_RGBA32);
		bmap.Lock();
		bmap.AddChild(&header_view);
		print.DrawView(&header_view, rect, BPoint(0.0, 0.0));
		HorizontalLine line(BRect(0, 0, pageRect.right, 0));
		bmap.AddChild(&line);
		print.DrawView(&line, line.Bounds(), BPoint(0, header_height + 1));
		bmap.Unlock();
		header_height += 5;

		do {
			int32 lineOffset = fContentView->TextView()->OffsetAt(lastLine);
			curPageRect.OffsetTo(0,
				fContentView->TextView()->PointAt(lineOffset).y);

			int32 fromLine = lastLine;
			lastLine = fContentView->TextView()->LineAt(
				BPoint(0.0, curPageRect.bottom - ((curPage == 1)
					? header_height : 0)));

			float curPageHeight = fContentView->TextView()->TextHeight(
				fromLine, lastLine) + (curPage == 1 ? header_height : 0);

			if (curPageHeight > pageRect.Height()) {
				curPageHeight = fContentView->TextView()->TextHeight(
					fromLine, --lastLine) + (curPage == 1 ? header_height : 0);
			}
			curPageRect.bottom = curPageRect.top + curPageHeight - 1.0;

			if (curPage >= print.FirstPage() && curPage <= print.LastPage()) {
				print.DrawView(fContentView->TextView(), curPageRect,
					BPoint(0.0, curPage == 1 ? header_height : 0.0));
				print.SpoolPage();
			}

			curPageRect = pageRect;
			lastLine++;
			curPage++;

		} while (print.CanContinue() && lastLine < maxLine);

		print.CommitJob();
		bmap.RemoveChild(&header_view);
		bmap.RemoveChild(&line);
	}
}


void
EmailReaderWindow::PrintSetup()
{
	BPrintJob printJob("mail_print");

	if (gReaderSettings->HasPrintSettings()) {
		BMessage printSettings = gReaderSettings->PrintSettings();
		printJob.SetSettings(new BMessage(printSettings));
	}

	if (printJob.ConfigPage() == B_OK)
		gReaderSettings->SetPrintSettings(printJob.Settings());
}


void
EmailReaderWindow::SetTo(const char* mailTo, const char* subject, const char* ccTo,
	const char* bccTo, const BString* body, BMessage* enclosures)
{
	Lock();

	if (mailTo != NULL && mailTo[0])
		fHeaderView->SetTo(mailTo);
	if (subject != NULL && subject[0])
		fHeaderView->SetSubject(subject);
	if (ccTo != NULL && ccTo[0])
		fHeaderView->SetCc(ccTo);
	if (bccTo != NULL && bccTo[0])
		fHeaderView->SetBcc(bccTo);

	if (body != NULL && body->Length()) {
		fContentView->TextView()->SetText(body->String(), body->Length());
		fContentView->TextView()->GoToLine(0);
	}

	if (enclosures && enclosures->HasRef("refs"))
		AddEnclosure(enclosures);

	Unlock();
}


void
EmailReaderWindow::CopyMessage(entry_ref* ref, EmailReaderWindow* src)
{
	BNode file(ref);
	if (file.InitCheck() == B_OK) {
		BString string;
		if (file.ReadAttrString(B_MAIL_ATTR_TO, &string) == B_OK)
			fHeaderView->SetTo(string);

		if (file.ReadAttrString(B_MAIL_ATTR_SUBJECT, &string) == B_OK)
			fHeaderView->SetSubject(string);

		if (file.ReadAttrString(B_MAIL_ATTR_CC, &string) == B_OK)
			fHeaderView->SetCc(string);
	}

	TTextView* text = src->fContentView->TextView();
	text_run_array* style = text->RunArray(0, text->TextLength());

	fContentView->TextView()->SetText(text->Text(), text->TextLength(), style);

	free(style);
}


void
EmailReaderWindow::Reply(entry_ref* ref, EmailReaderWindow* window, uint32 type)
{
	fRepliedMail = *ref;
	SetOriginatingWindow(window);

	BEmailMessage* mail = window->Mail();
	if (mail == NULL)
		return;

	if (type == M_REPLY_ALL)
		type = B_MAIL_REPLY_TO_ALL;
	else if (type == M_REPLY_TO_SENDER)
		type = B_MAIL_REPLY_TO_SENDER;
	else
		type = B_MAIL_REPLY_TO;

	uint32 useAccountFrom = gReaderSettings->UseAccountFrom();

	fMail = mail->ReplyMessage(mail_reply_to_mode(type),
		useAccountFrom == ACCOUNT_FROM_MAIL, QUOTE);

	// set header fields
	fHeaderView->SetTo(fMail->To());
	fHeaderView->SetCc(fMail->CC());
	fHeaderView->SetSubject(fMail->Subject());

	int32 accountID;
	BFile file(window->fRef, B_READ_ONLY);
	if (file.ReadAttr("MAIL:reply_with", B_INT32_TYPE, 0, &accountID,
		sizeof(int32)) != B_OK)
		accountID = -1;

	// set mail account

	if ((useAccountFrom == ACCOUNT_FROM_MAIL) || (accountID > -1)) {
		if (useAccountFrom == ACCOUNT_FROM_MAIL)
			fHeaderView->SetAccount(fMail->Account());
		else
			fHeaderView->SetAccount(accountID);
	}

	// create preamble string

	BString preamble = gReaderSettings->ReplyPreamble();

	BString name;
	mail->GetName(&name);
	if (name.Length() <= 0)
		name = B_TRANSLATE("(Name unavailable)");

	BString address(mail->From());
	if (address.Length() > 0) {
		// Extract just the email address from "Name <email>" format
		int32 open = address.FindFirst('<');
		int32 close = address.FindFirst('>', open);
		if (open >= 0 && close > open) {
			BString email;
			address.CopyInto(email, open + 1, close - open - 1);
			address = email;
		}
	}
	if (address.Length() <= 0)
		address = B_TRANSLATE("(Address unavailable)");

	BString date;
	{
		BNode node(window->fRef);
		time_t when = 0;
		// Try 8-byte time_t first, then fall back to 4-byte
		ssize_t size = node.ReadAttr("MAIL:when", B_TIME_TYPE,
			0, &when, sizeof(time_t));
		if (size < (ssize_t)sizeof(time_t)) {
			int32 when32 = 0;
			size = node.ReadAttr("MAIL:when", B_TIME_TYPE,
				0, &when32, sizeof(int32));
			if (size == sizeof(int32))
				when = (time_t)when32;
		}
		if (when > 0) {
			BDateTimeFormat formatter;
			formatter.Format(date, when,
				B_SHORT_DATE_FORMAT, B_SHORT_TIME_FORMAT);

			// Append sender's timezone offset if available
			BString tz = _ExtractTimezoneOffset(
				mail->HeaderField("Date"));
			if (tz.Length() > 0)
				date << " (" << tz << ")";
		}
	}
	if (date.Length() <= 0)
		date = B_TRANSLATE("(Date unavailable)");

	preamble.ReplaceAll("%n", name);
	preamble.ReplaceAll("%e", address);
	preamble.ReplaceAll("%d", date);
	preamble.ReplaceAll("\\n", "\n");

	// insert (if selection) or load (if whole mail) message text into text view

	int32 finish, start;
	window->fContentView->TextView()->GetSelection(&start, &finish);
	if (start != finish) {
		char* text = (char*)malloc(finish - start + 1);
		if (text == NULL)
			return;

		window->fContentView->TextView()->GetText(start, finish - start, text);
		if (text[strlen(text) - 1] != '\n') {
			text[strlen(text)] = '\n';
			finish++;
		}
		fContentView->TextView()->SetText(text, finish - start);
		free(text);

		finish = fContentView->TextView()->CountLines();
		for (int32 loop = 0; loop < finish; loop++) {
			fContentView->TextView()->GoToLine(loop);
			fContentView->TextView()->Insert((const char*)QUOTE);
		}

		if (gReaderSettings->ColoredQuotes()) {
			const BFont* font = fContentView->TextView()->Font();
			int32 length = fContentView->TextView()->TextLength();

			TextRunArray style(length / 8 + 8);

			FillInQuoteTextRuns(fContentView->TextView(), NULL,
				fContentView->TextView()->Text(), length, font, &style.Array(),
				style.MaxEntries());

			fContentView->TextView()->SetRunArray(0, length, &style.Array());
		}

		fContentView->TextView()->GoToLine(0);
		if (preamble.Length() > 0)
			fContentView->TextView()->Insert(preamble);
	} else {
		fContentView->TextView()->LoadMessage(mail, true, preamble);
	}

	fReplying = true;

	// Add auto-signature after the quoted text
	AddAutoSignature(false);
}


status_t
EmailReaderWindow::ComposeReplyTo(entry_ref* ref, uint32 type)
{
	// Load the source email
	uint32 characterSet = gReaderSettings != NULL
		? gReaderSettings->MailCharacterSet() : B_MAIL_NULL_CONVERSION;
	BEmailMessage* sourceMail = new BEmailMessage(ref, characterSet);
	if (sourceMail->InitCheck() != B_OK) {
		delete sourceMail;
		return B_ERROR;
	}

	// Trigger parsing of the mail body before ReplyMessage
	sourceMail->BodyText();
	sourceMail->Body();

	fRepliedMail = *ref;

	if (type == M_REPLY_ALL)
		type = B_MAIL_REPLY_TO_ALL;
	else if (type == M_REPLY_TO_SENDER)
		type = B_MAIL_REPLY_TO_SENDER;
	else
		type = B_MAIL_REPLY_TO;

	uint32 useAccountFrom = gReaderSettings->UseAccountFrom();

	fMail = sourceMail->ReplyMessage(mail_reply_to_mode(type),
		useAccountFrom == ACCOUNT_FROM_MAIL, QUOTE);

	// set header fields
	fHeaderView->SetTo(fMail->To());
	fHeaderView->SetCc(fMail->CC());
	fHeaderView->SetSubject(fMail->Subject());

	int32 accountID;
	BFile file(ref, B_READ_ONLY);
	if (file.ReadAttr("MAIL:reply_with", B_INT32_TYPE, 0, &accountID,
		sizeof(int32)) != B_OK)
		accountID = -1;

	// set mail account
	if ((useAccountFrom == ACCOUNT_FROM_MAIL) || (accountID > -1)) {
		if (useAccountFrom == ACCOUNT_FROM_MAIL)
			fHeaderView->SetAccount(fMail->Account());
		else
			fHeaderView->SetAccount(accountID);
	}

	// create preamble string
	BString preamble = gReaderSettings->ReplyPreamble();

	BString name;
	sourceMail->GetName(&name);
	if (name.Length() <= 0)
		name = B_TRANSLATE("(Name unavailable)");

	BString address(sourceMail->From());
	if (address.Length() > 0) {
		// Extract just the email address from "Name <email>" format
		int32 open = address.FindFirst('<');
		int32 close = address.FindFirst('>', open);
		if (open >= 0 && close > open) {
			BString email;
			address.CopyInto(email, open + 1, close - open - 1);
			address = email;
		}
	}
	if (address.Length() <= 0)
		address = B_TRANSLATE("(Address unavailable)");

	BString date;
	{
		BNode node(ref);
		time_t when = 0;
		// Try 8-byte time_t first, then fall back to 4-byte
		ssize_t size = node.ReadAttr("MAIL:when", B_TIME_TYPE,
			0, &when, sizeof(time_t));
		if (size < (ssize_t)sizeof(time_t)) {
			int32 when32 = 0;
			size = node.ReadAttr("MAIL:when", B_TIME_TYPE,
				0, &when32, sizeof(int32));
			if (size == sizeof(int32))
				when = (time_t)when32;
		}
		if (when > 0) {
			BDateTimeFormat formatter;
			formatter.Format(date, when,
				B_SHORT_DATE_FORMAT, B_SHORT_TIME_FORMAT);

			// Append sender's timezone offset if available
			BString tz = _ExtractTimezoneOffset(
				sourceMail->HeaderField("Date"));
			if (tz.Length() > 0)
				date << " (" << tz << ")";
		}
	}
	if (date.Length() <= 0)
		date = B_TRANSLATE("(Date unavailable)");

	preamble.ReplaceAll("%n", name);
	preamble.ReplaceAll("%e", address);
	preamble.ReplaceAll("%d", date);
	preamble.ReplaceAll("\\n", "\n");

	// No text selection from main window, always load full message
	fContentView->TextView()->LoadMessage(sourceMail, true, preamble);

	fReplying = true;
	
	// Add auto-signature after the quoted text
	AddAutoSignature(false);
	
	// Keep sourceMail alive — TTextView::fMail references it for quoted text,
	// and it must outlive the compose window.
	fSourceMail = sourceMail;
	return B_OK;
}


status_t
EmailReaderWindow::ComposeForwardOf(entry_ref* ref, bool includeAttachments)
{
	// Create source mail exactly like OpenMessage does for incoming mail
	uint32 characterSet = gReaderSettings != NULL
		? gReaderSettings->MailCharacterSet() : B_MAIL_NULL_CONVERSION;
	BEmailMessage* sourceMail = new BEmailMessage(ref, characterSet);
	if (sourceMail->InitCheck() != B_OK) {
		delete sourceMail;
		return B_ERROR;
	}

	// Trigger parsing of the mail body - this may be needed before ForwardMessage works
	sourceMail->BodyText();
	sourceMail->Body();

	// From here, identical to Forward()
	uint32 useAccountFrom = gReaderSettings->UseAccountFrom();

	fMail = sourceMail->ForwardMessage(useAccountFrom == ACCOUNT_FROM_MAIL,
		includeAttachments);

	BFile file(ref, O_RDONLY);
	if (file.InitCheck() < B_NO_ERROR) {
		// fMail from ForwardMessage may reference sourceMail's data,
		// so clean up both to avoid dangling pointers.
		delete fMail;
		fMail = NULL;
		delete sourceMail;
		return B_ERROR;
	}

	fHeaderView->SetSubject(fMail->Subject());

	if (useAccountFrom == ACCOUNT_FROM_MAIL)
		fHeaderView->SetAccount(fMail->Account());

	if (fMail->CountComponents() > 1) {
		// Add attachments from the forwarded email to the attachment strip
		if (fAttachmentStrip != NULL) {
			fAttachmentStrip->AddEnclosuresFromMail(fMail);
			if (fAttachmentStrip->HasAttachments()) {
				if (fAttachmentSeparator->IsHidden())
					fAttachmentSeparator->Show();
				if (fAttachmentStrip->IsHidden())
					fAttachmentStrip->Show();
			}
		}
	}

	fContentView->TextView()->LoadMessage(fMail, false, NULL);
	fChanged = false;
	fFieldState = 0;

	// Add auto-signature before the forwarded text
	AddAutoSignature(true);

	// Keep sourceMail alive — fMail from ForwardMessage() may internally
	// reference sourceMail's data (attachments, components).
	fSourceMail = sourceMail;
	return B_OK;
}


status_t
EmailReaderWindow::Send(bool now)
{
	if (!now) {
		status_t status = SaveAsDraft();
		if (status != B_OK) {
			beep();
			BAlert* alert = new BAlert("", B_TRANSLATE("E-mail draft could "
				"not be saved!"), B_TRANSLATE("OK"));
			alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
			alert->Go();
		} else if (fEmailViewsWindow != NULL) {
			// Notify main window to refresh query counts (draft count changed)
			fEmailViewsWindow->PostMessage(MSG_BACKGROUND_QUERY_UPDATE);
		}
		return status;
	}

	uint32 characterSetToUse = _CurrentCharacterSet();
	mail_encoding encodingForBody = quoted_printable;
	mail_encoding encodingForHeaders = quoted_printable;

	// Set up the encoding to use for converting binary to printable ASCII.
	// Normally this will be quoted printable, but for some old software,
	// particularly Japanese stuff, they only understand base64.  They also
	// prefer it for the smaller size.  Later on this will be reduced to 7bit
	// if the encoded text is just 7bit characters.
	if (characterSetToUse == B_SJIS_CONVERSION
		|| characterSetToUse == B_EUC_CONVERSION)
		encodingForBody = base64;
	else if (characterSetToUse == B_JIS_CONVERSION
		|| characterSetToUse == B_MAIL_US_ASCII_CONVERSION
		|| characterSetToUse == B_ISO1_CONVERSION
		|| characterSetToUse == B_EUC_KR_CONVERSION)
		encodingForBody = eight_bit;

	// Using quoted printable headers on almost completely non-ASCII Japanese
	// is a waste of time.  Besides, some stupid cell phone services need
	// base64 in the headers.
	if (characterSetToUse == B_SJIS_CONVERSION
		|| characterSetToUse == B_EUC_CONVERSION
		|| characterSetToUse == B_JIS_CONVERSION
		|| characterSetToUse == B_EUC_KR_CONVERSION)
		encodingForHeaders = base64;

	// Count the number of characters in the message body which aren't in the
	// currently selected character set.  Also see if the resulting encoded
	// text can safely use 7 bit characters.
	if (fContentView->TextView()->TextLength() > 0) {
		// First do a trial encoding with the user's character set.
		int32 converterState = 0;
		int32 originalLength;
		BString tempString;
		int32 tempStringLength;
		char* tempStringPntr;
		originalLength = fContentView->TextView()->TextLength();
		tempStringLength = originalLength * 6;
			// Some character sets bloat up on escape codes
		tempStringPntr = tempString.LockBuffer (tempStringLength);
		if (tempStringPntr != NULL && mail_convert_from_utf8(characterSetToUse,
				fContentView->TextView()->Text(), &originalLength,
				tempStringPntr, &tempStringLength, &converterState,
				0x1A /* used for unknown characters */) == B_OK) {
			// Check for any characters which don't fit in a 7 bit encoding.
			int i;
			bool has8Bit = false;
			for (i = 0; i < tempStringLength; i++) {
				if (tempString[i] == 0 || (tempString[i] & 0x80)) {
					has8Bit = true;
					break;
				}
			}
			if (!has8Bit)
				encodingForBody = seven_bit;
			tempString.UnlockBuffer (tempStringLength);

			// Count up the number of unencoded characters and warn the user
			if (gReaderSettings->WarnAboutUnencodableCharacters()) {
				// TODO: ideally, the encoding should be silently changed to
				// one that can express this character
				int32 offset = 0;
				int count = 0;
				while (offset >= 0) {
					offset = tempString.FindFirst (0x1A, offset);
					if (offset >= 0) {
						count++;
						offset++;
							// Don't get stuck finding the same character again.
					}
				}
				if (count > 0) {
					int32 userAnswer;
					BString	messageString;
					BString countString;
					countString << count;
					messageString << B_TRANSLATE("Your main text contains %ld"
						" unencodable characters. Perhaps a different "
						"character set would work better? Hit Send to send it "
						"anyway "
						"(a substitute character will be used in place of "
						"the unencodable ones), or choose Cancel to go back "
						"and try fixing it up.");
					messageString.ReplaceFirst("%ld", countString);
					BAlert* alert = new BAlert("Question", messageString.String(),
						B_TRANSLATE("Send"),
						B_TRANSLATE("Cancel"),
						NULL, B_WIDTH_AS_USUAL, B_OFFSET_SPACING,
						B_WARNING_ALERT);
					alert->SetShortcut(1, B_ESCAPE);
					userAnswer = alert->Go();

					if (userAnswer == 1) {
						// Cancel was picked.
						return -1;
					}
				}
			}
		}
	}

	Hide();
		// depending on the system (and I/O) load, this could take a while
		// but the user shouldn't be left waiting

	status_t result;

	if (fResending) {
		BFile file(fRef, O_RDONLY);
		result = file.InitCheck();
		if (result == B_OK) {
			BEmailMessage mail(&file);
			mail.SetTo(fHeaderView->To(), characterSetToUse,
				encodingForHeaders);

			if (fHeaderView->AccountID() != ~0L)
				mail.SendViaAccount(fHeaderView->AccountID());

			result = mail.Send(now);
		}
	} else {
		if (fMail == NULL)
			// the mail will be deleted when the window is closed
			fMail = new BEmailMessage;

		// Had an embarrassing bug where replying to a message and clearing the
		// CC field meant that it got sent out anyway, so pass in empty strings
		// when changing the header to force it to remove the header.

		fMail->SetTo(fHeaderView->To(), characterSetToUse, encodingForHeaders);
		fMail->SetSubject(fHeaderView->Subject(), characterSetToUse,
			encodingForHeaders);
		fMail->SetCC(fHeaderView->Cc(), characterSetToUse, encodingForHeaders);
		fMail->SetBCC(fHeaderView->Bcc());

		//--- Add X-Mailer field
		{
			// get app version
			version_info info;
			memset(&info, 0, sizeof(version_info));

			app_info appInfo;
			if (be_app->GetAppInfo(&appInfo) == B_OK) {
				BFile file(&appInfo.ref, B_READ_ONLY);
				if (file.InitCheck() == B_OK) {
					BAppFileInfo appFileInfo(&file);
					if (appFileInfo.InitCheck() == B_OK)
						appFileInfo.GetVersionInfo(&info, B_APP_VERSION_KIND);
				}
			}

			char versionString[255];
			sprintf(versionString,
				"Mail/Haiku %" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32,
				info.major, info.middle, info.minor);
			fMail->SetHeaderField("X-Mailer", versionString);
		}

		/****/

		// the content text is always added to make sure there is a mail body
		fMail->SetBodyTextTo("");
		fContentView->TextView()->AddAsContent(fMail, gReaderSettings->WrapMode(),
			characterSetToUse, encodingForBody);

		if (fAttachmentStrip != NULL) {
			for (int32 index = 0; index < fAttachmentStrip->CountAttachments(); index++) {
				// Check if it's a file reference (user-added) or component (forwarded)
				const entry_ref* constRef = fAttachmentStrip->AttachmentAt(index);
				BMailComponent* component = fAttachmentStrip->ComponentAt(index);
				
				if (component != NULL) {
					// Forwarded attachment - already a component, just add it
					// Note: The component is owned by fMail from ForwardMessage()
					// so we don't need to add it again - it's already there
					continue;
				}
				
				if (constRef == NULL)
					continue;

				// Make a copy since Mail Kit API expects non-const entry_ref*
				entry_ref ref = *constRef;

				// leave out missing enclosures
				BEntry entry(&ref);
				if (!entry.Exists())
					continue;

				// Check if this is an email file - if so, attach as message/rfc822
				// for compatibility with Gmail, Outlook, and other standard email clients
				BNode node(&ref);
				char mimeType[B_MIME_TYPE_LENGTH];
				BNodeInfo nodeInfo(&node);
				bool isEmail = false;
				if (nodeInfo.GetType(mimeType) == B_OK) {
					if (strcmp(mimeType, "text/x-email") == 0 
						|| strcmp(mimeType, "message/rfc822") == 0) {
						isEmail = true;
					}
				}

				if (isEmail) {
					// Attach email as standard message/rfc822 without x-bfile wrapper
					// Get filename from entry_ref, ensure .eml extension for compatibility
					BPath path(&ref);
					const char* leafName = path.Leaf();
					BString filename;
					if (leafName != NULL) {
						filename = leafName;
						// Add .eml extension if not present (Gmail/Outlook expect it)
						if (filename.FindLast(".eml") != filename.Length() - 4)
							filename.Append(".eml");
					} else {
						filename = "forwarded.eml";
					}
					
					// Create attachment using entry_ref (keeps file reference valid)
					BSimpleMailAttachment* attachment = 
						new BSimpleMailAttachment(&ref);
					attachment->SetFileName(filename.String());
					
					// Use 8bit encoding for message/rfc822 (RFC 2046 Section 5.2.1)
					// This prevents Gmail from also rendering the attachment inline
					attachment->SetEncoding(eight_bit);
					
					// Set Content-Type to message/rfc822 for standard compatibility
					BMessage contentType;
					contentType.AddString("unlabeled", "message/rfc822");
					contentType.AddString("name", filename.String());
					attachment->SetHeaderField("Content-Type", &contentType);
					
					// Set Content-Disposition to attachment (not inline) so Gmail
					// treats it as a downloadable file, not an expanded forwarded message
					BMessage disposition;
					disposition.AddString("unlabeled", "attachment");
					disposition.AddString("filename", filename.String());
					attachment->SetHeaderField("Content-Disposition", &disposition);
					
					fMail->AddComponent(attachment);
				} else {
					fMail->Attach(&ref, gReaderSettings->AttachAttributes());
				}
			}
		}
		if (fHeaderView->AccountID() != ~0L)
			fMail->SendViaAccount(fHeaderView->AccountID());

		result = fMail->Send(now);

		if (fReplying) {
			// Set status of the replied mail

			BNode node(&fRepliedMail);
			if (node.InitCheck() >= B_OK) {
				if (fOriginatingWindow) {
					BMessage msg(M_SAVE_POSITION), reply;
					fOriginatingWindow->SendMessage(&msg, &reply);
				}
				WriteAttrString(&node, B_MAIL_ATTR_STATUS, "Replied");
			}
		}
	}

	bool close = false;
	BString errorMessage;

	switch (result) {
		case B_OK:
			close = true;
			fSent = true;

			// If it's a draft, remove the draft file
			if (fDraft) {
				BEntry entry(fRef);
				entry.Remove();
			}
			break;

		case B_MAIL_NO_DAEMON:
		{
			close = true;
			fSent = true;

			BAlert* alert = new BAlert("no daemon",
				B_TRANSLATE("The mail_daemon is not running. The message is "
					"queued and will be sent when the mail_daemon is started."),
				B_TRANSLATE("Start now"), B_TRANSLATE("OK"));
			alert->SetShortcut(1, B_ESCAPE);
			int32 start = alert->Go();

			if (start == 0) {
				BMailDaemon daemon;
				result = daemon.Launch();
				if (result == B_OK) {
					daemon.SendQueuedMail();
				} else {
					errorMessage
						<< B_TRANSLATE("The mail_daemon could not be "
							"started:\n\t")
						<< strerror(result);
				}
			}
			break;
		}

//		case B_MAIL_UNKNOWN_HOST:
//		case B_MAIL_ACCESS_ERROR:
//			sprintf(errorMessage,
//				"An error occurred trying to connect with the SMTP "
//				"host.  Check your SMTP host name.");
//			break;
//
//		case B_MAIL_NO_RECIPIENT:
//			sprintf(errorMessage,
//				"You must have either a \"To\" or \"Bcc\" recipient.");
//			break;

		default:
			errorMessage << "An error occurred trying to send mail:\n\t"
				<< strerror(result);
			break;
	}

	if (result != B_NO_ERROR && result != B_MAIL_NO_DAEMON) {
		beep();
		BAlert* alert = new BAlert("", errorMessage.String(),
			B_TRANSLATE("OK"));
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
		alert->Go();
	}
	if (close) {
		PostMessage(B_QUIT_REQUESTED);
	} else {
		// The window was hidden earlier
		Show();
	}

	return result;
}


status_t
EmailReaderWindow::SaveAsDraft()
{
	BPath draftPath;
	BDirectory dir;
	BFile draft;
	uint32 flags = 0;

	if (fDraft) {
		status_t status = draft.SetTo(fRef,
				B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		if (status != B_OK)
			return status;
	} else {
		// Get the user home directory
		status_t status = find_directory(B_USER_DIRECTORY, &draftPath);
		if (status != B_OK)
			return status;

		// Append the relative path of the draft directory
		draftPath.Append(kDraftPath);

		// Create the file
		status = dir.SetTo(draftPath.Path());
		switch (status) {
			// Create the directory if it does not exist
			case B_ENTRY_NOT_FOUND:
				if ((status = dir.CreateDirectory(draftPath.Path(), &dir))
					!= B_OK)
					return status;
			case B_OK:
			{
				char fileName[B_FILE_NAME_LENGTH];
				// save as some version of the message's subject
				if (fHeaderView->IsSubjectEmpty()) {
					strlcpy(fileName, B_TRANSLATE("Untitled"),
						sizeof(fileName));
				} else {
					strlcpy(fileName, fHeaderView->Subject(), sizeof(fileName));
				}

				uint32 originalLength = strlen(fileName);

				// convert /, \ and : to -
				for (char* bad = fileName; (bad = strchr(bad, '/')) != NULL;
						++bad) {
					*bad = '-';
				}
				for (char* bad = fileName; (bad = strchr(bad, '\\')) != NULL;
						++bad) {
					*bad = '-';
				}
				for (char* bad = fileName; (bad = strchr(bad, ':')) != NULL;
						++bad) {
					*bad = '-';
				}

				// Create the file; if the name exists, find a unique name
				flags = B_WRITE_ONLY | B_CREATE_FILE | B_FAIL_IF_EXISTS;
				int32 i = 1;
				do {
					status = draft.SetTo(&dir, fileName, flags);
					if (status == B_OK)
						break;
					char appendix[B_FILE_NAME_LENGTH];
					sprintf(appendix, " %" B_PRId32, i++);
					int32 pos = min_c(sizeof(fileName) - strlen(appendix),
						originalLength);
					sprintf(fileName + pos, "%s", appendix);
				} while (status == B_FILE_EXISTS);
				if (status != B_OK)
					return status;

				// Cache the ref
				if (fRef == NULL)
					fRef = new entry_ref;
				BEntry entry(&dir, fileName);
				entry.GetRef(fRef);
				break;
			}
			default:
				return status;
		}
	}

	// Write the content of the message
	draft.Write(fContentView->TextView()->Text(),
		fContentView->TextView()->TextLength());

	// Add the header stuff as attributes
	WriteAttrString(&draft, B_MAIL_ATTR_NAME, fHeaderView->To());
	WriteAttrString(&draft, B_MAIL_ATTR_TO, fHeaderView->To());
	WriteAttrString(&draft, B_MAIL_ATTR_SUBJECT, fHeaderView->Subject());
	
	// Get the from address - for compose windows, build it from the account
	if (fHeaderView->From() != NULL && fHeaderView->From()[0] != '\0') {
		WriteAttrString(&draft, B_MAIL_ATTR_FROM, fHeaderView->From());
	} else {
		// Build from address from account settings
		BMailAccounts accounts;
		BMailAccountSettings* account = accounts.AccountByID(fHeaderView->AccountID());
		if (account != NULL) {
			BString from;
			from << account->RealName() << " <" << account->ReturnAddress() << ">";
			WriteAttrString(&draft, B_MAIL_ATTR_FROM, from.String());
		}
	}
	
	if (!fHeaderView->IsCcEmpty())
		WriteAttrString(&draft, B_MAIL_ATTR_CC, fHeaderView->Cc());
	if (!fHeaderView->IsBccEmpty())
		WriteAttrString(&draft, B_MAIL_ATTR_BCC, fHeaderView->Bcc());

	// Add account
	if (fHeaderView->AccountName() != NULL) {
		WriteAttrString(&draft, B_MAIL_ATTR_ACCOUNT,
			fHeaderView->AccountName());
	}

	// Add encoding
	BMenuItem* menuItem = fEncodingMenu->FindMarked();
	if (menuItem != NULL)
		WriteAttrString(&draft, "MAIL:encoding", menuItem->Label());

	// Add the draft attribute for indexing
	uint32 draftAttr = true;
	draft.WriteAttr("MAIL:draft", B_INT32_TYPE, 0, &draftAttr, sizeof(uint32));

	// Add Attachment paths in attribute
	if (fAttachmentStrip != NULL && fAttachmentStrip->IsComposeMode()) {
		BString pathStr;

		for (int32 i = 0; i < fAttachmentStrip->CountAttachments(); i++) {
			const entry_ref* ref = fAttachmentStrip->AttachmentAt(i);
			if (ref == NULL)
				continue;
			
			if (i > 0)
				pathStr.Append(":");

			BEntry entry(ref, true);
			if (!entry.Exists())
				continue;

			BPath path;
			entry.GetPath(&path);
			pathStr.Append(path.Path());
		}
		if (pathStr.Length())
			draft.WriteAttrString("MAIL:attachments", &pathStr);
	}

	// Set the MIME Type of the file
	BNodeInfo info(&draft);
	info.SetType(kDraftType);

	fDraft = true;
	fChanged = false;

	fToolBar->SetActionEnabled(M_SAVE_AS_DRAFT, false);

	return B_OK;
}


status_t
EmailReaderWindow::TrainMessageAs(const char* commandWord)
{
	status_t	errorCode = -1;
	BEntry		fileEntry;
	BPath		filePath;
	BMessage	replyMessage;
	BMessage	scriptingMessage;
	team_id		serverTeam;

	if (fRef == NULL)
		goto ErrorExit; // Need to have a real file and name.
	errorCode = fileEntry.SetTo(fRef, true);
	if (errorCode != B_OK)
		goto ErrorExit;
	errorCode = fileEntry.GetPath(&filePath);
	if (errorCode != B_OK)
		goto ErrorExit;
	fileEntry.Unset();

	// Get a connection to the spam database server.  Launch if needed.

	if (!fMessengerToSpamServer.IsValid()) {
		// Make sure the server is running.
		if (!be_roster->IsRunning (kSpamServerSignature)) {
			errorCode = be_roster->Launch (kSpamServerSignature);
			if (errorCode != B_OK) {
				BPath path;
				entry_ref ref;
				directory_which places[] = {B_SYSTEM_NONPACKAGED_BIN_DIRECTORY,
					B_SYSTEM_BIN_DIRECTORY};
				for (int32 i = 0; i < 2; i++) {
					find_directory(places[i],&path);
					path.Append("spamdbm");
					if (!BEntry(path.Path()).Exists())
						continue;
					get_ref_for_path(path.Path(),&ref);

					errorCode = be_roster->Launch(&ref);
					if (errorCode == B_OK)
						break;
				}
				if (errorCode != B_OK)
					goto ErrorExit;
			}
		}

		// Set up the messenger to the database server.
		errorCode = B_SERVER_NOT_FOUND;
		serverTeam = be_roster->TeamFor(kSpamServerSignature);
		if (serverTeam < 0)
			goto ErrorExit;

		fMessengerToSpamServer = BMessenger (kSpamServerSignature, serverTeam,
			&errorCode);

		if (!fMessengerToSpamServer.IsValid())
			goto ErrorExit;
	}

	// Ask the server to train on the message.  Give it the command word and
	// the absolute path name to use.

	scriptingMessage.MakeEmpty();
	scriptingMessage.what = B_SET_PROPERTY;
	scriptingMessage.AddSpecifier(commandWord);
	errorCode = scriptingMessage.AddData("data", B_STRING_TYPE,
		filePath.Path(), strlen(filePath.Path()) + 1, false);
	if (errorCode != B_OK)
		goto ErrorExit;
	replyMessage.MakeEmpty();
	errorCode = fMessengerToSpamServer.SendMessage(&scriptingMessage,
		&replyMessage);
	if (errorCode != B_OK
		|| replyMessage.FindInt32("error", &errorCode) != B_OK
		|| errorCode != B_OK)
		goto ErrorExit; // Classification failed in one of many ways.

	SetTitleForMessage();
		// Update window title to show new spam classification.
	return B_OK;

ErrorExit:
	beep();
	char errorString[1500];
	snprintf(errorString, sizeof(errorString), "Unable to train the message "
		"file \"%s\" as %s.  Possibly useful error code: %s (%" B_PRId32 ").",
		filePath.Path(), commandWord, strerror(errorCode), errorCode);
	BAlert* alert = new BAlert("", errorString,	B_TRANSLATE("OK"));
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	alert->Go();

	return errorCode;
}


void
EmailReaderWindow::SetTitleForMessage()
{
	// Figure out the title of this message and set the title bar
	BString title = B_TRANSLATE("Viewer");

	if (fIncoming) {
		BString senderName;
		if (fMail->GetName(&senderName) == B_OK && senderName.Length() > 0)
			title << " - " << senderName << ": \"" << fMail->Subject() << "\"";
		else
			title << " - " << fMail->Subject();

		if (fDownloading)
			title.Prepend("Downloading: ");

		if (gReaderSettings->ShowSpamGUI() && fRef != NULL) {
			BString	classification;
			BNode node(fRef);
			char numberString[30];
			BString oldTitle(title);
			float spamRatio;
			if (node.InitCheck() != B_OK || node.ReadAttrString(
					"MAIL:classification", &classification) != B_OK)
				classification = "Unrated";
			if (classification != "Spam" && classification != "Genuine") {
				// Uncertain, Unrated and other unknown classes, show the ratio.
				if (node.InitCheck() == B_OK && node.ReadAttr("MAIL:ratio_spam",
						B_FLOAT_TYPE, 0, &spamRatio, sizeof(spamRatio))
							== sizeof(spamRatio)) {
					sprintf(numberString, "%.4f", spamRatio);
					classification << " " << numberString;
				}
			}
			title = "";
			title << "[" << classification << "] " << oldTitle;
		}
	}
	SetTitle(title);
}


/*!	Open *another* message in the existing mail window.  Some code here is
	duplicated from various constructors.
	TODO: The duplicated code should be moved to a private initializer method
*/
status_t
EmailReaderWindow::OpenMessage(const entry_ref* ref, uint32 characterSetForDecoding)
{
	if (ref == NULL)
		return B_ERROR;

	// Set some references to the email file
	delete fRef;
	fRef = new entry_ref(*ref);

	fContentView->TextView()->StopLoad();
	delete fMail;
	fMail = NULL;

	BFile file(fRef, B_READ_ONLY);
	status_t err = file.InitCheck();
	if (err != B_OK)
		return err;

	char mimeType[256];
	BNodeInfo fileInfo(&file);
	fileInfo.GetType(mimeType);

	if (strcmp(mimeType, B_PARTIAL_MAIL_TYPE) == 0) {
		BMessenger listener(this);
		status_t status = BMailDaemon().FetchBody(*ref, &listener);
		if (status != B_OK)
			fprintf(stderr, "Could not fetch body: %s\n", strerror(status));
		fileInfo.GetType(mimeType);
		_SetDownloading(true);
	} else
		_SetDownloading(false);

	// Check if it's a draft file, which contains only the text, and has the
	// from, to, bcc, attachments listed as attributes.
	if (strcmp(kDraftType, mimeType) == 0) {
		BNode node(fRef);
		off_t size;
		BString string;

		fMail = new BEmailMessage; // Not really used much, but still needed.

		// Load the raw UTF-8 text from the file.
		file.GetSize(&size);
		fContentView->TextView()->SetText(&file, 0, size);

		// Restore Fields from attributes
		if (node.ReadAttrString(B_MAIL_ATTR_TO, &string) == B_OK)
			fHeaderView->SetTo(string);
		if (node.ReadAttrString(B_MAIL_ATTR_SUBJECT, &string) == B_OK)
			fHeaderView->SetSubject(string);
		if (node.ReadAttrString(B_MAIL_ATTR_CC, &string) == B_OK)
			fHeaderView->SetCc(string);
		if (node.ReadAttrString(B_MAIL_ATTR_BCC, &string) == B_OK)
			fHeaderView->SetBcc(string);

		// Restore account
		if (node.ReadAttrString(B_MAIL_ATTR_ACCOUNT, &string) == B_OK)
			fHeaderView->SetAccount(string);

		// Restore encoding
		if (node.ReadAttrString("MAIL:encoding", &string) == B_OK) {
			BMenuItem* encodingItem = fEncodingMenu->FindItem(string.String());
			if (encodingItem != NULL)
				encodingItem->SetMarked(true);
		}

		// Restore attachments
		if (node.ReadAttrString("MAIL:attachments", &string) == B_OK
			&& string.Length() > 0) {
			BMessage msg(REFS_RECEIVED);
			entry_ref enc_ref;

			BStringList list;
			string.Split(":", false, list);
			for (int32 i = 0; i < list.CountStrings(); i++) {
				BEntry entry(list.StringAt(i), true);
				if (entry.Exists()) {
					entry.GetRef(&enc_ref);
					msg.AddRef("refs", &enc_ref);
				}
			}
			AddEnclosure(&msg);
		}

		// restore the reading position if available
		PostMessage(M_READ_POS);

		PostMessage(RESET_BUTTONS);
		fIncoming = false;
		fDraft = true;
	} else {
		// A real mail message, parse its headers to get from, to, etc.
		fMail = new BEmailMessage(fRef, characterSetForDecoding);
		fIncoming = true;
		fHeaderView->SetFromMessage(fMail);
	}

	err = fMail->InitCheck();
	if (err < B_OK) {
		delete fMail;
		fMail = NULL;
		return err;
	}

	SetTitleForMessage();

	if (fIncoming) {
		//	Put the addresses in the 'Save Address' Menu
		BMenuItem* item;
		while ((item = fSaveAddrMenu->RemoveItem((int32)0)) != NULL)
			delete item;

		// create the list of addresses + names

		BList addressList;
		get_address_list(addressList, fMail->To(), extract_address);
		get_address_list(addressList, fMail->CC(), extract_address);
		get_address_list(addressList, fMail->From(), extract_address);
		get_address_list(addressList, fMail->ReplyTo(), extract_address);

		BList nameList;
		get_address_list(nameList, fMail->To(), extract_address_name);
		get_address_list(nameList, fMail->CC(), extract_address_name);
		get_address_list(nameList, fMail->From(), extract_address_name);
		get_address_list(nameList, fMail->ReplyTo(), extract_address_name);

		BMessage* msg;

		for (int32 i = addressList.CountItems(); i-- > 0;) {
			char* address = (char*)addressList.RemoveItem((int32)0);
			char* name = (char*)nameList.RemoveItem((int32)0);

			// insert the new address in alphabetical order
			int32 index = 0;
			while ((item = fSaveAddrMenu->ItemAt(index)) != NULL) {
				if (!strcmp(address, item->Label())) {
					// item already in list
					goto skip;
				}

				if (strcmp(address, item->Label()) < 0)
					break;

				index++;
			}

			msg = new BMessage(M_SAVE);
			msg->AddString("address", address);
			msg->AddString("name", name);
			fSaveAddrMenu->AddItem(new BMenuItem(address, msg), index);

		skip:
			free(address);
			free(name);
		}

		// Clear out existing contents of text view.
		fContentView->TextView()->SetText("", (int32)0);

		// Hide attachment links in text when using attachment strip
		if (fAttachmentStrip != NULL)
			fContentView->TextView()->SetShowAttachmentLinks(false);

		fContentView->TextView()->LoadMessage(fMail, false, NULL);

		// Populate attachment strip for incoming emails
		if (fAttachmentStrip != NULL) {
			// Create a copy of the email for the attachment strip
			// (it takes ownership and needs its own BEmailMessage)
			BEmailMessage* emailCopy = new BEmailMessage(fRef, characterSetForDecoding);
			BPath emailPath(fRef);
			fAttachmentStrip->SetEmailPath(emailPath.Path());
			fAttachmentStrip->SetAttachments(emailCopy);
			
			// Show/hide separator and strip based on whether there are attachments
			if (fAttachmentStrip->HasAttachments()) {
				if (fAttachmentSeparator != NULL && fAttachmentSeparator->IsHidden())
					fAttachmentSeparator->Show();
				if (fAttachmentStrip->IsHidden())
					fAttachmentStrip->Show();
			} else {
				if (fAttachmentSeparator != NULL && !fAttachmentSeparator->IsHidden())
					fAttachmentSeparator->Hide();
				if (!fAttachmentStrip->IsHidden())
					fAttachmentStrip->Hide();
			}
			
			// Check for HTML content and show/hide the HTML version button
			// Clear previous HTML content
			free(fHtmlBodyContent);
			fHtmlBodyContent = NULL;
			fHtmlBodyContentSize = 0;
			
			bool hasHtml = false;
			
			// Check if the email body is HTML (same check as TTextView::LoadMessage)
			BTextMailComponent* body = fMail->Body();
			if (body != NULL) {
				BMimeType mimeType;
				if (body->MIMEType(&mimeType) == B_OK && mimeType == "text/html") {
					hasHtml = true;
					// Extract raw HTML content preserving original charset
					void* rawHtml = NULL;
					size_t rawHtmlSize = 0;
					// Try multipart extraction first, then simple body extraction
					if (fAttachmentStrip->ExtractRawHtmlFromEmail(
							emailPath.Path(), &rawHtml, &rawHtmlSize)) {
						fHtmlBodyContent = rawHtml;
						fHtmlBodyContentSize = rawHtmlSize;
					} else if (fAttachmentStrip->ExtractRawBodyFromEmail(
							emailPath.Path(), &rawHtml, &rawHtmlSize)) {
						fHtmlBodyContent = rawHtml;
						fHtmlBodyContentSize = rawHtmlSize;
					}
				}
			}
			
			// If body is not HTML, check for HTML alternative in multipart email
			if (!hasHtml) {
				// Iterate through email components to find text/html part
				for (int32 i = 0; i < fMail->CountComponents(); i++) {
					BMailComponent* component = fMail->GetComponent(i);
					if (component == NULL || component == body)
						continue;
					
					BMimeType mimeType;
					if (component->MIMEType(&mimeType) == B_OK && mimeType == "text/html") {
						hasHtml = true;
						// Extract raw HTML content preserving original charset
						void* rawHtml = NULL;
						size_t rawHtmlSize = 0;
						if (fAttachmentStrip->ExtractRawHtmlFromEmail(
								emailPath.Path(), &rawHtml, &rawHtmlSize)) {
							fHtmlBodyContent = rawHtml;
							fHtmlBodyContentSize = rawHtmlSize;
						}
						break;
					}
				}
			}
			
			// Show/hide HTML version button
			if (fHtmlVersionButton != NULL) {
				if (hasHtml && fHtmlVersionButton->IsHidden()) {
					fHtmlVersionButton->Show();
				} else if (!hasHtml && !fHtmlVersionButton->IsHidden()) {
					fHtmlVersionButton->Hide();
				}
			}
		}

		if (gReaderSettings->ShowToolBar())
			_UpdateReadButton();

		if (fIncoming)
			_UpdateNavigationButtons();
	}

	return B_OK;
}


EmailReaderWindow*
EmailReaderWindow::FrontmostWindow()
{
	BAutolock locker(sWindowListLock);
	if (sWindowList.CountItems() > 0)
		return (EmailReaderWindow*)sWindowList.ItemAt(0);

	return NULL;
}


EmailReaderWindow*
EmailReaderWindow::FindWindow(const entry_ref& ref)
{
	BAutolock locker(sWindowListLock);
	
	for (int32 i = 0; i < sWindowList.CountItems(); i++) {
		EmailReaderWindow* window = (EmailReaderWindow*)sWindowList.ItemAt(i);
		if (window->fRef != NULL && *window->fRef == ref)
			return window;
	}
	
	return NULL;
}


// #pragma mark - EmailViews Integration


void
EmailReaderWindow::SetEmailViewsWindow(EmailViewsWindow* window)
{
	fEmailViewsWindow = window;
}


// #pragma mark -


void
EmailReaderWindow::_CreateNewPerson(BString address, BString name)
{
	BMessage message(M_LAUNCH_PEOPLE);
	message.AddString("META:name", name);
	message.AddString("META:email", address);

	status_t result = be_roster->Launch("application/x-person", &message);

	if ((result != B_OK) && (result != B_ALREADY_RUNNING)) {
		BAlert* alert = new BAlert("", B_TRANSLATE(
			"Sorry, could not find an application that "
			"supports the 'Person' data type."),
			B_TRANSLATE("OK"));
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
		alert->Go();
	}
}


void
EmailReaderWindow::_AddReadButton()
{
	BNode node(fRef);

	read_flags flag = B_UNREAD;
	read_read_attr(node, flag);

	if (flag == B_READ) {
		fToolBar->SetActionVisible(M_UNREAD, true);
		fToolBar->SetActionVisible(M_READ, false);
	} else {
		fToolBar->SetActionVisible(M_UNREAD, false);
		fToolBar->SetActionVisible(M_READ, true);
	}
}


void
EmailReaderWindow::_UpdateReadButton()
{
	if (gReaderSettings->ShowToolBar()) {
		if (!fAutoMarkRead && fIncoming)
			_AddReadButton();
		else {
			fToolBar->SetActionVisible(M_UNREAD, false);
			fToolBar->SetActionVisible(M_READ, false);
		}
	}
	UpdateViews();
}


void
EmailReaderWindow::_UpdateNavigationButtons()
{
	if (fEmailViewsWindow == NULL || fRef == NULL)
		return;

	// Lock the main window to safely query the email list.
	// Use timeout to avoid blocking if the main window is busy.
	if (fEmailViewsWindow->LockLooperWithTimeout(200000) != B_OK)
		return;

	bool hasNext = fEmailViewsWindow->HasNextEmail(fRef);
	bool hasPrev = fEmailViewsWindow->HasPrevEmail(fRef);

	fEmailViewsWindow->UnlockLooper();

	fToolBar->SetActionEnabled(M_NEXTMSG, hasNext);
	fToolBar->SetActionEnabled(M_PREVMSG, hasPrev);
}


void
EmailReaderWindow::AddAutoSignature(bool beforeQuotedText, bool resetChanged)
{
	if (fIncoming || fSigAdded)
		return;

	BString signature = gReaderSettings->Signature();

	if (strcmp(signature.String(), B_TRANSLATE("None")) == 0)
		return;

	// Create a query to find this signature
	BVolume volume;
	BVolumeRoster().GetBootVolume(&volume);

	BQuery query;
	query.SetVolume(&volume);
	query.PushAttr(INDEX_SIGNATURE);
	query.PushString(signature.String());
	query.PushOp(B_EQ);
	query.Fetch();

	// If we find the named signature, add it to the text
	BEntry entry;
	if (query.GetNextEntry(&entry) == B_NO_ERROR) {
		BFile file;
		file.SetTo(&entry, O_RDWR);
		if (file.InitCheck() == B_NO_ERROR) {
			entry_ref ref;
			entry.GetRef(&ref);

			BMessage msg(M_SIGNATURE);
			msg.AddRef("ref", &ref);
			if (beforeQuotedText)
				msg.AddInt32("insert_at", 0);
			PostMessage(&msg);
			
			// For new blank emails, reset fChanged after signature insertion
			// to avoid "save as draft?" prompt when closing without changes
			if (resetChanged) {
				BMessage resetMsg(M_RESET_CHANGED);
				new BMessageRunner(BMessenger(this), &resetMsg, 100000, 1);  // 100ms delay, run once
			}
		}
	} else {
		// Query failed - nothing to do
	}
}


void
EmailReaderWindow::_SetDownloading(bool downloading)
{
	fDownloading = downloading;
}


uint32
EmailReaderWindow::_CurrentCharacterSet() const
{
	uint32 defaultCharSet = fResending || !fIncoming
		? gReaderSettings->MailCharacterSet() : B_MAIL_NULL_CONVERSION;

	BMenuItem* marked = fEncodingMenu->FindMarked();
	if (marked == NULL)
		return defaultCharSet;

	return marked->Message()->GetInt32("charset", defaultCharSet);
}
