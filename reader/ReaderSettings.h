/*
 * Copyright 1991-2001, Be Incorporated. All rights reserved.
 * Copyright 2025-2026, EmailViews contributors
 * Distributed under the terms of the MIT License.
 *
 * ReaderSettings - Email reader/composer settings and preferences
 * Adapted from BeGroups Reader's TReaderSettings (non-BApplication version)
 */
#ifndef _READER_SETTINGS_H
#define _READER_SETTINGS_H


#include <Entry.h>
#include <Font.h>
#include <List.h>
#include <Locker.h>
#include <Message.h>
#include <String.h>

#include "People.h"
#include "QueryList.h"


// Forward declarations
class BMessenger;
class EmailReaderWindow;
class TPrefsWindow;
class TSignatureWindow;
class TReaderSettings;


// Global accessor for reader settings (set by EmailViewsApp)
extern TReaderSettings* gReaderSettings;


// Note: ACCOUNT_USE_DEFAULT and ACCOUNT_FROM_MAIL are defined in Prefs.h


class TReaderSettings {
public:
								TReaderSettings();
	virtual						~TReaderSettings();

			void				Init();

			// Window management
			EmailReaderWindow*	FindWindow(const entry_ref& ref);
			void				AddWindow(EmailReaderWindow* window);
			void				RemoveWindow(EmailReaderWindow* window);
			int32				CountWindows() const;
			EmailReaderWindow*	WindowAt(int32 index) const;

			// Print settings
			void				SetPrintSettings(const BMessage* settings);
			bool				HasPrintSettings();
			BMessage			PrintSettings();

			// Window frame
			void				SetLastWindowFrame(BRect frame);
			BRect				NewWindowFrame();

			// Settings accessors
			BString				Signature();
			BString				ReplyPreamble();
			bool				WrapMode();
			bool				AttachAttributes();
			bool				ColoredQuotes();
			uint8				ShowToolBar();
			bool				WarnAboutUnencodableCharacters();
			bool				StartWithSpellCheckOn();
			bool				AutoMarkRead();
			bool				ShowTimeRange();
			void				SetShowTimeRange(bool show);
			bool				ShowSpamGUI() const { return fShowSpamGUI; }
			bool				UseSystemFontSize();
			void				SetUseSystemFontSize(bool use);

			// Optional sidebar view visibility
			bool				ShowAllEmails() const { return fShowAllEmails; }
			bool				ShowStarredEmails() const { return fShowStarredEmails; }
			bool				ShowWithAttachments() const { return fShowWithAttachments; }
			bool				ShowSpamView() const { return fShowSpamView; }
			void				SetDefaultAccount(int32 account);
			int32				DefaultAccount();
			int32				UseAccountFrom();
			uint32				MailCharacterSet();
			const BFont&		ContentFont();

			// People/contacts
			QueryList&			PeopleQueryList()
									{ return fPeopleQueryList; }
			PersonList&			People()
									{ return fPeople; }
			GroupList&			PeopleGroups()
									{ return fPeopleGroups; }

			// Preferences window
			void				ShowPrefsWindow();
			void				ClearPrefsWindow() { fPrefsWindow = NULL; }

			// Signature window
			void				ShowSignatureWindow();
			void				ClearSignatureWindow() { fSigWindow = NULL; }

			// Font change notification
			void				FontChange();

private:
			status_t			_GetSettingsPath(BPath& path);
			status_t			_LoadSettings();
			status_t			_SaveSettings();
			void				_CheckForSpamFilterExistence();

			BList				fWindowList;
			BLocker				fWindowListLock;

			TPrefsWindow*		fPrefsWindow;
			TSignatureWindow*	fSigWindow;

			BRect				fMailWindowFrame;
			BRect				fLastMailWindowFrame;
			BRect				fSignatureWindowFrame;
			BPoint				fPrefsWindowPos;
			int32				fWindowCount;

			BMessage*			fPrintSettings;

			// Settings
			char*				fSignature;
			char*				fReplyPreamble;
			bool				fWrapMode;
			bool				fAttachAttributes;
			bool				fColoredQuotes;
			uint8				fShowToolBar;
			bool				fWarnAboutUnencodableCharacters;
			bool				fStartWithSpellCheckOn;
			bool				fAutoMarkRead;
			bool				fShowTimeRange;
			bool				fShowSpamGUI;
			bool				fUseSystemFontSize;
			bool				fShowAllEmails;
			bool				fShowStarredEmails;
			bool				fShowWithAttachments;
			bool				fShowSpamView;
			int32				fDefaultAccount;
			int32				fUseAccountFrom;
			uint32				fMailCharacterSet;
			BFont				fContentFont;

			QueryList			fPeopleQueryList;
			PersonList			fPeople;
			GroupList			fPeopleGroups;
};


#endif // _READER_SETTINGS_H
