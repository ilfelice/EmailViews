/*
 * Copyright 1991-2001, Be Incorporated. All rights reserved.
 * Copyright 2025-2026, EmailViews contributors
 * Distributed under the terms of the MIT License.
 *
 * ReaderSettings - Email reader/composer settings and preferences
 * Adapted from BeGroups Reader's TReaderSettings
 */


#include "ReaderSettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <Catalog.h>
#include <CharacterSet.h>
#include <CharacterSetRoster.h>
#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Locale.h>
#include <Path.h>
#include <Screen.h>

#include <MailSettings.h>

#include "EmailReaderWindow.h"
#include "FindWindow.h"
#include "Messages.h"
#include "Prefs.h"
#include "Signature.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ReaderSettings"


using namespace BPrivate;


// Forward declarations
static bool IsOnSameVolume(const node_ref& a, const node_ref& b);
static bool IsInSameDirectory(const node_ref& a, const node_ref& b);


// Global accessor for reader settings
TReaderSettings* gReaderSettings = NULL;


// Default window dimensions (Haiku Mail base + 20%, width adjusted)
static const float WIND_WIDTH = 750;
static const float WIND_HEIGHT = 600;


TReaderSettings::TReaderSettings()
	:
	fWindowListLock("reader window list"),
	fPrefsWindow(NULL),
	fSigWindow(NULL),
	fPrintSettings(NULL),
	fWrapMode(true),
	fAttachAttributes(true),
	fColoredQuotes(true),
	fShowToolBar(1),
	fWarnAboutUnencodableCharacters(true),
	fStartWithSpellCheckOn(false),
	fAutoMarkRead(true),
	fShowTimeRange(true),
	fShowSpamGUI(true),
	fUseSystemFontSize(true),
	fShowAllEmails(true),
	fShowStarredEmails(true),
	fShowWithAttachments(true),
	fShowSpamView(true),
	fDefaultAccount(-1),
	fUseAccountFrom(ACCOUNT_USE_DEFAULT),
	fMailCharacterSet(B_MAIL_UTF8_CONVERSION),
	fContentFont(be_fixed_font),
	fPeople(fPeopleQueryList),
	fPeopleGroups(fPeopleQueryList)
{
	// Set default values
	fSignature = strdup(B_TRANSLATE("None"));
	fReplyPreamble = strdup(B_TRANSLATE("\\n\\nOn %d %n wrote:\\n"));

	fMailWindowFrame.Set(0, 0, 0, 0);
	fWindowCount = 0;

	// Get default character encoding
	const BCharacterSet* defaultEncoding
		= BCharacterSetRoster::FindCharacterSetByName("UTF-8");
	if (defaultEncoding != NULL)
		fMailCharacterSet = defaultEncoding->GetConversionID();

	fContentFont.SetSpacing(B_BITMAP_SPACING);
}


TReaderSettings::~TReaderSettings()
{
	_SaveSettings();

	delete fPrintSettings;
	free(fSignature);
	free(fReplyPreamble);
}


void
TReaderSettings::Init()
{
	_LoadSettings();
	fLastMailWindowFrame = fMailWindowFrame;

	// Check for spam filter existence to enable/disable spam GUI
	_CheckForSpamFilterExistence();
	
	// Start people query for address auto-completion
	// Uses META:email=** to find People files with email addresses
	fPeopleQueryList.Init("META:email=**");
}


EmailReaderWindow*
TReaderSettings::FindWindow(const entry_ref& ref)
{
	BAutolock locker(fWindowListLock);

	// Get node_ref for the requested file
	BEntry entry(&ref);
	if (entry.InitCheck() != B_OK)
		return NULL;
	
	node_ref targetNodeRef;
	if (entry.GetNodeRef(&targetNodeRef) != B_OK)
		return NULL;

	for (int32 i = 0; i < fWindowList.CountItems(); i++) {
		EmailReaderWindow* window = (EmailReaderWindow*)fWindowList.ItemAt(i);
		if (window == NULL)
			continue;

		node_ref windowNodeRef;
		if (window->GetMailNodeRef(windowNodeRef) == B_OK) {
			if (windowNodeRef.device == targetNodeRef.device
				&& windowNodeRef.node == targetNodeRef.node) {
				return window;
			}
		}
	}
	return NULL;
}


void
TReaderSettings::AddWindow(EmailReaderWindow* window)
{
	BAutolock locker(fWindowListLock);
	fWindowList.AddItem(window);
}


void
TReaderSettings::RemoveWindow(EmailReaderWindow* window)
{
	BAutolock locker(fWindowListLock);
	fWindowList.RemoveItem(window);
}


int32
TReaderSettings::CountWindows() const
{
	return fWindowList.CountItems();
}


EmailReaderWindow*
TReaderSettings::WindowAt(int32 index) const
{
	return (EmailReaderWindow*)fWindowList.ItemAt(index);
}


void
TReaderSettings::SetPrintSettings(const BMessage* settings)
{
	if (fPrintSettings != NULL)
		delete fPrintSettings;

	if (settings != NULL)
		fPrintSettings = new BMessage(*settings);
	else
		fPrintSettings = NULL;
}


bool
TReaderSettings::HasPrintSettings()
{
	return fPrintSettings != NULL;
}


BMessage
TReaderSettings::PrintSettings()
{
	if (fPrintSettings != NULL)
		return *fPrintSettings;

	return BMessage();
}


void
TReaderSettings::SetLastWindowFrame(BRect frame)
{
	fLastMailWindowFrame = frame;
}


BRect
TReaderSettings::NewWindowFrame()
{
	float fontFactor = be_plain_font->Size() / 12.0f;
	BRect frame;
	
	if (fMailWindowFrame.Width() < 64 || fMailWindowFrame.Height() < 20) {
		// Default size scaled by font factor
		float width = fontFactor * WIND_WIDTH;
		float height = fontFactor * WIND_HEIGHT;
		
		// Center on screen
		BRect screenFrame = BScreen().Frame();
		float left = (screenFrame.Width() - width) / 2;
		float top = (screenFrame.Height() - height) / 2;
		frame.Set(left, top, left + width, top + height);
	} else {
		frame = fMailWindowFrame;
	}

	// Cascade windows - offset by 15 pixels per window (wraps after 10)
	// Skip offset for first window (fWindowCount == 0) to keep it centered
	if (fWindowCount > 0) {
		frame.OffsetBy(fontFactor * (((fWindowCount + 5) % 10) * 15 - 75),
			fontFactor * (((fWindowCount + 5) % 10) * 15 - 75));
	}

	fWindowCount++;

	return frame;
}


BString
TReaderSettings::Signature()
{
	BAutolock locker(fWindowListLock);
	return BString(fSignature);
}


BString
TReaderSettings::ReplyPreamble()
{
	BAutolock locker(fWindowListLock);
	return BString(fReplyPreamble);
}


bool
TReaderSettings::WrapMode()
{
	BAutolock locker(fWindowListLock);
	return fWrapMode;
}


bool
TReaderSettings::AttachAttributes()
{
	BAutolock locker(fWindowListLock);
	return fAttachAttributes;
}


bool
TReaderSettings::ColoredQuotes()
{
	BAutolock locker(fWindowListLock);
	return fColoredQuotes;
}


uint8
TReaderSettings::ShowToolBar()
{
	BAutolock locker(fWindowListLock);
	return fShowToolBar;
}


bool
TReaderSettings::WarnAboutUnencodableCharacters()
{
	BAutolock locker(fWindowListLock);
	return fWarnAboutUnencodableCharacters;
}


bool
TReaderSettings::StartWithSpellCheckOn()
{
	BAutolock locker(fWindowListLock);
	return fStartWithSpellCheckOn;
}


bool
TReaderSettings::AutoMarkRead()
{
	BAutolock locker(fWindowListLock);
	return fAutoMarkRead;
}


bool
TReaderSettings::ShowTimeRange()
{
	BAutolock locker(fWindowListLock);
	return fShowTimeRange;
}


void
TReaderSettings::SetShowTimeRange(bool show)
{
	BAutolock locker(fWindowListLock);
	fShowTimeRange = show;
	_SaveSettings();
}


bool
TReaderSettings::UseSystemFontSize()
{
	BAutolock locker(fWindowListLock);
	return fUseSystemFontSize;
}


void
TReaderSettings::SetUseSystemFontSize(bool use)
{
	BAutolock locker(fWindowListLock);
	fUseSystemFontSize = use;
	if (use)
		fContentFont.SetSize(be_fixed_font->Size());
}


void
TReaderSettings::SetDefaultAccount(int32 account)
{
	BAutolock locker(fWindowListLock);
	fDefaultAccount = account;
}


int32
TReaderSettings::DefaultAccount()
{
	BAutolock locker(fWindowListLock);
	return fDefaultAccount;
}


int32
TReaderSettings::UseAccountFrom()
{
	BAutolock locker(fWindowListLock);
	return fUseAccountFrom;
}


uint32
TReaderSettings::MailCharacterSet()
{
	BAutolock locker(fWindowListLock);
	return fMailCharacterSet;
}


const BFont&
TReaderSettings::ContentFont()
{
	BAutolock locker(fWindowListLock);
	if (fUseSystemFontSize)
		fContentFont.SetSize(be_fixed_font->Size());
	return fContentFont;
}


void
TReaderSettings::ShowPrefsWindow()
{
	if (fPrefsWindow != NULL) {
		fPrefsWindow->Activate(true);
	} else {
		fPrefsWindow = new TPrefsWindow(fPrefsWindowPos,
			&fContentFont, NULL, &fWrapMode, &fAttachAttributes,
			&fColoredQuotes, &fDefaultAccount, &fUseAccountFrom,
			&fReplyPreamble, &fSignature, &fMailCharacterSet,
			&fWarnAboutUnencodableCharacters, &fStartWithSpellCheckOn,
			&fAutoMarkRead, &fShowToolBar, &fShowTimeRange,
			&fUseSystemFontSize,
			&fShowAllEmails, &fShowStarredEmails,
			&fShowWithAttachments, &fShowSpamView);
		fPrefsWindow->SetFeel(B_MODAL_APP_WINDOW_FEEL);
		
		if (fPrefsWindowPos.x <= 0 || fPrefsWindowPos.y <= 0) {
			fPrefsWindow->CenterOnScreen();
		}
		fPrefsWindow->MoveOnScreen();
		fPrefsWindow->Show();
	}
}


void
TReaderSettings::ShowSignatureWindow()
{
	if (fSigWindow != NULL) {
		fSigWindow->Activate(true);
	} else {
		fSigWindow = new TSignatureWindow(fSignatureWindowFrame);
		if (!fSignatureWindowFrame.IsValid()) {
			fSigWindow->CenterOnScreen();
		}
		fSigWindow->MoveOnScreen();
		fSigWindow->Show();
	}
}


void
TReaderSettings::FontChange()
{
	BAutolock locker(fWindowListLock);

	BMessage msg(CHANGE_FONT);
	msg.AddPointer("font", &fContentFont);

	for (int32 i = 0; i < fWindowList.CountItems(); i++) {
		BWindow* window = (BWindow*)fWindowList.ItemAt(i);
		if (window != NULL)
			window->PostMessage(&msg);
	}
}


status_t
TReaderSettings::_GetSettingsPath(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;

	path.Append("EmailViews");
	return create_directory(path.Path(), 0755);
}


status_t
TReaderSettings::_SaveSettings()
{
	BMailSettings accountSettings;

	if (fDefaultAccount != ~0L) {
		accountSettings.SetDefaultOutboundAccount(fDefaultAccount);
		accountSettings.Save();
	}

	BPath path;
	status_t status = _GetSettingsPath(path);
	if (status != B_OK)
		return status;

	path.Append("reader_settings~");

	BFile file;
	status = file.SetTo(path.Path(), B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
	if (status != B_OK)
		return status;

	BMessage settings('BeMl');
	settings.AddRect("MailWindowSize", fMailWindowFrame);

	font_family fontFamily;
	font_style fontStyle;
	fContentFont.GetFamilyAndStyle(&fontFamily, &fontStyle);

	settings.AddString("FontFamily", fontFamily);
	settings.AddString("FontStyle", fontStyle);
	settings.AddFloat("FontSize", fContentFont.Size());

	settings.AddRect("SignatureWindowSize", fSignatureWindowFrame);
	settings.AddBool("WordWrapMode", fWrapMode);
	settings.AddPoint("PreferencesWindowLocation", fPrefsWindowPos);
	settings.AddString("SignatureText", fSignature);
	settings.AddInt32("CharacterSet", fMailCharacterSet);
	settings.AddString("FindString", FindWindow::GetFindString());
	settings.AddInt8("ShowButtonBar", fShowToolBar);
	settings.AddInt32("UseAccountFrom", fUseAccountFrom);
	settings.AddBool("ColoredQuotes", fColoredQuotes);
	// Only save the preamble if the user customized it. If it matches the
	// translated default, omit it so language changes and future default
	// updates take effect automatically.
	BString defaultPreamble(B_TRANSLATE("\\n\\nOn %d %n wrote:\\n"));
	if (defaultPreamble != fReplyPreamble)
		settings.AddString("ReplyPreamble", fReplyPreamble);
	settings.AddBool("AttachAttributes", fAttachAttributes);
	settings.AddBool("WarnAboutUnencodableCharacters", fWarnAboutUnencodableCharacters);
	settings.AddBool("StartWithSpellCheck", fStartWithSpellCheckOn);
	settings.AddBool("ShowTimeRange", fShowTimeRange);
	settings.AddBool("UseSystemFontSize", fUseSystemFontSize);
	settings.AddBool("ShowAllEmails", fShowAllEmails);
	settings.AddBool("ShowStarredEmails", fShowStarredEmails);
	settings.AddBool("ShowWithAttachments", fShowWithAttachments);
	settings.AddBool("ShowSpamView", fShowSpamView);

	BEntry entry;
	status = entry.SetTo(path.Path());
	if (status != B_OK)
		return status;

	status = settings.Flatten(&file);
	if (status == B_OK) {
		status = entry.Rename("reader_settings", true);
	} else {
		entry.Remove();
	}

	return status;
}


status_t
TReaderSettings::_LoadSettings()
{
	BMailSettings accountSettings;
	fDefaultAccount = accountSettings.DefaultOutboundAccount();

	BPath path;
	status_t status = _GetSettingsPath(path);
	if (status != B_OK)
		return status;

	path.Append("reader_settings");

	BFile file;
	status = file.SetTo(path.Path(), B_READ_ONLY);
	if (status != B_OK)
		return status;

	BMessage settings;
	status = settings.Unflatten(&file);
	if (status < B_OK || settings.what != 'BeMl')
		return status;

	BRect rect;
	if (settings.FindRect("MailWindowSize", &rect) == B_OK)
		fMailWindowFrame = rect;

	const char* fontFamily;
	if (settings.FindString("FontFamily", &fontFamily) == B_OK) {
		const char* fontStyle;
		if (settings.FindString("FontStyle", &fontStyle) == B_OK) {
			float size;
			if (settings.FindFloat("FontSize", &size) == B_OK) {
				if (size >= 7)
					fContentFont.SetSize(size);

				if (fontFamily[0] && fontStyle[0]) {
					fContentFont.SetFamilyAndStyle(
						fontFamily[0] ? fontFamily : NULL,
						fontStyle[0] ? fontStyle : NULL);
				}
			}
		}
	}

	if (settings.FindRect("SignatureWindowSize", &rect) == B_OK)
		fSignatureWindowFrame = rect;

	bool boolValue;
	if (settings.FindBool("WordWrapMode", &boolValue) == B_OK)
		fWrapMode = boolValue;

	BPoint point;
	if (settings.FindPoint("PreferencesWindowLocation", &point) == B_OK)
		fPrefsWindowPos = point;

	const char* string;
	if (settings.FindString("SignatureText", &string) == B_OK) {
		free(fSignature);
		fSignature = strdup(string);
	}

	int32 int32Value;
	if (settings.FindInt32("CharacterSet", &int32Value) == B_OK)
		fMailCharacterSet = int32Value;
	if (fMailCharacterSet != B_MAIL_UTF8_CONVERSION
		&& fMailCharacterSet != B_MAIL_US_ASCII_CONVERSION
		&& BCharacterSetRoster::GetCharacterSetByConversionID(fMailCharacterSet) == NULL)
		fMailCharacterSet = B_MS_WINDOWS_CONVERSION;

	if (settings.FindString("FindString", &string) == B_OK)
		FindWindow::SetFindString(string);

	int8 int8Value;
	if (settings.FindInt8("ShowButtonBar", &int8Value) == B_OK)
		fShowToolBar = int8Value;

	if (settings.FindInt32("UseAccountFrom", &int32Value) == B_OK)
		fUseAccountFrom = int32Value;
	if (fUseAccountFrom < ACCOUNT_USE_DEFAULT
		|| fUseAccountFrom > ACCOUNT_FROM_MAIL)
		fUseAccountFrom = ACCOUNT_USE_DEFAULT;

	if (settings.FindBool("ColoredQuotes", &boolValue) == B_OK)
		fColoredQuotes = boolValue;

	if (settings.FindString("ReplyPreamble", &string) == B_OK) {
		free(fReplyPreamble);
		fReplyPreamble = strdup(string);
	}

	if (settings.FindBool("AttachAttributes", &boolValue) == B_OK)
		fAttachAttributes = boolValue;

	if (settings.FindBool("WarnAboutUnencodableCharacters", &boolValue) == B_OK)
		fWarnAboutUnencodableCharacters = boolValue;

	if (settings.FindBool("StartWithSpellCheck", &boolValue) == B_OK)
		fStartWithSpellCheckOn = boolValue;

	if (settings.FindBool("ShowTimeRange", &boolValue) == B_OK)
		fShowTimeRange = boolValue;

	if (settings.FindBool("UseSystemFontSize", &boolValue) == B_OK)
		fUseSystemFontSize = boolValue;
	else
		fUseSystemFontSize = true;

	if (settings.FindBool("ShowAllEmails", &boolValue) == B_OK)
		fShowAllEmails = boolValue;
	if (settings.FindBool("ShowStarredEmails", &boolValue) == B_OK)
		fShowStarredEmails = boolValue;
	if (settings.FindBool("ShowWithAttachments", &boolValue) == B_OK)
		fShowWithAttachments = boolValue;
	if (settings.FindBool("ShowSpamView", &boolValue) == B_OK)
		fShowSpamView = boolValue;

	return B_OK;
}


void
TReaderSettings::_CheckForSpamFilterExistence()
{
	int32 addonNameIndex;
	const char* addonNamePntr;
	BDirectory inChainDir;
	BPath path;
	BEntry settingsEntry;
	BFile settingsFile;
	BMessage settingsMessage;

	fShowSpamGUI = false;

	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;
	path.Append("Mail/chains/inbound");
	if (inChainDir.SetTo(path.Path()) != B_OK)
		return;

	while (inChainDir.GetNextEntry(&settingsEntry, true) == B_OK) {
		if (!settingsEntry.IsFile())
			continue;
		if (settingsFile.SetTo(&settingsEntry, B_READ_ONLY) != B_OK)
			continue;
		if (settingsMessage.Unflatten(&settingsFile) != B_OK)
			continue;
		for (addonNameIndex = 0;
				B_OK == settingsMessage.FindString("filter_addons",
					addonNameIndex, &addonNamePntr);
				addonNameIndex++) {
			if (strstr(addonNamePntr, "Spam Filter") != NULL) {
				fShowSpamGUI = true;
				return;
			}
		}
	}
}


// Helper functions for node comparison
static bool
IsOnSameVolume(const node_ref& a, const node_ref& b)
{
	return a.device == b.device;
}


static bool
IsInSameDirectory(const node_ref& a, const node_ref& b)
{
	return a.node == b.node;
}
