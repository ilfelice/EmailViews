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


#include "Prefs.h"
#include "ReaderSettings.h"

#include <private/interface/Spinner.h>

#include <Catalog.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include <Application.h>
#include <private/textencoding/CharacterSet.h>
#include <private/textencoding/CharacterSetRoster.h>
#include <E-mail.h>
#include <FindDirectory.h>
#include <GridView.h>
#include <LayoutBuilder.h>
#include <InterfaceKit.h>
#include <Locale.h>
#include <ListView.h>
#include <MailSettings.h>
#include <mail_encoding.h>
#include <Node.h>
#include <Path.h>
#include <Query.h>
#include <ScrollView.h>
#include <StorageKit.h>
#include <String.h>
#include <StringList.h>
#include <TabView.h>
#include <Volume.h>
#include <VolumeRoster.h>

using namespace BPrivate;

#include "ReaderSupport.h"
#include "EmailReaderWindow.h"
#include "Messages.h"
#include "Signature.h"


#define B_TRANSLATION_CONTEXT "Preferences"

enum P_MESSAGES {
	P_OK = 128, P_CANCEL, P_REVERT, P_FONT,
	P_SIZE, P_LEVEL, P_WRAP, P_ATTACH_ATTRIBUTES,
	P_SIG, P_ENC, P_WARN_UNENCODABLE,
	P_SPELL_CHECK_START_ON, P_BUTTON_BAR,
	P_ACCOUNT, P_REPLYTO, P_REPLY_PREAMBLE,
	P_COLORED_QUOTES, P_MARK_READ, P_SHOW_TIME_RANGE,
	P_USE_SYSTEM_FONT_SIZE,
	P_BLOCK_ADD, P_BLOCK_REMOVE, P_BLOCK_SELECTION, P_BLOCK_FILTER
};


static inline void
add_menu_to_layout(BMenuField* menu, BGridLayout* layout, int32& row)
{
	menu->SetAlignment(B_ALIGN_RIGHT);
	layout->AddItem(menu->CreateLabelLayoutItem(), 0, row);
	layout->AddItem(menu->CreateMenuBarLayoutItem(), 1, row, 2);
	row++;
}


// #pragma mark -


TPrefsWindow::TPrefsWindow(BPoint leftTop, BFont* font, int32* level,
	bool* wrap, bool* attachAttributes, bool* cquotes, int32* account,
	int32* replyTo, char** preamble, char** sig, uint32* encoding,
	bool* warnUnencodable, bool* spellCheckStartOn, bool* autoMarkRead,
	uint8* buttonBar, bool* showTimeRange, bool* useSystemFontSize)
	:
	BWindow(BRect(leftTop.x, leftTop.y, leftTop.x + 100, leftTop.y + 100),
		B_TRANSLATE("Email preferences"),
		B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE
			| B_AUTO_UPDATE_SIZE_LIMITS),

	fNewWrap(wrap),
	fWrap(*fNewWrap),

	fNewAttachAttributes(attachAttributes),
	fAttachAttributes(*fNewAttachAttributes),

	fNewButtonBar(buttonBar),
	fButtonBar(*fNewButtonBar),

	fNewColoredQuotes(cquotes),
	fColoredQuotes(*fNewColoredQuotes),

	fNewAccount(account),
	fAccount(*fNewAccount),

	fNewReplyTo(replyTo),
	fReplyTo(*fNewReplyTo),

	fNewPreamble(preamble),

	fNewSignature(sig),
	fSignature((char*)malloc(strlen(*fNewSignature) + 1)),

	fNewFont(font),
	fFont(*fNewFont),

	fNewEncoding(encoding),
	fEncoding(*fNewEncoding),

	fNewWarnUnencodable(warnUnencodable),
	fWarnUnencodable(*fNewWarnUnencodable),

	fNewSpellCheckStartOn(spellCheckStartOn),
	fSpellCheckStartOn(*fNewSpellCheckStartOn),

	fNewAutoMarkRead(autoMarkRead),
	fAutoMarkRead(*autoMarkRead),

	fNewShowTimeRange(showTimeRange),
	fShowTimeRange(*showTimeRange),
	fNewUseSystemFontSize(useSystemFontSize),
	fUseSystemFontSize(*useSystemFontSize)
{
	strcpy(fSignature, *fNewSignature);

	BMenuField* menu;

	// Tab view with separate tabs for User Interface and Mailing settings.
	// Each tab has its own independent grid layout, avoiding column width
	// coupling that caused the Reply preamble text field to be too narrow.

	BTabView* tabView = new BTabView("prefTabs", B_WIDTH_FROM_LABEL);

	BGridView* interfaceView = new BGridView(B_TRANSLATE("User interface"));
	BGridLayout* interfaceLayout = interfaceView->GridLayout();
	interfaceLayout->SetInsets(B_USE_DEFAULT_SPACING);

	BGridView* mailView = new BGridView(B_TRANSLATE("Mailing"));
	BGridLayout* mailLayout = mailView->GridLayout();
	mailLayout->SetInsets(B_USE_DEFAULT_SPACING);

	tabView->AddTab(interfaceView);
	tabView->AddTab(mailView);

	// Spam filtering tab
	BView* spamView = new BView(B_TRANSLATE("Spam filtering"), 0);
	spamView->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	fBlocklistView = new BListView("blocklist");
	fBlocklistView->SetSelectionMessage(new BMessage(P_BLOCK_SELECTION));
	fBlocklistScrollView = new BScrollView("blocklist_scroll",
		fBlocklistView, 0, false, true);

	fBlockFilterField = new BTextControl("blockFilter", NULL, "", NULL);
	fBlockFilterField->SetModificationMessage(new BMessage(P_BLOCK_FILTER));
	fBlockFilterField->TextView()->SetMaxBytes(256);

	fBlockAddressField = new BTextControl("blockAddress", NULL,
		"", NULL);
	fBlockAddressField->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));

	fAddBlockButton = new BButton("add", B_TRANSLATE("Add"),
		new BMessage(P_BLOCK_ADD));
	fRemoveBlockButton = new BButton("remove", B_TRANSLATE("Remove"),
		new BMessage(P_BLOCK_REMOVE));
	fRemoveBlockButton->SetEnabled(false);

	BLayoutBuilder::Group<>(spamView, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.Add(new BStringView("blockLabel",
			B_TRANSLATE("Blocked senders and domains:")))
		.AddGroup(B_HORIZONTAL, B_USE_HALF_ITEM_SPACING)
			.Add(new BStringView("filterLabel", B_TRANSLATE("Filter:")))
			.Add(fBlockFilterField)
		.End()
		.Add(fBlocklistScrollView, 1.0f)
		.AddGroup(B_HORIZONTAL)
			.Add(fBlockAddressField)
			.Add(fAddBlockButton)
			.Add(fRemoveBlockButton)
			.AddGlue()
		.End();

	_LoadBlocklist();

	tabView->AddTab(spamView);

	tabView->SetBorder(B_NO_BORDER);

	// revert, ok & cancel

	BButton* okButton = new BButton("ok", B_TRANSLATE("OK"),
		new BMessage(P_OK));
	okButton->MakeDefault(true);

	BButton* cancelButton = new BButton("cancel", B_TRANSLATE("Cancel"),
		new BMessage(P_CANCEL));

	fRevert = new BButton("revert", B_TRANSLATE("Revert"),
		new BMessage(P_REVERT));
	fRevert->SetEnabled(false);

	// User Interface
	int32 layoutRow = 0;

	fButtonBarMenu = _BuildButtonBarMenu(*buttonBar);
	menu = new BMenuField("bar", B_TRANSLATE("Toolbar:"), fButtonBarMenu);
	add_menu_to_layout(menu, interfaceLayout, layoutRow);

	fShowTimeRangeMenu = _BuildShowTimeRangeMenu(fShowTimeRange);
	menu = new BMenuField("showTimeRange",
		B_TRANSLATE("Time range slider:"),
		fShowTimeRangeMenu);
	add_menu_to_layout(menu, interfaceLayout, layoutRow);

	fFontMenu = _BuildFontMenu(font);
	menu = new BMenuField("font", B_TRANSLATE("Font:"), fFontMenu);
	add_menu_to_layout(menu, interfaceLayout, layoutRow);

	// Size spinner + "Use system font size" checkbox on the same row.
	// If the flag is on, show the current system size and disable the spinner.
	int32 currentSize = (int32)gReaderSettings->ContentFont().Size();

	BStringView* sizeLabel = new BStringView("sizeLabel", B_TRANSLATE("Size:"));
	sizeLabel->SetAlignment(B_ALIGN_RIGHT);

	fSizeSpinner = new BSpinner("size", "", new BMessage(P_SIZE));
	fSizeSpinner->SetRange(7, 72);
	fSizeSpinner->SetValue(currentSize);
	fSizeSpinner->SetEnabled(!fUseSystemFontSize);

	fUseSystemFontSizeCheckBox = new BCheckBox("useSystemFontSize",
		B_TRANSLATE("Use system font size"),
		new BMessage(P_USE_SYSTEM_FONT_SIZE));
	fUseSystemFontSizeCheckBox->SetValue(
		fUseSystemFontSize ? B_CONTROL_ON : B_CONTROL_OFF);

	// Separate label in col 0 for alignment with other rows.
	// Spinner as whole view in col 1 for correct row height.
	// Checkbox in col 2.
	interfaceLayout->AddView(sizeLabel, 0, layoutRow);
	interfaceLayout->AddView(fSizeSpinner, 1, layoutRow);
	interfaceLayout->AddView(fUseSystemFontSizeCheckBox, 2, layoutRow);
	layoutRow++;

	fColoredQuotesMenu = _BuildColoredQuotesMenu(fColoredQuotes);
	menu = new BMenuField("cquotes", B_TRANSLATE("Colored quotes:"),
		fColoredQuotesMenu);
	add_menu_to_layout(menu, interfaceLayout, layoutRow);

	fSpellCheckStartOnMenu = _BuildSpellCheckStartOnMenu(fSpellCheckStartOn);
	menu = new BMenuField("spellCheckStartOn",
		B_TRANSLATE("Initial spell check mode:"),
		fSpellCheckStartOnMenu);
	add_menu_to_layout(menu, interfaceLayout, layoutRow);

	fAutoMarkReadMenu = _BuildAutoMarkReadMenu(fAutoMarkRead);
	menu = new BMenuField("autoMarkRead",
		B_TRANSLATE("Automatically mark mail as read:"),
		fAutoMarkReadMenu);
	add_menu_to_layout(menu, interfaceLayout, layoutRow);
	// Mail Accounts

	layoutRow = 0;

	fAccountMenu = _BuildAccountMenu(fAccount);
	menu = new BMenuField("account", B_TRANSLATE("Default account:"),
		fAccountMenu);
	add_menu_to_layout(menu, mailLayout, layoutRow);

	fReplyToMenu = _BuildReplyToMenu(fReplyTo);
	menu = new BMenuField("replyTo", B_TRANSLATE("Reply account:"),
		fReplyToMenu);
	add_menu_to_layout(menu, mailLayout, layoutRow);

	// Mail Contents

	fReplyPreamble = new BTextControl("replytext",
		B_TRANSLATE("Reply preamble:"),
		*preamble, new BMessage(P_REPLY_PREAMBLE));
	fReplyPreamble->SetAlignment(B_ALIGN_RIGHT, B_ALIGN_LEFT);

	fReplyPreambleMenu = _BuildReplyPreambleMenu();
	menu = new BMenuField("replyPreamble", NULL, fReplyPreambleMenu);
	menu->SetExplicitMaxSize(BSize(menu->MinSize().width, B_SIZE_UNSET));

	mailLayout->AddItem(fReplyPreamble->CreateLabelLayoutItem(), 0, layoutRow);
	mailLayout->AddItem(fReplyPreamble->CreateTextViewLayoutItem(), 1,
		layoutRow);
	mailLayout->AddView(menu, 2, layoutRow);
	layoutRow++;

	fSignatureMenu = _BuildSignatureMenu(*sig);
	menu = new BMenuField("sig", B_TRANSLATE("Auto signature:"),
		fSignatureMenu);
	add_menu_to_layout(menu, mailLayout, layoutRow);

	fEncodingMenu = _BuildEncodingMenu(fEncoding);
	menu = new BMenuField("enc", B_TRANSLATE("Outgoing encoding:"), fEncodingMenu);
	add_menu_to_layout(menu, mailLayout, layoutRow);

	fWarnUnencodableMenu = _BuildWarnUnencodableMenu(fWarnUnencodable);
	menu = new BMenuField("warnUnencodable", B_TRANSLATE("Warn unencodable:"),
		fWarnUnencodableMenu);
	add_menu_to_layout(menu, mailLayout, layoutRow);

	fWrapMenu = _BuildWrapMenu(*wrap);
	menu = new BMenuField("wrap", B_TRANSLATE("Text wrapping:"), fWrapMenu);
	add_menu_to_layout(menu, mailLayout, layoutRow);

	fAttachAttributesMenu = _BuildAttachAttributesMenu(*attachAttributes);
	menu = new BMenuField("attachAttributes", B_TRANSLATE("Attach attributes:"),
		fAttachAttributesMenu);
	add_menu_to_layout(menu, mailLayout, layoutRow);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.SetInsets(0, B_USE_DEFAULT_SPACING, 0, B_USE_DEFAULT_SPACING)
		.Add(tabView)
		.AddGroup(B_HORIZONTAL)
			.Add(fRevert)
			.AddGlue()
			.Add(cancelButton)
			.Add(okButton)
			.SetInsets(B_USE_WINDOW_SPACING, B_USE_DEFAULT_SPACING,
				B_USE_WINDOW_SPACING, 0)
		.End();

}


TPrefsWindow::~TPrefsWindow()
{
	BMessage msg(WINDOW_CLOSED);
	msg.AddInt32("kind", PREFS_WINDOW);
	msg.AddPoint("window pos", Frame().LeftTop());
	be_app->PostMessage(&msg);
}


void
TPrefsWindow::DispatchMessage(BMessage* message, BHandler* handler)
{
	if (message->what == B_KEY_DOWN) {
		const char* bytes;
		if (message->FindString("bytes", &bytes) == B_OK
			&& bytes[0] == B_ESCAPE) {
			PostMessage(P_CANCEL);
			return;
		}
	}
	BWindow::DispatchMessage(message, handler);
}


void
TPrefsWindow::MessageReceived(BMessage* msg)
{
	bool revert = true;
	const char* family;
	const char* signature;
	const char* style;
	char label[256];
	int32 new_size;
	int32 old_size;
	font_family new_family;
	font_family old_family;
	font_style new_style;
	font_style old_style;
	BMenuItem* item;
	BMessage message;

	switch (msg->what) {
		case P_OK:
			if (strcmp(fReplyPreamble->Text(), *fNewPreamble)) {
				free(*fNewPreamble);
				*fNewPreamble
					= (char *)malloc(strlen(fReplyPreamble->Text()) + 1);
				strcpy(*fNewPreamble, fReplyPreamble->Text());
			}
			_SaveBlocklist();
			be_app->PostMessage(PREFS_CHANGED);
			Quit();
			break;

		case P_CANCEL:
			revert = false;
			// supposed to fall through
		case P_REVERT:
			fFont.GetFamilyAndStyle(&old_family, &old_style);
			fNewFont->GetFamilyAndStyle(&new_family, &new_style);
			old_size = (int32)fFont.Size();
			new_size = (int32)fNewFont->Size();
			if (strcmp(old_family, new_family) || strcmp(old_style, new_style)
				|| old_size != new_size) {
				fNewFont->SetFamilyAndStyle(old_family, old_style);
				if (revert) {
					sprintf(label, "%s %s", old_family, old_style);
					item = fFontMenu->FindItem(label);
					if (item != NULL)
						item->SetMarked(true);
				}

				fNewFont->SetSize(old_size);
				if (revert)
					fSizeSpinner->SetValue(old_size);
				message.what = M_FONT;
				be_app->PostMessage(&message);
			}
			*fNewWrap = fWrap;
			*fNewAttachAttributes = fAttachAttributes;

			if (strcmp(fSignature, *fNewSignature)) {
				free(*fNewSignature);
				*fNewSignature = (char*)malloc(strlen(fSignature) + 1);
				strcpy(*fNewSignature, fSignature);
			}

			*fNewEncoding = fEncoding;
			*fNewWarnUnencodable = fWarnUnencodable;
			*fNewSpellCheckStartOn = fSpellCheckStartOn;
			*fNewAutoMarkRead = fAutoMarkRead;
			*fNewButtonBar = fButtonBar;
			*fNewShowTimeRange = fShowTimeRange;
			*fNewUseSystemFontSize = fUseSystemFontSize;
			if (revert) {
				fUseSystemFontSizeCheckBox->SetValue(
					fUseSystemFontSize ? B_CONTROL_ON : B_CONTROL_OFF);
				fSizeSpinner->SetEnabled(!fUseSystemFontSize);
			}

			be_app->PostMessage(PREFS_CHANGED);

			if (revert) {
				for (int i = fAccountMenu->CountItems(); --i > 0;) {
					BMenuItem *accountItem = fAccountMenu->ItemAt(i);
					BMessage* itemMessage = accountItem->Message();
					if (itemMessage != NULL
						&& itemMessage->FindInt32("id") == *(int32 *)&fAccount) {
						accountItem->SetMarked(true);
						break;
					}
				}

				strcpy(label,fReplyTo == ACCOUNT_USE_DEFAULT
					? B_TRANSLATE("Use default account")
					: B_TRANSLATE("Account from mail"));
				if ((item = fReplyToMenu->FindItem(label)) != NULL)
					item->SetMarked(true);

				strcpy(label, fWrap ? B_TRANSLATE("On") : B_TRANSLATE("Off"));
				if ((item = fWrapMenu->FindItem(label)) != NULL)
					item->SetMarked(true);

				strcpy(label, fAttachAttributes
					? B_TRANSLATE("Include file attributes in attachments")
					: B_TRANSLATE("No file attributes, just plain data"));
				if ((item = fAttachAttributesMenu->FindItem(label)) != NULL)
					item->SetMarked(true);

				strcpy(label, fColoredQuotes ? B_TRANSLATE("On") : B_TRANSLATE("Off"));
				if ((item = fColoredQuotesMenu->FindItem(label)) != NULL)
					item->SetMarked(true);

				if (strcmp(fReplyPreamble->Text(), *fNewPreamble))
					fReplyPreamble->SetText(*fNewPreamble);

				item = fSignatureMenu->FindItem(fSignature);
				if (item)
					item->SetMarked(true);

				uint32 index = 0;
				while ((item = fEncodingMenu->ItemAt(index++)) != NULL) {
					BMessage* itemMessage = item->Message();
					if (itemMessage == NULL)
						continue;

					int32 encoding;
					if (itemMessage->FindInt32("encoding", &encoding) == B_OK
						&& (uint32)encoding == *fNewEncoding) {
						item->SetMarked(true);
						break;
					}
				}

				strcpy(label, fWarnUnencodable ? B_TRANSLATE("On") : B_TRANSLATE("Off"));
				if ((item = fWarnUnencodableMenu->FindItem(label)) != NULL)
					item->SetMarked(true);

				strcpy(label, fSpellCheckStartOn ? B_TRANSLATE("On") : B_TRANSLATE("Off"));
				if ((item = fSpellCheckStartOnMenu->FindItem(label)) != NULL)
					item->SetMarked(true);
			} else
				Quit();
			break;

		case P_FONT:
			family = NULL;
			style = NULL;
			int32 family_menu_index;
			if (msg->FindString("font", &family) == B_OK) {
				msg->FindString("style", &style);
				fNewFont->SetFamilyAndStyle(family, style);
				message.what = M_FONT;
				be_app->PostMessage(&message);
			}

			/* grab this little tidbit so we can set the correct Family */
			if (msg->FindInt32("parent_index", &family_menu_index) == B_OK)
				fFontMenu->ItemAt(family_menu_index)->SetMarked(true);
			break;

		case P_SIZE:
			fNewFont->SetSize(fSizeSpinner->Value());
			message.what = M_FONT;
			be_app->PostMessage(&message);
			break;

		case P_WRAP:
			msg->FindBool("wrap", fNewWrap);
			break;
		case P_ATTACH_ATTRIBUTES:
			msg->FindBool("attachAttributes", fNewAttachAttributes);
			break;
		case P_COLORED_QUOTES:
			msg->FindBool("cquotes", fNewColoredQuotes);
			break;
		case P_ACCOUNT:
			msg->FindInt32("id",(int32*)fNewAccount);
			break;
		case P_REPLYTO:
			msg->FindInt32("replyTo", fNewReplyTo);
			break;
		case P_REPLY_PREAMBLE:
		{
			int32 index = -1;
			if (msg->FindInt32("index", &index) < B_OK)
				break;
			BMenuItem *preambleItem = fReplyPreambleMenu->ItemAt(index);
			if (preambleItem == NULL) {
				msg->PrintToStream();
				break;
			}

			BTextView *text = fReplyPreamble->TextView();
			int32 start;
			int32 end;
			text->GetSelection(&start, &end);

			// If nothing is explicitly selected (e.g. text control lost
			// focus when clicking the dropdown), append at the end instead
			// of replacing the entire text.
			if (start == 0 && end == text->TextLength()) {
				start = end;
			} else if (start != end) {
				text->Delete(start, end);
			}

			text->Insert(start, preambleItem->Label(), 2);
			text->Select(start + 2, start + 2);
			break;
		}
		case P_SIG:
			free(*fNewSignature);
			if (msg->FindString("signature", &signature) == B_NO_ERROR) {
				*fNewSignature = (char*)malloc(strlen(signature) + 1);
				strcpy(*fNewSignature, signature);
			} else {
				*fNewSignature = (char*)malloc(
					strlen(B_TRANSLATE("None")) + 1);
				strcpy(*fNewSignature, B_TRANSLATE("None"));
			}
			break;
		case P_ENC:
			msg->FindInt32("encoding", (int32*)fNewEncoding);
			break;
		case P_WARN_UNENCODABLE:
			msg->FindBool("warnUnencodable", fNewWarnUnencodable);
			break;
		case P_SPELL_CHECK_START_ON:
			msg->FindBool("spellCheckStartOn", fNewSpellCheckStartOn);
			break;
		case P_MARK_READ:
			msg->FindBool("autoMarkRead", fNewAutoMarkRead);
			be_app->PostMessage(PREFS_CHANGED);
			break;
		case P_BUTTON_BAR:
			msg->FindInt8("bar", (int8*)fNewButtonBar);
			be_app->PostMessage(PREFS_CHANGED);
			break;
		case P_SHOW_TIME_RANGE:
			msg->FindBool("showTimeRange", fNewShowTimeRange);
			be_app->PostMessage(PREFS_CHANGED);
			break;

		case P_USE_SYSTEM_FONT_SIZE:
		{
			bool use = (fUseSystemFontSizeCheckBox->Value() == B_CONTROL_ON);
			*fNewUseSystemFontSize = use;
			if (gReaderSettings != NULL)
				gReaderSettings->SetUseSystemFontSize(use);
			if (use) {
				int32 systemSize = (int32)be_fixed_font->Size();
				fSizeSpinner->SetValue(systemSize);
				fNewFont->SetSize(systemSize);
			}
			fSizeSpinner->SetEnabled(!use);
			{
				BMessage fontMsg(M_FONT);
				be_app->PostMessage(&fontMsg);
			}
			be_app->PostMessage(PREFS_CHANGED);
			break;
		}

		case P_BLOCK_ADD:
		{
			BString address(fBlockAddressField->Text());
			address.Trim();
			address.ToLower();
			if (address.Length() == 0)
				break;

			// Check for duplicates in backing data
			bool found = false;
			for (int32 i = 0; i < fBlocklistData.CountStrings(); i++) {
				if (fBlocklistData.StringAt(i).ICompare(address) == 0) {
					found = true;
					break;
				}
			}
			if (!found) {
				fBlocklistData.Add(address);
				fBlockAddressField->SetText("");
				_RefreshBlocklistView();
			}
			break;
		}

		case P_BLOCK_REMOVE:
		{
			int32 selected = fBlocklistView->CurrentSelection();
			if (selected >= 0) {
				BStringItem* item = dynamic_cast<BStringItem*>(
					fBlocklistView->ItemAt(selected));
				if (item != NULL) {
					// Remove from backing data
					BString text(item->Text());
					for (int32 i = 0; i < fBlocklistData.CountStrings(); i++) {
						if (fBlocklistData.StringAt(i).ICompare(text) == 0) {
							fBlocklistData.Remove(i);
							break;
						}
					}
				}
				_RefreshBlocklistView();
				fRemoveBlockButton->SetEnabled(
					fBlocklistView->CurrentSelection() >= 0);
			}
			break;
		}

		case P_BLOCK_SELECTION:
			fRemoveBlockButton->SetEnabled(
				fBlocklistView->CurrentSelection() >= 0);
			break;

		case P_BLOCK_FILTER:
			_RefreshBlocklistView();
			break;

		default:
			BWindow::MessageReceived(msg);
	}

	fFont.GetFamilyAndStyle(&old_family, &old_style);
	fNewFont->GetFamilyAndStyle(&new_family, &new_style);
	old_size = (int32)fFont.Size();
	new_size = fSizeSpinner->Value();
	bool changed = old_size != new_size
		|| fWrap != *fNewWrap
		|| fAttachAttributes != *fNewAttachAttributes
		|| fColoredQuotes != *fNewColoredQuotes
		|| fAccount != *fNewAccount
		|| fReplyTo != *fNewReplyTo
		|| strcmp(old_family, new_family)
		|| strcmp(old_style, new_style)
		|| strcmp(fReplyPreamble->Text(), *fNewPreamble)
		|| strcmp(fSignature, *fNewSignature)
		|| fEncoding != *fNewEncoding
		|| fWarnUnencodable != *fNewWarnUnencodable
		|| fSpellCheckStartOn != *fNewSpellCheckStartOn
		|| fAutoMarkRead != *fNewAutoMarkRead
		|| fButtonBar != *fNewButtonBar
		|| fShowTimeRange != *fNewShowTimeRange
		|| fUseSystemFontSize != *fNewUseSystemFontSize;
	fRevert->SetEnabled(changed);
}


BPopUpMenu*
TPrefsWindow::_BuildFontMenu(BFont* font)
{
	font_family	def_family;
	font_style	def_style;
	font_family	f_family;
	font_style	f_style;

	BPopUpMenu *menu = new BPopUpMenu("");
	font->GetFamilyAndStyle(&def_family, &def_style);

	int32 family_menu_index = 0;
	int family_count = count_font_families();
	for (int family_loop = 0; family_loop < family_count; family_loop++) {
		get_font_family(family_loop, &f_family);
		BMenu *family_menu = new BMenu(f_family);

		int style_count = count_font_styles(f_family);
		for (int style_loop = 0; style_loop < style_count; style_loop++) {
			get_font_style(f_family, style_loop, &f_style);

			BMessage *msg = new BMessage(P_FONT);
			msg->AddString("font", f_family);
			msg->AddString("style", f_style);
			// we send this to make setting the Family easier when things
			// change
			msg->AddInt32("parent_index", family_menu_index);

			BMenuItem *item = new BMenuItem(f_style, msg);
			family_menu->AddItem(item);
			if ((strcmp(def_family, f_family) == 0)
				&& (strcmp(def_style, f_style) == 0)) {
				item->SetMarked(true);
			}

			item->SetTarget(this);
		}

		menu->AddItem(family_menu);
		BMenuItem *item = menu->ItemAt(family_menu_index);
		BMessage *msg = new BMessage(P_FONT);
		msg->AddString("font", f_family);

		item->SetMessage(msg);
		item->SetTarget(this);
		if (strcmp(def_family, f_family) == 0)
			item->SetMarked(true);

		family_menu_index++;
	}
	return menu;
}


BPopUpMenu*
TPrefsWindow::_BuildLevelMenu(int32 level)
{
	BMenuItem* item;
	BMessage* msg;
	BPopUpMenu* menu;

	menu = new BPopUpMenu("");
	msg = new BMessage(P_LEVEL);
	msg->AddInt32("level", L_BEGINNER);
	menu->AddItem(item = new BMenuItem(B_TRANSLATE("Beginner"), msg));
	if (level == L_BEGINNER)
		item->SetMarked(true);

	msg = new BMessage(P_LEVEL);
	msg->AddInt32("level", L_EXPERT);
	menu->AddItem(item = new BMenuItem(B_TRANSLATE("Expert"), msg));
	if (level == L_EXPERT)
		item->SetMarked(true);

	return menu;
}


BPopUpMenu*
TPrefsWindow::_BuildAccountMenu(int32 account)
{
	BPopUpMenu* menu = new BPopUpMenu("");
	BMenuItem* item;

	//menu->SetRadioMode(true);
	BMailAccounts accounts;
	if (accounts.CountAccounts() == 0) {
		menu->AddItem(item = new BMenuItem(B_TRANSLATE("<no account found>"), NULL));
		item->SetEnabled(false);
		return menu;
	}

	BMessage* msg;
	for (int32 i = 0; i < accounts.CountAccounts(); i++) {
		BMailAccountSettings* settings = accounts.AccountAt(i);
		item = new BMenuItem(settings->Name(), msg = new BMessage(P_ACCOUNT));

		msg->AddInt32("id", settings->AccountID());

		if (account == settings->AccountID())
			item->SetMarked(true);

		menu->AddItem(item);
	}
	return menu;
}


BPopUpMenu*
TPrefsWindow::_BuildReplyToMenu(int32 account)
{
	BPopUpMenu* menu = new BPopUpMenu(B_EMPTY_STRING);

	BMenuItem* item;
	BMessage* msg;
	menu->AddItem(item = new BMenuItem(B_TRANSLATE("Use default account"),
		msg = new BMessage(P_REPLYTO)));
	msg->AddInt32("replyTo", ACCOUNT_USE_DEFAULT);
	if (account == ACCOUNT_USE_DEFAULT)
		item->SetMarked(true);

	menu->AddItem(item = new BMenuItem(B_TRANSLATE("Account from mail"),
		msg = new BMessage(P_REPLYTO)));
	msg->AddInt32("replyTo", ACCOUNT_FROM_MAIL);
	if (account == ACCOUNT_FROM_MAIL)
		item->SetMarked(true);

	return menu;
}


BMenu*
TPrefsWindow::_BuildReplyPreambleMenu()
{
	BMenu *menu = new BMenu(B_EMPTY_STRING);

	menu->AddItem(new BMenuItem(B_TRANSLATE("%n - Full name"),
		new BMessage(P_REPLY_PREAMBLE)));

	menu->AddItem(new BMenuItem(B_TRANSLATE("%e - Email address"),
		new BMessage(P_REPLY_PREAMBLE)));

	menu->AddItem(new BMenuItem(B_TRANSLATE("%d - Date"),
		new BMessage(P_REPLY_PREAMBLE)));

	menu->AddSeparatorItem();

	menu->AddItem(new BMenuItem(B_TRANSLATE("\\n - Line break"),
		new BMessage(P_REPLY_PREAMBLE)));

	return menu;
}


BPopUpMenu*
TPrefsWindow::_BuildSignatureMenu(char* sig)
{
	char name[B_FILE_NAME_LENGTH];
	BEntry entry;
	BFile file;
	BMenuItem* item;
	BMessage* msg;
	BQuery query;
	BVolume vol;
	BVolumeRoster volume;

	BPopUpMenu* menu = new BPopUpMenu("");

	msg = new BMessage(P_SIG);
	msg->AddString("signature", B_TRANSLATE("None"));
	menu->AddItem(item = new BMenuItem(B_TRANSLATE("None"), msg));
	if (!strcmp(sig, B_TRANSLATE("None")))
		item->SetMarked(true);
	menu->AddSeparatorItem();

	volume.GetBootVolume(&vol);
	query.SetVolume(&vol);
	query.SetPredicate("_signature = *");
	query.Fetch();

	while (query.GetNextEntry(&entry) == B_NO_ERROR) {
		file.SetTo(&entry, O_RDONLY);
		if (file.InitCheck() == B_NO_ERROR) {
			msg = new BMessage(P_SIG);
			file.ReadAttr("_signature", B_STRING_TYPE, 0, name, sizeof(name));
			msg->AddString("signature", name);
			menu->AddItem(item = new BMenuItem(name, msg));
			if (!strcmp(sig, name))
				item->SetMarked(true);
		}
	}
	return menu;
}


BPopUpMenu*
TPrefsWindow::_BuildBoolMenu(uint32 what, const char* boolItem, bool isTrue)
{
	BMenuItem* item;
	BMessage* msg;
	BPopUpMenu* menu;

	menu = new BPopUpMenu("");
	msg = new BMessage(what);
	msg->AddBool(boolItem, true);
	menu->AddItem(item = new BMenuItem(B_TRANSLATE("On"), msg));
	if (isTrue)
		item->SetMarked(true);

	msg = new BMessage(what);
	msg->AddInt32(boolItem, false);
	menu->AddItem(item = new BMenuItem(B_TRANSLATE("Off"), msg));
	if (!isTrue)
		item->SetMarked(true);

	return menu;
}


BPopUpMenu*
TPrefsWindow::_BuildWrapMenu(bool wrap)
{
	return _BuildBoolMenu(P_WRAP, "wrap", wrap);
}


BPopUpMenu*
TPrefsWindow::_BuildAttachAttributesMenu(bool attachAttributes)
{
	BMenuItem* item;
	BMessage* msg;
	BPopUpMenu* menu;

	menu = new BPopUpMenu("");
	msg = new BMessage(P_ATTACH_ATTRIBUTES);
	msg->AddBool("attachAttributes", true);
	menu->AddItem(item = new BMenuItem(
		B_TRANSLATE("Include file attributes in attachments"), msg));
	if (attachAttributes)
		item->SetMarked(true);

	msg = new BMessage(P_ATTACH_ATTRIBUTES);
	msg->AddInt32("attachAttributes", false);
	menu->AddItem(item = new BMenuItem(
		B_TRANSLATE("No file attributes, just plain data"), msg));
	if (!attachAttributes)
		item->SetMarked(true);

	return menu;
}


BPopUpMenu*
TPrefsWindow::_BuildColoredQuotesMenu(bool quote)
{
	return _BuildBoolMenu(P_COLORED_QUOTES, "cquotes", quote);
}


BPopUpMenu*
TPrefsWindow::_BuildEncodingMenu(uint32 encoding)
{
	BMenuItem* item;
	BMessage* msg;
	BPopUpMenu* menu;

	menu = new BPopUpMenu("");

	BCharacterSetRoster roster;
	BCharacterSet charset;
	while (roster.GetNextCharacterSet(&charset) == B_NO_ERROR) {
		BString name(charset.GetPrintName());
		const char* mime = charset.GetMIMEName();
		if (mime)
			name << " (" << mime << ")";
		msg = new BMessage(P_ENC);
		uint32 convert_id;
		if ((mime == 0) || (strcasecmp(mime, "UTF-8") != 0))
			convert_id = charset.GetConversionID();
		else
			convert_id = B_MAIL_UTF8_CONVERSION;
		msg->AddInt32("encoding", convert_id);
		menu->AddItem(item = new BMenuItem(name.String(), msg));
		if (convert_id == encoding)
			item->SetMarked(true);
	}

	msg = new BMessage(P_ENC);
	msg->AddInt32("encoding", B_MAIL_US_ASCII_CONVERSION);
	menu->AddItem(item = new BMenuItem("US-ASCII", msg));
	if (encoding == B_MAIL_US_ASCII_CONVERSION)
		item->SetMarked(true);

	return menu;
}


BPopUpMenu*
TPrefsWindow::_BuildWarnUnencodableMenu(bool warnUnencodable)
{
	return _BuildBoolMenu(P_WARN_UNENCODABLE, "warnUnencodable",
		warnUnencodable);
}


BPopUpMenu*
TPrefsWindow::_BuildSpellCheckStartOnMenu(bool spellCheckStartOn)
{
	return _BuildBoolMenu(P_SPELL_CHECK_START_ON, "spellCheckStartOn",
		spellCheckStartOn);
}


BPopUpMenu*
TPrefsWindow::_BuildAutoMarkReadMenu(bool autoMarkRead)
{
	return _BuildBoolMenu(P_MARK_READ, "autoMarkRead",
		autoMarkRead);
}


BPopUpMenu*
TPrefsWindow::_BuildButtonBarMenu(uint8 show)
{
	BMenuItem* item;
	BMessage* msg;
	BPopUpMenu* menu = new BPopUpMenu("");

	msg = new BMessage(P_BUTTON_BAR);
	msg->AddInt8("bar", kShowToolBar);
	menu->AddItem(item = new BMenuItem(
		B_TRANSLATE("Show icons & labels"), msg));
	if (show == kShowToolBar || show == kHideToolBar)
		item->SetMarked(true);  // Default to icons & labels if was hidden

	msg = new BMessage(P_BUTTON_BAR);
	msg->AddInt8("bar", kShowToolBarIconsOnly);
	menu->AddItem(item = new BMenuItem(B_TRANSLATE("Show icons only"), msg));
	if (show == kShowToolBarIconsOnly)
		item->SetMarked(true);

	return menu;
}


BPopUpMenu*
TPrefsWindow::_BuildShowTimeRangeMenu(bool showTimeRange)
{
	return _BuildBoolMenu(P_SHOW_TIME_RANGE, "showTimeRange", showTimeRange);
}


void
TPrefsWindow::_RefreshBlocklistView()
{
	// Clear list view
	while (fBlocklistView->CountItems() > 0)
		delete fBlocklistView->RemoveItem(int32(0));

	// Get filter text
	BString filter(fBlockFilterField->Text());
	filter.Trim();
	filter.ToLower();

	// Repopulate with matching entries from backing data
	for (int32 i = 0; i < fBlocklistData.CountStrings(); i++) {
		BString entry = fBlocklistData.StringAt(i);
		if (filter.Length() == 0) {
			fBlocklistView->AddItem(new BStringItem(entry.String()));
		} else {
			BString lower(entry);
			lower.ToLower();
			if (lower.FindFirst(filter) >= 0)
				fBlocklistView->AddItem(new BStringItem(entry.String()));
		}
	}

	fRemoveBlockButton->SetEnabled(
		fBlocklistView->CurrentSelection() >= 0);
}


void
TPrefsWindow::_LoadBlocklist()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;
	path.Append("EmailViews/blocked_senders");

	FILE* f = fopen(path.Path(), "r");
	if (f == NULL)
		return;

	char line[512];
	while (fgets(line, sizeof(line), f) != NULL) {
		BString entry(line);
		entry.Trim();
		if (entry.Length() > 0 && entry.ByteAt(0) != '#') {
			fBlocklistData.Add(entry);
			fBlocklistView->AddItem(new BStringItem(entry.String()));
			fOriginalBlocklist.Add(entry);
		}
	}
	fclose(f);
}


void
TPrefsWindow::_SaveBlocklist()
{
	// Find which entries were removed
	_UnclassifyRemovedSenders();

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;
	path.Append("EmailViews");

	// Ensure directory exists
	create_directory(path.Path(), 0755);

	path.Append("blocked_senders");

	FILE* f = fopen(path.Path(), "w");
	if (f == NULL)
		return;

	for (int32 i = 0; i < fBlocklistData.CountStrings(); i++) {
		fprintf(f, "%s\n", fBlocklistData.StringAt(i).String());
	}
	fclose(f);
}


void
TPrefsWindow::_UnclassifyRemovedSenders()
{
	// Build set of current entries for fast lookup
	BStringList currentList;
	for (int32 i = 0; i < fBlocklistData.CountStrings(); i++) {
		BString entry = fBlocklistData.StringAt(i);
		entry.ToLower();
		currentList.Add(entry);
	}

	// Find entries that were in the original but are no longer present
	BStringList removed;
	for (int32 i = 0; i < fOriginalBlocklist.CountStrings(); i++) {
		BString entry = fOriginalBlocklist.StringAt(i);
		entry.ToLower();
		if (!currentList.HasString(entry))
			removed.Add(entry);
	}

	if (removed.IsEmpty())
		return;

	// Query all spam-classified emails and clear classification
	// for those matching removed senders
	BVolumeRoster volumeRoster;
	BVolume volume;
	volumeRoster.GetBootVolume(&volume);

	BQuery query;
	query.SetVolume(&volume);
	query.SetPredicate("(BEOS:TYPE==\"text/x-email\")"
		"&&(MAIL:classification==Spam)");

	if (query.Fetch() != B_OK)
		return;

	// Collect matching refs first to avoid index update race
	BList matchingRefs;
	entry_ref ref;
	while (query.GetNextRef(&ref) == B_OK) {
		BNode node(&ref);
		if (node.InitCheck() != B_OK)
			continue;

		char fromBuf[256] = "";
		node.ReadAttr("MAIL:from", B_STRING_TYPE, 0,
			fromBuf, sizeof(fromBuf) - 1);
		if (fromBuf[0] == '\0')
			continue;

		// Extract bare email address
		BString fromStr(fromBuf);
		BString addr;
		int32 open = fromStr.FindFirst('<');
		int32 close = fromStr.FindFirst('>', open);
		if (open >= 0 && close > open)
			fromStr.CopyInto(addr, open + 1, close - open - 1);
		else
			addr = fromStr;
		addr.Trim();
		addr.ToLower();

		// Check if this sender matches any removed entry
		bool matches = false;
		for (int32 i = 0; i < removed.CountStrings(); i++) {
			BString entry = removed.StringAt(i);
			if (entry == addr) {
				matches = true;
				break;
			}
			// Check @domain match
			int32 at = addr.FindFirst('@');
			if (at >= 0) {
				BString domain;
				addr.CopyInto(domain, at, addr.Length() - at);
				if (entry == domain) {
					matches = true;
					break;
				}
			}
		}

		if (matches)
			matchingRefs.AddItem(new entry_ref(ref));
	}

	// Now clear attributes on collected refs
	for (int32 i = 0; i < matchingRefs.CountItems(); i++) {
		entry_ref* matchRef = (entry_ref*)matchingRefs.ItemAt(i);
		BNode node(matchRef);
		if (node.InitCheck() == B_OK)
			node.RemoveAttr("MAIL:classification");
		delete matchRef;
	}
}

