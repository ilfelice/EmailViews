/*
 * EmailViews.h - Main window and application classes
 * Distributed under the terms of the MIT License.
 *
 * EmailViewsWindow is the three-pane main window:
 *   Left pane:   QueryListView (sidebar) with built-in and custom queries
 *   Upper right pane: EmailListView (email list) with column headers
 *   Lower right pane:  Preview panel (HTML or plain text) with attachment strip
 *
 * EmailViewsApp handles application lifecycle, Deskbar replicant, and
 * launches reader/compose windows via the EmailReaderWindow class.
 *
 * The query system works in two layers:
 *   1. Built-in queries (All Emails, Unread, etc.) are hard-coded predicates
 *   2. Custom queries are Haiku query files saved in ~/config/settings/EmailViews/queries/
 * Both are executed by EmailListView's background loader thread.
 *
 * A separate "background query" monitors for New/Seen emails to update
 * sidebar counts even when viewing a different query.
 */

#ifndef EMAILVIEWS_H
#define EMAILVIEWS_H

#include <BeBuild.h>
#include <Application.h>
#include <Catalog.h>
#include <Window.h>
#include <Handler.h>
#include <OutlineListView.h>
#include <StringView.h>
#include <TextView.h>
#include <TextControl.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <CardView.h>
#include <CardLayout.h>
#include <GroupLayout.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <Button.h>
#include <Bitmap.h>
#include <MenuField.h>
#include <PopUpMenu.h>
#include <vector>
#include <set>
#include <utility>
#include <MenuBar.h>
#include <Menu.h>
#include <MenuItem.h>
#include <Entry.h>
#include <Path.h>
#include <VolumeRoster.h>
#include <ControlLook.h>
#include <SeparatorView.h>
#include "ToolBarView.h"
#include <cstdio>

// Shared attachment strip view
#include "AttachmentStripView.h"

// About dialog
#include "AboutWindow.h"

// New high-performance email list view component
#include "EmailListView.h"

// Trash item view
#include "TrashItemView.h"

// Query list items
#include "QueryItem.h"

// Query list view
#include "QueryListView.h"

// Search bar view
#include "SearchBarView.h"

// Time range slider
#include "TimeRangeSlider.h"

#include <Directory.h>
#include <File.h>
#include <Node.h>
#include <NodeInfo.h>
#include <NodeMonitor.h>
#include <Roster.h>
#include <Query.h>
#include <String.h>
#include <ObjectList.h>
#include <Mime.h>

#include <map>

#include <mail/E-mail.h>
#include <mail/MailMessage.h>
#include <mail/MailAttachment.h>

// Forward declarations
class BFilePanel;

// Message constants
const uint32 MSG_QUERY_SELECTED = 'fldr';
const uint32 MSG_EMAIL_SELECTED = 'esel';
const uint32 MSG_EMAIL_INVOKED = 'einv';
const uint32 MSG_DELETE_EMAIL = 'edel';
const uint32 MSG_MARK_READ = 'mred';
const uint32 MSG_MARK_UNREAD = 'murd';
const uint32 MSG_MARK_SENT = 'msnt';
const uint32 MSG_FILTER_BY_SENDER = 'fbsn';
const uint32 MSG_FILTER_BY_RECIPIENT = 'fbrp';
const uint32 MSG_FILTER_BY_ACCOUNT = 'fbac';
const uint32 MSG_REPLY = 'rply';
const uint32 MSG_REPLY_ALL = 'rpla';
const uint32 MSG_FORWARD = 'frwd';
const uint32 MSG_SEND_AS_ATTACHMENT = 'saat';
const uint32 MSG_REMOVE_FILTER = 'rmfr';
const uint32 MSG_EDIT_QUERY = 'edqr';
const uint32 MSG_RELOAD_QUERIES = 'rlfr';
const uint32 MSG_RELOAD_QUERIES_DEBOUNCED = 'rldb';
const uint32 MSG_ABOUT = 'abut';
const uint32 MSG_CREATE_EMAIL = 'crem';
const uint32 MSG_NEW_EMAIL = 'newm';
const uint32 MSG_EMAIL_SETTINGS = 'emst';
const uint32 MSG_CHECK_EMAIL = 'chkm';
const uint32 MSG_QUIT = 'quit';
const uint32 MSG_OPEN_QUERIES_FOLDER = 'opqf';
const uint32 MSG_SEARCH_MODIFIED = 'srmd';
const uint32 MSG_SEARCH_CLEAR = 'srcl';
const uint32 MSG_SEARCH_ADD_QUERY = 'sraq';
const uint32 MSG_TOGGLE_DESKBAR = 'tgdb';
const uint32 MSG_SHOW_WINDOW = 'shwn';
const uint32 MSG_EMAIL_SENDER = 'emsd';
const uint32 MSG_CREATE_PERSON = 'crps';
const uint32 MSG_BACKUP_EMAILS = 'bkup';
const uint32 MSG_BACKUP_FINISHED = 'bkdn';
const uint32 MSG_TRASH_EMPTIED = 'trem';
const uint32 MSG_TRASH_BATCH = 'trbt';
const uint32 MSG_TRASH_LOAD_DONE = 'trld';
const uint32 MSG_UPDATE_QUERY_COUNTS = 'ufct';
const uint32 MSG_APPLY_TIME_RANGE_FILTER = 'atrf';
const uint32 MSG_INIT_PREVIEW_PANE = 'inpp';
const uint32 MSG_DEFERRED_INIT = 'dfin';
const uint32 MSG_RESTORE_EMAIL = 'rste';
const uint32 MSG_RESTORE_FOLDER_SELECTED = 'rsfs';
const uint32 MSG_RESTORE_SHOW_FOLDER_PANEL = 'rsfp';
const uint32 MSG_STAR_EMAIL = 'star';
const uint32 MSG_VIEW_HTML_MESSAGE = 'vhtm';
const uint32 MSG_BACKGROUND_QUERY_UPDATE = 'bgqu';
const uint32 MSG_VOLUME_SELECTED = 'vsel';
const uint32 MSG_TOGGLE_TIME_RANGE = 'tgtr';
const uint32 MSG_QUERY_COUNTS_READY = 'qcrd';
const uint32 MSG_NEXT_EMAIL = 'nxem';
const uint32 MSG_PREV_EMAIL = 'pvem';
const uint32 MSG_SELECT_ALL_EMAILS = 'sall';
const uint32 MSG_FOCUS_SEARCH = 'fsrc';
const uint32 MSG_UNDO_DELETE = 'undl';
const uint32 MSG_MARK_SPAM = 'mspm';
const uint32 MSG_UNMARK_SPAM = 'uspm';

// Tracker scripting constants (for Mail Next/Previous navigation)
const uint32 kNextSpecifier = 'snxt';
const uint32 kPreviousSpecifier = 'sprv';

// Forward declarations
class QueryItem;
class AttachmentStripView;
class EmailItem;
class EmailViewsWindow;
class TrashItemView;
class DeskbarReplicant;
class AboutWindow;
class BackgroundQueryHandler;

// Receives B_QUERY_UPDATE from the always-running "unread mail" query and
// forwards it to the main window as MSG_BACKGROUND_QUERY_UPDATE. This lets
// the sidebar update New/Unread counts even when the display query shows
// a different view (e.g., "All Emails" or "Starred").
class BackgroundQueryHandler : public BHandler {
public:
    explicit BackgroundQueryHandler(BLooper* target)
        : BHandler("BackgroundQueryHandler"),
          fTarget(target)
    {
    }
    
    virtual void MessageReceived(BMessage* message) override
    {
        if (message->what == B_QUERY_UPDATE) {
            // Forward as a different message type so the window knows
            // this is from the background query, not the display query
            BMessenger(fTarget).SendMessage(MSG_BACKGROUND_QUERY_UPDATE);
        }
    }
    
private:
    BLooper* fTarget;
};

// Message constants for exact match filtering from context menu
const uint32 MSG_FILTER_EXACT_SENDER = 'fxsn';
const uint32 MSG_FILTER_EXACT_RECIPIENT = 'fxrp';
const uint32 MSG_FILTER_EXACT_SUBJECT = 'fxsb';
const uint32 MSG_FILTER_EXACT_ACCOUNT = 'fxac';

class EmailViewsWindow : public BWindow {
public:
    EmailViewsWindow();
    virtual ~EmailViewsWindow();
    
    virtual void MessageReceived(BMessage* message);
    virtual bool QuitRequested();
    virtual BHandler* ResolveSpecifier(BMessage* message, int32 index,
        BMessage* specifier, int32 form, const char* property);
    
    // Select a built-in query view by name (for opening in specific view)
    bool SelectBuiltInQueryByName(const char* name);
    bool SelectBuiltInQueryByIndex(int32 index);
    
    // Navigation for EmailReaderWindow - returns true if found
    bool GetNextEmailRef(const entry_ref* current, entry_ref* next);
    bool GetPrevEmailRef(const entry_ref* current, entry_ref* prev);
    bool HasNextEmail(const entry_ref* current);
    bool HasPrevEmail(const entry_ref* current);
    
    // Select email in list (for reader to sync selection)
    void SelectEmailByRef(const entry_ref* ref);
    
    // Open email in reader window (public for RefsReceived)
    void OpenEmailInViewer(const char* emailPath);
    
private:
    void LoadQueries();
    void ScheduleQueryCountUpdate();
    void UpdateQueryCounts();
    void AddCustomQuery(const char* name, const char* query, const char* menuSelection, const char* searchText, bool selectIt = true);
    void RemoveCustomQuery(QueryItem* item);
    void _RemoveQueryItemByName(const char* leafName);
    void DeleteEmail(const char* emailPath);
    
    static status_t _InitBackgroundQueriesThread(void* data);

    void MarkEmailAsRead(const char* emailPath, bool read);
    void ComposeResponse(int32 opCode);
    void SaveWindowState();
    void LoadWindowState();
    
    void DisplayEmailPreview(const char* emailPath);
    void ClearPreviewPane();
    void _UpdateToolBar();
    void _UpdateToolBarLabel(uint32 command, const char* label, bool show);
    void _UpdateNavigationButtons();
    void _UpdateMenuItemStates(bool state);
    void _DisableToolBarIcons();
    
    void _AddToDeskbar();
    void _RemoveFromDeskbar();
    void SaveColumnPrefsForView(QueryItem* item);
    void LoadColumnPrefsForView(QueryItem* item);
    BString GetColumnPrefsKey(QueryItem* item);
    
    // Sidebar keyboard navigation (Alt+Up/Down)
    void _NavigateQueryList(int32 direction);
    
    // Tracker scripting support (for Mail Next/Previous navigation)
    bool _HandleTrackerScripting(BMessage* message);
    bool _HandleSetSelection(BMessage* message);
    int32 _FindEmailRowIndexByRef(const entry_ref* ref) const;
    
    // Execute a query via the new EmailListView component
    void ExecuteQuery();
    void ResolveBaseQuery(QueryItem* item);
    void LoadTrashEmails();
    static int32 _TrashLoaderThread(void* data);
    static int32 _QueryCountThread(void* data);
    
    QueryListView* fQueryList;
    EmailListView* fEmailList;
    BTextView* fPreviewPane;
    AttachmentStripView* fAttachmentStrip;
    
    BSplitView* fHorizontalSplit;
    BSplitView* fVerticalSplit;
    
    ToolBarView* fToolBar;

    BMenuBar*  fMenuBar;
    BMenuItem* fMarkReadMenuItem;
    BMenuItem* fMarkUnreadMenuItem;
    BMenuItem* fMarkSpamMenuItem;
    BMenuItem* fUnmarkSpamMenuItem;
    BMenuItem* fTimeRangeMenuItem;
    BMenuItem* fDeskbarMenuItem;
    
    BString fMailDirectory;
    BString fTrashDirectory;  // System trash path for email filtering
    node_ref fTrashDirRef;    // For monitoring trash directory when viewing trash
    BString fCurrentFolder;
    QueryItem* fCurrentViewItem;  // Currently displayed view (for column prefs)
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<BQuery, true> fBackgroundNewMailQueries;  // Background queries for new mail count
#else
    BObjectList<BQuery> fBackgroundNewMailQueries;  // Background queries for new mail count
#endif
    BackgroundQueryHandler* fBackgroundQueryHandler;  // Handler for background query messages
    bool fShowTrashOnly;
    bool fShowSpamOnly;
    volatile bool fTrashLoaderStop;
    bool fEmptyingTrash;
    bool fAttachmentsOnly;
    bool fShowInDeskbar;
    node_ref fQueriesDirRef;
    BDirectory fQueriesDir;  // Keep this alive so node monitoring works
    BMessageRunner* fQueryReloadRunner;
    BMessageRunner* fQueryCountRunner;
    thread_id fQueryCountThread;
    volatile bool fQueryCountStop;
    int32 fQueryCountGeneration;
    BMessageRunner* fTimeRangeFilterRunner;  // Debounce time range changes
    
    // Cached query counts (refreshed by RefreshQueryCountDisplay)
    int32 fCachedNewCount;
    int32 fCachedTrashCount;
    void RefreshQueryCountDisplay(); // Query fresh counts and update UI
    
    // Search
    SearchBarView* fSearchField;
    BString fBaseQuery;  // Query predicate before search/time filters are applied
    bool fIsSearchActive;
    
    // Pending body/full-text search (deferred until BFS query completes)
    bool fPendingBodySearch;
    BString fPendingBodySearchText;
    bool fPendingBodySearchCaseSensitive;
    bool fPendingBodySearchFullText;
    
    // Time range filter
    TimeRangeSlider* fTimeRangeSlider;
    BStringView* fTimeRangeLabel;
    BGroupView* fTimeRangeGroup;
    void UpdateEmailCountLabel();
    void ApplyTimeRangeFilter();
    
    // Empty list message (shown when email list is empty)
    BStringView* fEmptyListLabel;
    BCardView* fEmailListCardView;
    void ShowEmptyListMessage(const char* message);
    void ShowEmailListContent();
    
    // Empty preview message (shown when no email is selected)
    BStringView* fEmptyPreviewLabel;
    BButton* fHtmlMessageButton;      // Button in empty preview card (for HTML-only emails)
    BButton* fHtmlVersionButton;      // Button above preview pane (for HTML alternative)
    void* fHtmlBodyContent;           // Stored HTML content for viewing (raw bytes)
    size_t fHtmlBodyContentSize;      // Size of fHtmlBodyContent buffer
    BCardView* fPreviewCardView;
    BScrollView* fPreviewScrollView;
    void ShowEmptyPreviewMessage(const char* message);
    void ShowHtmlPreviewMessage(const void* htmlContent, size_t size);
    void ShowPreviewContent();
    
    // Trash item (fixed at bottom of left pane)
    TrashItemView* fTrashItem;
    
    // Restore folder selection panel (for orphaned emails)
    BFilePanel* fRestoreFolderPanel;
    BList fPendingRestoreRefs;  // List of entry_ref* for emails awaiting folder selection
    
    // Undo delete stack — each entry is a list of node_refs from one delete operation
    // (max 10 levels; older entries are dropped when the stack is full)
    std::vector<std::vector<node_ref>> fUndoStack;
    BMenuItem* fUndoMenuItem;  // "Undo Move to Trash" in Messages menu
    
    void ApplySearchFilter();
    
    // Volume selection
    BMenu* fVolumeMenu;
    BVolumeRoster fVolumeRoster;  // Must persist for B_WATCH_VOLUME notifications
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<BVolume, true> fSelectedVolumes;  // List of currently selected volumes
#else
    BObjectList<BVolume> fSelectedVolumes;  // List of currently selected volumes
#endif
    void BuildVolumeMenu();
    void UpdateVolumeMenu();
    void LoadVolumeSelection();
    void SaveVolumeSelection();
    bool IsVolumeSelected(dev_t device) const;
    void SetVolumeSelected(dev_t device, bool selected);
    
    // Spam sender blocklist
    void LoadSpamBlocklist();
    void SaveSpamBlocklist();
    void AddToSpamBlocklist(const char* address);
    void RemoveFromSpamBlocklist(const char* address);
    bool IsSenderBlocked(const char* fromField) const;
    std::set<BString> fSpamBlocklist;  // lowercase addresses and @domains
};

class EmailViewsApp : public BApplication {
public:
    EmailViewsApp();
    virtual ~EmailViewsApp();
    virtual void MessageReceived(BMessage* message);
    virtual void RefsReceived(BMessage* message);
    virtual void ReadyToRun();
    
private:
    void              _LoadDictionaries();
    EmailViewsWindow* fWindow;
};

// Deskbar replicant for new email notification
class DeskbarReplicant : public BView {
public:
    DeskbarReplicant(BRect frame, int32 resizingMode);
    explicit DeskbarReplicant(BMessage* archive);
    virtual ~DeskbarReplicant();
    
    static _EXPORT DeskbarReplicant* Instantiate(BMessage* archive);
    virtual status_t Archive(BMessage* archive, bool deep = true) const;
    
    virtual void AttachedToWindow();
    virtual void MessageReceived(BMessage* message);
    virtual void Draw(BRect updateRect);
    virtual void MouseDown(BPoint where);
    virtual void Pulse();
    
private:
    void _Init();
    void _UpdateNewMailCount();
    void _ShowAbout();
    const char* _GetString(const char* string, const char* context);
    
    BBitmap* fIconNoEmail;
    BBitmap* fIconNewEmail;
    int32 fNewMailCount;
    BCatalog fCatalog;
};

// Helper function to find our image for resource loading in Deskbar
status_t our_image(image_info& image);

#endif // EMAILVIEWS_H
