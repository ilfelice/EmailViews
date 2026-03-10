/*
 * EmailViews.cpp - Main window for EmailViews native email viewer for Haiku
 * Distributed under the terms of the MIT License.
 *
 * This file contains three major classes:
 *   EmailViewsWindow - Three-pane main window (sidebar / email list / preview).
 *                      Owns the query list, search bar, time range filter, and
 *                      preview pane. Delegates email loading and list management
 *                      to EmailListView.
 *   DeskbarReplicant - Tray icon showing new mail count with popup menu.
 *                      Runs in the Deskbar process, loads its own resources
 *                      from our binary image.
 *   EmailViewsApp    - BApplication subclass. Manages compose windows, global
 *                      settings (gReaderSettings), and spell-check dictionaries.
 *
 * Threading model:
 *   Several operations run on background threads to keep the UI responsive:
 *   - Zip backup (ZipWorkerThread) - pipes paths to zip via stdin
 *   - Trash emptying (TrashEmptyThread) - queries + deletes
 *   - Move to trash (MoveToTrashThread) - writes _trk/original_path, moves
 *   - Restore from trash (RestoreThread) - restores by original path or account
 *   - Permanent delete (PermanentDeleteThread)
 *   - Sidebar count updates (_QueryCountThread) - counts unread/draft/trash/custom
 *   - Background new-mail queries (_InitBackgroundQueriesThread) - live BQuery
 *   All communicate back to the window thread via BMessenger::SendMessage().
 */

#include "EmailViews.h"
#include "AppInfo.h"
#include "EmailAccountMap.h"
#include "QueryNameDialog.h"
#include "reader/EmailReaderWindow.h"
#include "reader/Messages.h"
#include "reader/ReaderSettings.h"
#include "reader/ReaderSupport.h"
#include "reader/Words.h"
#include "reader/Prefs.h"
#include <Catalog.h>
#include <Screen.h>
#include <FindDirectory.h>
#include <Volume.h>
#include <VolumeRoster.h>
#include <fs_attr.h>
#include <fs_index.h>
#include <Alert.h>
#include <Size.h>
#include <DateTimeFormat.h>
#include <parsedate.h>
#include <time.h>
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include <Message.h>
#include <MessageFilter.h>
#include <Messenger.h>
#include <MessageRunner.h>
#include <IconUtils.h>
#include <Resources.h>
#include <MimeType.h>
#include <Notification.h>
#include <DataIO.h>
#include <Deskbar.h>
#include <Archivable.h>
#include <FilePanel.h>
#include <PropertyInfo.h>
#include <MailDaemon.h>
#include <MailAttachment.h>
#include <MailContainer.h>
#include <mail_util.h>
#include <StringFormat.h>
#include <cstdio>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <image.h>
#include <sys/resource.h>

#include <cmath>
#include <algorithm>
#include <OS.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "EmailViewsWindow"

// Deskbar replicant exports
extern "C" _EXPORT BView* instantiate_deskbar_item(float maxWidth, float maxHeight);

// Application identifiers
static const char* kAppSignature = "application/x-vnd.EmailViews";
static const char* kDeskbarReplicantName = "EmailViews";

// Mail app operation codes for inter-application communication
const int32 OP_REPLY = 1;
const int32 OP_REPLY_ALL = 2;
const int32 OP_FORWARD = 3;

// Internal message for Alt+Up/Down sidebar navigation
static const uint32 MSG_NAVIGATE_QUERY_LIST = 'nqls';

// Message filter that intercepts Alt+Up/Down to navigate the query sidebar.
// Installed on the window so it fires regardless of which child view has focus.
class QueryNavFilter : public BMessageFilter {
public:
    QueryNavFilter(BWindow* window)
        : BMessageFilter(B_KEY_DOWN),
          fWindow(window)
    {
    }

    virtual filter_result Filter(BMessage* message, BHandler** _target)
    {
        int32 modifiers;
        if (message->FindInt32("modifiers", &modifiers) != B_OK)
            return B_DISPATCH_MESSAGE;

        // Only act on Alt (Command key on Haiku) without other modifiers
        // (allow Alt+Shift etc. to pass through)
        if ((modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_SHIFT_KEY | B_OPTION_KEY))
                != B_COMMAND_KEY)
            return B_DISPATCH_MESSAGE;

        int32 rawChar;
        if (message->FindInt32("raw_char", &rawChar) != B_OK)
            return B_DISPATCH_MESSAGE;

        int32 direction = 0;
        if (rawChar == B_UP_ARROW)
            direction = -1;
        else if (rawChar == B_DOWN_ARROW)
            direction = 1;

        if (direction == 0)
            return B_DISPATCH_MESSAGE;

        // Post navigation message to the window
        BMessage nav(MSG_NAVIGATE_QUERY_LIST);
        nav.AddInt32("direction", direction);
        fWindow->PostMessage(&nav);

        return B_SKIP_MESSAGE;
    }

private:
    BWindow* fWindow;
};

// Zip backup worker thread data (uses piped input to handle large file lists)
struct ZipWorkerData {
    BString savePath;
    BString* paths;
    int32 count;
    BMessenger messenger;  // To notify window when done
};

static status_t ZipWorkerThread(void* data)
{
    ZipWorkerData* wd = (ZipWorkerData*)data;
    
    // Build command: zip -ry <archive> -@
    // The -@ flag tells zip to read filenames from stdin
    // Note: we don't use -j (junk paths) so directory structure is preserved,
    // avoiding duplicate filename overwrites from different folders
    BString command("zip -ry '");
    command << wd->savePath << "' -@ > /dev/null 2>&1";
    
    // Open pipe to zip process
    FILE* zipPipe = popen(command.String(), "w");
    if (zipPipe == NULL) {
        delete[] wd->paths;
        
        BNotification notification(B_ERROR_NOTIFICATION);
        notification.SetGroup(B_TRANSLATE_SYSTEM_NAME(kAppName));
        notification.SetTitle(B_TRANSLATE("Backup"));
        notification.SetContent(B_TRANSLATE("Failed to start zip process."));
        notification.Send();
        
        // Notify window that backup finished (even on failure)
        BMessage doneMsg(MSG_BACKUP_FINISHED);
        wd->messenger.SendMessage(&doneMsg);
        delete wd;
        return B_ERROR;
    }
    
    // Write each file path to zip's stdin
    for (int32 i = 0; i < wd->count; i++) {
        if (wd->paths[i].Length() > 0) {
            fprintf(zipPipe, "%s\n", wd->paths[i].String());
        }
    }
    
    // Close pipe and wait for zip to finish
    int result = pclose(zipPipe);
    
    // Send notification
    BNotification notification(result == 0 ? 
        B_INFORMATION_NOTIFICATION : B_ERROR_NOTIFICATION);
    notification.SetGroup(B_TRANSLATE_SYSTEM_NAME(kAppName));
    notification.SetTitle(B_TRANSLATE_COMMENT("Backup emails", "Notification title"));
    
    if (result == 0) {
        BString content;
        static BStringFormat format(B_TRANSLATE("{0, plural,"
            "one{Successfully backed up # email.}"
            "other{Successfully backed up # emails.}}"));
        format.Format(content, wd->count);
        notification.SetContent(content.String());
    } else {
        notification.SetContent(B_TRANSLATE("Backup failed: could not create ZIP archive."));
    }
    notification.Send();
    
    // Notify window that backup finished
    BMessage doneMsg(MSG_BACKUP_FINISHED);
    wd->messenger.SendMessage(&doneMsg);
    
    delete[] wd->paths;
    delete wd;
    return B_OK;
}

// Trash empty thread data
struct TrashEmptyData {
    BList* emailRefs;  // List of entry_ref* to delete (may be NULL if query needed)
    BMessenger messenger;  // To notify window when done
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<BVolume, false> volumes;  // Volumes to query (non-owning)
#else
    BObjectList<BVolume> volumes;  // Volumes to query (non-owning)
#endif
};

static status_t TrashEmptyThread(void* data)
{
    TrashEmptyData* emptyData = (TrashEmptyData*)data;
    
    // If no refs provided, query for them now (moved from window thread)
    if (emptyData->emailRefs == NULL) {
        emptyData->emailRefs = new BList();
        
        for (int32 v = 0; v < emptyData->volumes.CountItems(); v++) {
            BVolume* volume = emptyData->volumes.ItemAt(v);
            if (volume == NULL)
                continue;
            
            BPath volumeTrashPath;
            if (find_directory(B_TRASH_DIRECTORY, &volumeTrashPath, false, volume) != B_OK)
                continue;
            BString trashLower(volumeTrashPath.Path());
            trashLower.ToLower();
            
            BQuery trashQuery;
            trashQuery.SetVolume(volume);
            trashQuery.SetPredicate("MAIL:subject=**");
            
            if (trashQuery.Fetch() == B_OK) {
                entry_ref ref;
                while (trashQuery.GetNextRef(&ref) == B_OK) {
                    BEntry entry(&ref);
                    BPath path;
                    entry.GetPath(&path);
                    
                    BString pathStr(path.Path());
                    pathStr.ToLower();
                    if (pathStr.FindFirst(trashLower) >= 0) {
                        entry_ref* refCopy = new entry_ref(ref);
                        emptyData->emailRefs->AddItem(refCopy);
                    }
                }
            }
        }
    }
    
    int32 count = emptyData->emailRefs->CountItems();
    int32 deleted = 0;
    
    // Delete all emails
    for (int32 i = 0; i < count; i++) {
        entry_ref* ref = (entry_ref*)emptyData->emailRefs->ItemAt(i);
        BEntry entry(ref);
        if (entry.Remove() == B_OK)
            deleted++;
        delete ref;
    }
    delete emptyData->emailRefs;
    
    // Send notification
    BNotification notification(B_INFORMATION_NOTIFICATION);
    notification.SetGroup(B_TRANSLATE_SYSTEM_NAME(kAppName));
    notification.SetTitle(B_TRANSLATE_COMMENT("Delete from Trash", "Notification title"));
    
    BString content;
        static BStringFormat format(B_TRANSLATE("{0, plural,"
            "one{Permanently deleted # email.}"
            "other{Permanently deleted # emails.}}"));
        format.Format(content, deleted);

    notification.SetContent(content.String());
    notification.Send();
    
    // Notify window to update UI
    BMessage doneMsg(MSG_TRASH_EMPTIED);
    emptyData->messenger.SendMessage(&doneMsg);
    
    delete emptyData;
    return B_OK;
}

// Query count thread data (for counting unread/draft/trash in background)
struct QueryCountCustomQuery {
    BString path;       // File path to the query file
    BString predicate;  // Query predicate read from _trk/qrystr attribute
    BString baseName;   // Display name without count
};

struct QueryCountData {
    BMessenger messenger;
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<BVolume, true> volumes;
    BObjectList<QueryCountCustomQuery, true> customQueries;
#else
    BObjectList<BVolume> volumes;
    BObjectList<QueryCountCustomQuery> customQueries;
#endif
    volatile bool* stopFlag;
    bool showTrashOnly;
    int32 listCount;
    int32 generation;
};

// Extract bare email address from a MAIL:from field like
// "John Doe <john@example.com>" or just "john@example.com"
static BString
_ExtractEmailAddress(const char* fromField)
{
    if (fromField == NULL)
        return BString();

    BString from(fromField);
    int32 open = from.FindFirst('<');
    int32 close = from.FindFirst('>', open);
    if (open >= 0 && close > open) {
        BString addr;
        from.CopyInto(addr, open + 1, close - open - 1);
        addr.Trim();
        addr.ToLower();
        return addr;
    }
    // No angle brackets — treat the whole thing as an address
    from.Trim();
    from.ToLower();
    return from;
}

static int32 _CountWithExclusions(BQuery& query, const BString& trashLower,
    volatile bool* stopFlag)
{
    int32 count = 0;
    entry_ref ref;
    while (query.GetNextRef(&ref) == B_OK) {
        if (stopFlag && *stopFlag)
            return count;
        BPath path(&ref);
        BString pathStr(path.Path());
        pathStr.ToLower();
        // Exclude trash
        if (trashLower.Length() > 0 && pathStr.FindFirst(trashLower) >= 0)
            continue;
        // Exclude spam
        BNode node(&ref);
        BString classification;
        if (node.InitCheck() == B_OK
            && node.ReadAttrString("MAIL:classification", &classification) == B_OK
            && classification.ICompare("Spam") == 0)
            continue;
        count++;
    }
    return count;
}

struct PermanentDeleteData {
    BList* emailPaths;  // List of BString* paths to delete
    BMessenger messenger;  // To notify window when done
    int32 firstSelectedIndex;  // For repositioning selection after delete
};

// Message for permanent delete completion
const uint32 MSG_PERMANENT_DELETE_DONE = 'pddn';

// Background move-to-trash
struct MoveToTrashData {
    BList* emailPaths;     // List of BString* paths to move
    BMessenger messenger;  // To notify window when done
};

const uint32 MSG_MOVE_TO_TRASH_DONE = 'mtdn';

static status_t MoveToTrashThread(void* data)
{
    MoveToTrashData* trashData = (MoveToTrashData*)data;
    
    for (int32 i = 0; i < trashData->emailPaths->CountItems(); i++) {
        BString* pathStr = (BString*)trashData->emailPaths->ItemAt(i);
        if (!pathStr)
            continue;
        
        BEntry entry(pathStr->String());
        if (entry.InitCheck() != B_OK) {
            delete pathStr;
            continue;
        }
        
        BVolume entryVolume;
        if (entry.GetVolume(&entryVolume) != B_OK) {
            delete pathStr;
            continue;
        }
        
        BPath trashPath;
        if (find_directory(B_TRASH_DIRECTORY, &trashPath, true, &entryVolume) != B_OK) {
            delete pathStr;
            continue;
        }
        
        BDirectory trashDir(trashPath.Path());
        if (trashDir.InitCheck() != B_OK) {
            delete pathStr;
            continue;
        }
        
        // Store original path as _trk/original_path attribute before moving.
        // This is the same attribute Tracker uses, enabling restore via either
        // EmailViews or Tracker's "Restore" menu item.
        BNode node(&entry);
        if (node.InitCheck() == B_OK) {
            node.WriteAttr("_trk/original_path", B_STRING_TYPE, 0,
                pathStr->String(), pathStr->Length() + 1);
        }
        
        // Move to trash, handle name collision
        status_t moveStatus = entry.MoveTo(&trashDir);
        if (moveStatus == B_FILE_EXISTS) {
            char originalName[B_FILE_NAME_LENGTH];
            entry.GetName(originalName);
            
            for (int32 suffix = 1; suffix < 1000; suffix++) {
                BString newName(originalName);
                newName << " " << suffix;
                
                BEntry testEntry;
                BPath testPath(trashPath.Path());
                testPath.Append(newName.String());
                if (testEntry.SetTo(testPath.Path()) != B_OK || !testEntry.Exists()) {
                    if (entry.Rename(newName.String()) == B_OK) {
                        entry.MoveTo(&trashDir);
                        break;
                    }
                }
            }
        }
        
        delete pathStr;
    }
    
    // Notify window that move is complete
    BMessage doneMsg(MSG_MOVE_TO_TRASH_DONE);
    trashData->messenger.SendMessage(&doneMsg);
    
    delete trashData->emailPaths;
    delete trashData;
    return B_OK;
}

// Restore thread data (for restoring selected emails from Trash view)
struct RestoreThreadData {
    BList* emailRefs;  // List of entry_ref* to restore
    BMessenger messenger;  // To notify window when done
    int32 firstSelectedIndex;  // For repositioning selection after restore
    std::map<int32, BString>* accountMap;  // Copy of account map for lookups
};

// Message for restore completion
const uint32 MSG_RESTORE_DONE = 'rsdn';

// Structure to track restore results
struct RestoreResult {
    node_ref nref;
    bool success;
};

static status_t RestoreThread(void* data)
{
    RestoreThreadData* restoreData = (RestoreThreadData*)data;
    
    // Track results for UI update
    BList* results = new BList();  // List of RestoreResult*
    BList* orphanedRefs = new BList();  // List of entry_ref* for emails with unknown accounts
    
    for (int32 i = 0; i < restoreData->emailRefs->CountItems(); i++) {
        entry_ref* ref = (entry_ref*)restoreData->emailRefs->ItemAt(i);
        if (!ref)
            continue;
        
        BNode node(ref);
        if (node.InitCheck() != B_OK) {
            delete ref;
            continue;
        }
        
        node_ref nref;
        node.GetNodeRef(&nref);
        
        // Try to get original path first
        char originalPath[B_PATH_NAME_LENGTH];
        ssize_t size = node.ReadAttr("_trk/original_path", B_STRING_TYPE, 0, 
            originalPath, sizeof(originalPath) - 1);
        
        // Restore destination priority:
        // 1. _trk/original_path attribute (set by MoveToTrashThread or Tracker)
        // 2. Account-based inbox path (~/mail/<account>/INBOX)
        // 3. Orphaned list (account deleted - user picks folder via file panel)
        BString destinationFolder;
        
        if (size > 0) {
            originalPath[size] = '\0';
            BPath origPath(originalPath);
            BPath parentPath;
            if (origPath.GetParent(&parentPath) == B_OK) {
                destinationFolder = parentPath.Path();
            }
        }
        
        // If no original path, try account-based restoration
        if (destinationFolder.IsEmpty()) {
            int32 accountId = -1;
            bool hasAccountAttr = false;
            bool accountFound = false;
            
            attr_info attrInfo;
            if (node.GetAttrInfo("MAIL:account", &attrInfo) == B_OK) {
                hasAccountAttr = true;
                if (attrInfo.type == B_INT32_TYPE) {
                    node.ReadAttr("MAIL:account", B_INT32_TYPE, 0, &accountId, sizeof(accountId));
                    accountFound = (restoreData->accountMap->find(accountId) != restoreData->accountMap->end());
                } else if (attrInfo.type == B_STRING_TYPE) {
                    char accountName[256];
                    ssize_t nameSize = node.ReadAttr("MAIL:account", B_STRING_TYPE, 0, accountName, sizeof(accountName) - 1);
                    if (nameSize > 0) {
                        accountName[nameSize] = '\0';
                        for (auto& pair : *restoreData->accountMap) {
                            if (pair.second == accountName) {
                                accountId = pair.first;
                                accountFound = true;
                                break;
                            }
                        }
                    }
                }
            }
            
            if (hasAccountAttr && !accountFound) {
                // Account not found - add to orphaned list for folder selection dialog
                orphanedRefs->AddItem(new entry_ref(*ref));
                delete ref;
                continue;
            }
            
            // Get inbox path for account
            if (accountFound && accountId >= 0) {
                auto it = restoreData->accountMap->find(accountId);
                if (it != restoreData->accountMap->end()) {
                    BPath mailPath;
                    if (find_directory(B_USER_DIRECTORY, &mailPath) == B_OK) {
                        mailPath.Append("mail");
                        mailPath.Append(it->second.String());
                        mailPath.Append("INBOX");
                        destinationFolder = mailPath.Path();
                    }
                }
            }
        }
        
        // Perform the restore
        RestoreResult* result = new RestoreResult();
        result->nref = nref;
        result->success = false;
        
        if (!destinationFolder.IsEmpty()) {
            create_directory(destinationFolder.String(), 0755);
            BEntry entry(ref);
            if (entry.InitCheck() == B_OK) {
                BDirectory destDir(destinationFolder.String());
                if (destDir.InitCheck() == B_OK) {
                    result->success = (entry.MoveTo(&destDir) == B_OK);
                }
            }
        }
        
        results->AddItem(result);
        delete ref;
    }
    
    delete restoreData->emailRefs;
    delete restoreData->accountMap;
    
    // Notify window to update UI
    BMessage doneMsg(MSG_RESTORE_DONE);
    doneMsg.AddInt32("first_index", restoreData->firstSelectedIndex);
    doneMsg.AddPointer("results", results);
    doneMsg.AddPointer("orphaned_refs", orphanedRefs);
    restoreData->messenger.SendMessage(&doneMsg);
    
    delete restoreData;
    return B_OK;
}

static status_t PermanentDeleteThread(void* data)
{
    PermanentDeleteData* deleteData = (PermanentDeleteData*)data;
    
    int32 count = deleteData->emailPaths->CountItems();
    int32 deleted = 0;
    
    // Track successfully deleted paths to pass back for UI update
    BList* deletedPaths = new BList();
    
    // Delete all emails
    for (int32 i = 0; i < count; i++) {
        BString* path = (BString*)deleteData->emailPaths->ItemAt(i);
        if (path) {
            BEntry entry(path->String());
            if (entry.InitCheck() == B_OK && entry.Remove() == B_OK) {
                deleted++;
                // Keep path for UI update
                deletedPaths->AddItem(path);
            } else {
                // Delete failed - free the path
                delete path;
            }
        }
    }
    delete deleteData->emailPaths;
    
    // Send notification
    BNotification notification(B_INFORMATION_NOTIFICATION);
    notification.SetGroup(B_TRANSLATE_SYSTEM_NAME(kAppName));
    notification.SetTitle(B_TRANSLATE_COMMENT("Delete from Trash", "Notification title"));

    BString content;
        static BStringFormat format(B_TRANSLATE("{0, plural,"
            "one{Permanently deleted # email.}"
            "other{Permanently deleted # emails.}}"));
        format.Format(content, deleted);
    notification.SetContent(content.String());
    notification.Send();
    
    // Notify window to update UI - pass the deleted paths
    BMessage doneMsg(MSG_PERMANENT_DELETE_DONE);
    doneMsg.AddInt32("first_index", deleteData->firstSelectedIndex);
    doneMsg.AddPointer("deleted_paths", deletedPaths);
    deleteData->messenger.SendMessage(&doneMsg);
    
    delete deleteData;
    return B_OK;
}






// Background thread to initialize live queries for new mail count.
// BQuery::Fetch() returns initial results synchronously. With many emails,
// consuming these results can take several seconds. We do this on a background
// thread so the window doesn't block. After consuming all initial results,
// the query transitions to live mode and sends B_QUERY_UPDATE for new matches.
/*static*/ status_t
EmailViewsWindow::_InitBackgroundQueriesThread(void* data)
{
    EmailViewsWindow* window = static_cast<EmailViewsWindow*>(data);
    
    for (int32 i = 0; i < window->fSelectedVolumes.CountItems(); i++) {
        BVolume* volume = window->fSelectedVolumes.ItemAt(i);
        if (volume == NULL)
            continue;
        
        BQuery* query = new BQuery();
        query->SetVolume(volume);
        
        // Must lock window to access fBackgroundQueryHandler
        if (!window->LockLooper()) {
            delete query;
            continue;
        }
        query->SetTarget(BMessenger(window->fBackgroundQueryHandler, window));
        window->UnlockLooper();
        
        query->SetPredicate("(BEOS:TYPE==\"text/x-email\")&&((MAIL:status==New)||(MAIL:status==Seen))");
        if (query->Fetch() == B_OK) {
            // Must consume all initial results before live updates are sent
            entry_ref ref;
            while (query->GetNextRef(&ref) == B_OK) {
                // Just consume, don't process
            }
            
            if (window->LockLooper()) {
                window->fBackgroundNewMailQueries.AddItem(query);
                window->UnlockLooper();
            } else {
                delete query;
            }
        } else {
            delete query;
        }
    }
    
    // Trigger initial count update now that queries are primed
    if (window->LockLooper()) {
        window->ScheduleQueryCountUpdate();
        window->UnlockLooper();
    }
    
    return B_OK;
}


// EmailViewsWindow implementation
EmailViewsWindow::EmailViewsWindow()
    : BWindow(BRect(100, 100, 900, 700), kAppName,
              B_DOCUMENT_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
      fBackgroundQueryHandler(NULL),
      fShowTrashOnly(false),
      fShowSpamOnly(false),
      fTrashLoaderStop(false),
      fEmptyingTrash(false),
      fAttachmentsOnly(false),
      fQueryReloadRunner(NULL),
      fQueryCountRunner(NULL),
      fQueryCountThread(-1),
      fQueryCountStop(false),
      fQueryCountGeneration(0),
      fTimeRangeFilterRunner(NULL),
      fShowInDeskbar(false),
      fCachedNewCount(-1),
      fCachedTrashCount(-1),
      fSearchField(NULL),
      fIsSearchActive(false),
      fPendingBodySearch(false),
      fPendingBodySearchCaseSensitive(false),
      fPendingBodySearchFullText(false),
      fTimeRangeSlider(NULL),
      fTimeRangeLabel(NULL),
      fTimeRangeGroup(NULL),
      fEmptyListLabel(NULL),
      fEmailListCardView(NULL),
      fEmptyPreviewLabel(NULL),
      fHtmlMessageButton(NULL),
      fHtmlVersionButton(NULL),
      fHtmlBodyContent(NULL),
      fHtmlBodyContentSize(0),
      fPreviewCardView(NULL),
      fPreviewScrollView(NULL),
      fTrashItem(NULL),
      fDeskbarMenuItem(NULL),
      fCurrentViewItem(NULL),
      fAttachmentStrip(NULL),
      fRestoreFolderPanel(NULL),
      fVolumeMenu(NULL),
      fSelectedVolumes(5)
{
    // Find mail directory
    BPath path;
    if (find_directory(B_USER_DIRECTORY, &path) == B_OK) {
        path.Append("mail");
        fMailDirectory = path.Path();
    }
    
    // Find trash directory and get its node_ref for monitoring
    BPath trashPath;
    if (find_directory(B_TRASH_DIRECTORY, &trashPath) == B_OK) {
        fTrashDirectory = trashPath.Path();
        BDirectory trashDir(trashPath.Path());
        if (trashDir.InitCheck() == B_OK) {
            trashDir.GetNodeRef(&fTrashDirRef);
        }
    }
    
    // Ensure FILE:starred index exists on the boot volume for starred email
    // queries. EmailViews uses this custom attribute (not part of mail_daemon)
    // to track starred status. Without the index, BQuery can't match on it.
    // fs_create_index silently returns EEXIST if already present.
    {
        BVolumeRoster volumeRoster;
        BVolume bootVolume;
        volumeRoster.GetBootVolume(&bootVolume);
        
        if (bootVolume.InitCheck() == B_OK) {
            // Create index - ignore EEXIST (already exists)
            fs_create_index(bootVolume.Device(), "FILE:starred", B_INT32_TYPE, 0);
            fs_create_index(bootVolume.Device(), "MAIL:classification", B_STRING_TYPE, 0);
        }
    }
    
    // Create menu bar
    fMenuBar = new BMenuBar("menubar");
    
    // Create EmailViews menu
    BMenu* mailViewerMenu = new BMenu(B_TRANSLATE_SYSTEM_NAME(kAppName));
    mailViewerMenu->AddItem(new BMenuItem(B_TRANSLATE("About EmailViews" B_UTF8_ELLIPSIS), 
                                          new BMessage(MSG_ABOUT)));
    mailViewerMenu->AddSeparatorItem();
    BMenuItem* emailSettingsItem = new BMenuItem(B_TRANSLATE("Email preferences" B_UTF8_ELLIPSIS), 
                                          new BMessage(M_PREFS),',');
    emailSettingsItem->SetTarget(be_app);
    mailViewerMenu->AddItem(emailSettingsItem);
    mailViewerMenu->AddItem(new BMenuItem(B_TRANSLATE("Email accounts" B_UTF8_ELLIPSIS), 
                                          new BMessage(MSG_EMAIL_SETTINGS)));
    fDeskbarMenuItem = new BMenuItem(B_TRANSLATE("Show in Deskbar"), new BMessage(MSG_TOGGLE_DESKBAR));
    mailViewerMenu->AddItem(fDeskbarMenuItem);
    mailViewerMenu->AddSeparatorItem();
    mailViewerMenu->AddItem(new BMenuItem(B_TRANSLATE("Quit"), new BMessage(MSG_QUIT), 'Q'));
    fMenuBar->AddItem(mailViewerMenu);
    
    // Create Messages menu
    BMenu* messagesMenu = new BMenu(B_TRANSLATE("Messages"));
    messagesMenu->AddItem(new BMenuItem(B_TRANSLATE("New email"), new BMessage(MSG_CREATE_EMAIL), 'N'));
    messagesMenu->AddSeparatorItem();
    messagesMenu->AddItem(new BMenuItem(B_TRANSLATE("Reply"), new BMessage(MSG_REPLY), 'R'));
    messagesMenu->AddItem(new BMenuItem(B_TRANSLATE("Reply all"), new BMessage(MSG_REPLY_ALL), 'R', B_SHIFT_KEY));
    messagesMenu->AddItem(new BMenuItem(B_TRANSLATE("Forward"), new BMessage(MSG_FORWARD), 'F', B_SHIFT_KEY));
    messagesMenu->AddSeparatorItem();
    fMarkReadMenuItem = new BMenuItem(B_TRANSLATE("Mark as read"), new BMessage(MSG_MARK_READ),'M');
    fMarkReadMenuItem->SetEnabled(false);
    messagesMenu->AddItem(fMarkReadMenuItem);
    fMarkUnreadMenuItem = new BMenuItem(B_TRANSLATE("Mark as unread"), new BMessage(MSG_MARK_UNREAD));
    fMarkUnreadMenuItem->SetEnabled(false);
    messagesMenu->AddItem(fMarkUnreadMenuItem);
    messagesMenu->AddSeparatorItem();
    BMenuItem* item = new BMenuItem(B_TRANSLATE("Move to Trash"), new BMessage(MSG_DELETE_EMAIL));
    item->SetEnabled(false);
    messagesMenu->AddItem(item);
    fMarkSpamMenuItem = new BMenuItem(B_TRANSLATE("Mark as spam"), new BMessage(MSG_MARK_SPAM));
    fMarkSpamMenuItem->SetEnabled(false);
    messagesMenu->AddItem(fMarkSpamMenuItem);
    fUnmarkSpamMenuItem = new BMenuItem(B_TRANSLATE("Not spam"), new BMessage(MSG_UNMARK_SPAM));
    fUnmarkSpamMenuItem->SetEnabled(false);
    messagesMenu->AddItem(fUnmarkSpamMenuItem);
    messagesMenu->AddSeparatorItem();
    fUndoMenuItem = new BMenuItem(B_TRANSLATE("Undo Move to Trash"), new BMessage(MSG_UNDO_DELETE), 'Z');
    fUndoMenuItem->SetEnabled(false);
    messagesMenu->AddItem(fUndoMenuItem);
    fMenuBar->AddItem(messagesMenu);

    // Create Filter menu
    BMenu* filterMenu = new BMenu(B_TRANSLATE("Filter"));
    filterMenu->AddItem(new BMenuItem(B_TRANSLATE("Filter by 'Sender'"), new BMessage(MSG_FILTER_EXACT_SENDER)));
    filterMenu->AddItem(new BMenuItem(B_TRANSLATE("Filter by 'Recipient'"), new BMessage(MSG_FILTER_EXACT_RECIPIENT)));
    filterMenu->AddItem(new BMenuItem(B_TRANSLATE("Filter by 'Subject'"), new BMessage(MSG_FILTER_EXACT_SUBJECT)));
    filterMenu->AddItem(new BMenuItem(B_TRANSLATE("Filter by 'Account'"), new BMessage(MSG_FILTER_EXACT_ACCOUNT)));
    filterMenu->AddSeparatorItem();
    filterMenu->AddItem(new BMenuItem(B_TRANSLATE("Clear filter"), new BMessage(MSG_SEARCH_CLEAR)));
    filterMenu->AddSeparatorItem();
    fTimeRangeMenuItem = new BMenuItem(B_TRANSLATE("Show time range"),
        new BMessage(MSG_TOGGLE_TIME_RANGE), 'T', B_SHIFT_KEY);
    fTimeRangeMenuItem->SetMarked(true);
    filterMenu->AddItem(fTimeRangeMenuItem);
    fMenuBar->AddItem(filterMenu);

    // Create Queries menu
    BMenu* queriesMenu = new BMenu(B_TRANSLATE("Queries"));
    queriesMenu->AddItem(new BMenuItem(B_TRANSLATE("Add 'From' query"), new BMessage(MSG_FILTER_BY_SENDER)));
    queriesMenu->AddItem(new BMenuItem(B_TRANSLATE("Add 'To' query"), new BMessage(MSG_FILTER_BY_RECIPIENT)));
    queriesMenu->AddItem(new BMenuItem(B_TRANSLATE("Add 'Account' query"), new BMessage(MSG_FILTER_BY_ACCOUNT)));
    queriesMenu->AddSeparatorItem();
    queriesMenu->AddItem(new BMenuItem(B_TRANSLATE("Open queries folder"), new BMessage(MSG_OPEN_QUERIES_FOLDER)));
    fMenuBar->AddItem(queriesMenu);

    // Create Volumes menu (will be populated by BuildVolumeMenu)
    fVolumeMenu = new BMenu(B_TRANSLATE("Volumes"));
    fMenuBar->AddItem(fVolumeMenu);
    
    // Helper function to load HVIF icon from app resources by numeric ID
    auto LoadIconById = [](int32 id) -> BBitmap* {
        BResources* resources = BApplication::AppResources();
        if (resources == NULL)
            return nullptr;
        
        size_t size;
        const void* data = resources->LoadResource(B_VECTOR_ICON_TYPE, id, &size);
        if (data == NULL || size == 0)
            return nullptr;
        
        BBitmap* icon = new BBitmap(BRect(BPoint(0, 0),
            be_control_look->ComposeIconSize(31)), B_RGBA32);
        if (BIconUtils::GetVectorIcon((const uint8*)data, size, icon) == B_OK) {
            return icon;
        }
        
        delete icon;
        return nullptr;
    };
    
    // Resource IDs for toolbar icons (from EmailViews.rdef)
    const int32 kIconCheckEmail = 205;
    const int32 kIconNewEmail = 201;
    const int32 kIconReply = 1008;
    const int32 kIconForward = 1009;
    const int32 kIconMarkRead = 202;
    const int32 kIconMarkUnread = 203;
    const int32 kIconDelete = 204;
    
    // Create toolbar
    fToolBar = new ToolBarView();
    #undef B_TRANSLATION_CONTEXT
    #define B_TRANSLATION_CONTEXT "Toolbar"
    fToolBar->AddAction(MSG_CHECK_EMAIL, this, LoadIconById(kIconCheckEmail), NULL, B_TRANSLATE_COMMENT("Check email", "As short as possible"));
    fToolBar->AddAction(MSG_NEW_EMAIL, this, LoadIconById(kIconNewEmail), NULL, B_TRANSLATE_COMMENT("New email", "As short as possible"));
    fToolBar->AddAction(MSG_REPLY, this, LoadIconById(kIconReply), NULL, B_TRANSLATE_COMMENT("Reply", "As short as possible"));
    fToolBar->AddAction(MSG_FORWARD, this, LoadIconById(kIconForward), NULL, B_TRANSLATE_COMMENT("Forward", "As short as possible"));
    fToolBar->AddAction(MSG_MARK_READ, this, LoadIconById(kIconMarkRead), NULL, B_TRANSLATE_COMMENT("Mark read", "As short as possible"));
    fToolBar->AddAction(MSG_MARK_UNREAD, this, LoadIconById(kIconMarkUnread), NULL, B_TRANSLATE_COMMENT("Mark unread", "As short as possible"));
    fToolBar->AddAction(MSG_DELETE_EMAIL, this, LoadIconById(kIconDelete), NULL, B_TRANSLATE_COMMENT("Trash", "As short as possible"));
    fToolBar->AddSeparator();
    const int32 kIconNext = 1013;
    const int32 kIconPrevious = 1012;
    fToolBar->AddAction(MSG_NEXT_EMAIL, this, LoadIconById(kIconNext), NULL, B_TRANSLATE_COMMENT("Next", "As short as possible"));
    fToolBar->AddAction(MSG_PREV_EMAIL, this, LoadIconById(kIconPrevious), NULL, B_TRANSLATE_COMMENT("Previous", "As short as possible"));
    _DisableToolBarIcons();

    fToolBar->AddGlue();  // Glue at end like reader window
    #undef B_TRANSLATION_CONTEXT
    #define B_TRANSLATION_CONTEXT "EmailViewsWindow"
    
    // Apply toolbar button bar preference (icons only vs icons & labels)
    _UpdateToolBar();
    
    // Create the query list (no border - toolbar separator provides top line)
    fQueryList = new QueryListView("queryList", this);
    fQueryList->SetSelectionMessage(new BMessage(MSG_QUERY_SELECTED));
    BScrollView* queryScroll = new BScrollView("queryScroll", fQueryList,
                                                  0, false, true, B_NO_BORDER);
    
    // Create the fixed trash item at bottom
    fTrashItem = new TrashItemView(this);
    BScrollView* trashScroll = new BScrollView("trashScroll", fTrashItem,
                                                0, false, false, B_PLAIN_BORDER);  // No scrollbars
    // Height matches the built-in sidebar row height: ComposeIconSize(31) + one label spacing
    float trashIconSize = (float)be_control_look->ComposeIconSize(31).width + 1;
    float trashRowHeight = trashIconSize + be_control_look->DefaultLabelSpacing();
    trashScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, trashRowHeight));
    trashScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, trashRowHeight));
    
    // Create container for query list + trash item
    // Use -1 top inset on trashScroll to merge with queryScroll's bottom border
    BGroupView* queryContainer = new BGroupView(B_VERTICAL, 0);
    BLayoutBuilder::Group<>(queryContainer, B_VERTICAL, 0)
        .Add(queryScroll, 1.0f)
        .AddGroup(B_VERTICAL, 0)
            .Add(trashScroll, 0.0f)
            .SetInsets(0, -1, 0, 0)
        .End()
        .SetInsets(0, -1, 0, -1);

    // Create the email list view - self-contained high-performance component
    // Includes column headers, scrollbars, status bar, query execution, live queries
    fEmailList = new EmailListView("emailList", this);
    fEmailList->SetSelectionMessage(new BMessage(MSG_EMAIL_SELECTED));
    fEmailList->SetInvocationMessage(new BMessage(MSG_EMAIL_INVOKED));
    
    // Create empty list message (shown when no emails match)
    // Use BStringView in a centered layout for automatic positioning
    fEmptyListLabel = new BStringView("emptyListLabel", 
        B_TRANSLATE("No emails found."));
    fEmptyListLabel->SetAlignment(B_ALIGN_CENTER);
    fEmptyListLabel->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    
    // Create a container for the empty message with centered layout
    BGroupView* emptyListInner = new BGroupView(B_VERTICAL);
    emptyListInner->SetViewUIColor(B_LIST_BACKGROUND_COLOR);
    emptyListInner->SetExplicitMinSize(BSize(0, 0));
    emptyListInner->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    BLayoutBuilder::Group<>(emptyListInner, B_VERTICAL)
        .AddGlue()
        .Add(fEmptyListLabel)
        .AddGlue()
        .SetInsets(B_USE_DEFAULT_SPACING);
    
    // Wrap in scroll view for border matching the email list (B_FANCY_BORDER)
    BScrollView* emptyListContainer = new BScrollView("emptyListScroll", emptyListInner,
        0, false, false, B_FANCY_BORDER);
    emptyListContainer->SetExplicitMinSize(BSize(0, 0));
    emptyListContainer->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    
    // Create card view to switch between empty message and email list
    fEmailListCardView = new BCardView("emailListCards");
    fEmailListCardView->SetExplicitMinSize(BSize(0, 0));
    fEmailListCardView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    fEmailListCardView->AddChild(emptyListContainer);  // Card 0: empty message
    fEmailListCardView->AddChild(fEmailList);          // Card 1: email list
    fEmailListCardView->CardLayout()->SetVisibleItem((int32)0);  // Start with empty message;
    
    // Create the preview pane for displaying email content
    fPreviewPane = new BTextView("previewPane");
    fPreviewPane->MakeEditable(false);
    fPreviewPane->SetStylable(true);
    fPreviewPane->SetWordWrap(true);
    fPreviewPane->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    fPreviewPane->SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    fPreviewPane->SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
    fPreviewScrollView = new BScrollView("previewScroll", fPreviewPane,
                                                   0, true, true, B_PLAIN_BORDER);
    
    // Create empty preview message (shown when no email is selected)
    // Use BStringView in a centered layout for automatic positioning
    fEmptyPreviewLabel = new BStringView("emptyPreviewLabel", 
        B_TRANSLATE("Select an email to preview its contents."));
    fEmptyPreviewLabel->SetAlignment(B_ALIGN_CENTER);
    fEmptyPreviewLabel->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    
    // Create HTML message button (hidden by default)
    fHtmlMessageButton = new BButton("htmlMessageButton",
        B_TRANSLATE("HTML message"), new BMessage(MSG_VIEW_HTML_MESSAGE));
    fHtmlMessageButton->SetExplicitMaxSize(BSize(B_SIZE_UNSET, B_SIZE_UNSET));
    fHtmlMessageButton->Hide();
    
    // Create a container for the empty message with centered layout
    BGroupView* emptyPreviewInner = new BGroupView(B_VERTICAL);
    emptyPreviewInner->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    emptyPreviewInner->SetExplicitMinSize(BSize(0, 0));
    emptyPreviewInner->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    BLayoutBuilder::Group<>(emptyPreviewInner, B_VERTICAL)
        .AddGlue()
        .Add(fEmptyPreviewLabel)
        .Add(fHtmlMessageButton)
        .AddGlue()
        .SetInsets(B_USE_DEFAULT_SPACING);
    
    // Wrap in scroll view for border matching the preview pane (B_PLAIN_BORDER)
    BScrollView* emptyPreviewContainer = new BScrollView("emptyPreviewScroll", emptyPreviewInner,
        0, false, false, B_PLAIN_BORDER);
    emptyPreviewContainer->SetExplicitMinSize(BSize(0, 0));
    emptyPreviewContainer->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    
    // Create HTML version button (shown above preview when HTML alternative exists)
    fHtmlVersionButton = new BButton("htmlVersionButton",
        B_TRANSLATE("View HTML version"), new BMessage(MSG_VIEW_HTML_MESSAGE));
    fHtmlVersionButton->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fHtmlVersionButton->Hide();
    
    // Create preview content group (HTML button + preview scroll view)
    BGroupView* previewContentGroup = new BGroupView(B_VERTICAL, 0);
    previewContentGroup->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    BLayoutBuilder::Group<>(previewContentGroup, B_VERTICAL, 0)
        .Add(fHtmlVersionButton, 0.0f)
        .Add(fPreviewScrollView, 1.0f)
        .SetInsets(0);
    
    // Create card view to switch between empty message and preview content
    fPreviewCardView = new BCardView("previewCards");
    fPreviewCardView->SetExplicitMinSize(BSize(0, 0));
    fPreviewCardView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    fPreviewCardView->AddChild(emptyPreviewContainer);  // Card 0: empty message
    fPreviewCardView->AddChild(previewContentGroup);     // Card 1: preview content
    fPreviewCardView->CardLayout()->SetVisibleItem((int32)0);  // Start with empty message
    
    // Create attachment strip (initially hidden)
    fAttachmentStrip = new AttachmentStripView();
    
    // Create preview container (attachment strip + preview card view)
    BGroupView* previewContainer = new BGroupView(B_VERTICAL, 0);
    BLayoutBuilder::Group<>(previewContainer, B_VERTICAL, 0)
        .Add(fAttachmentStrip, 0.0f)
        .Add(fPreviewCardView, 1.0f)
        .SetInsets(0);
    
    // Create search bar with attribute dropdown and clear/add query/backup buttons
    fSearchField = new SearchBarView(new BMessage(MSG_SEARCH_MODIFIED),
                                     new BMessage(MSG_SEARCH_CLEAR),
                                     new BMessage(MSG_SEARCH_ADD_QUERY),
                                     new BMessage(MSG_BACKUP_EMAILS));
    fSearchField->SetExplicitMinSize(BSize(B_SIZE_UNSET, 26));
    fIsSearchActive = false;
    
    // Create search bar group
    BGroupView* searchGroup = new BGroupView(B_HORIZONTAL, 2);
    BLayoutBuilder::Group<>(searchGroup)
        .Add(fSearchField)
        .SetInsets(2, 6, 2, 2);
    
    // Create time range slider with label
    fTimeRangeLabel = new BStringView("timeRangeLabel", B_TRANSLATE("Time range:"));
    fTimeRangeSlider = new TimeRangeSlider("timeRangeSlider", new BMessage(MSG_TIME_RANGE_CHANGED));
    
    // Set size constraints for time range row
    fTimeRangeSlider->SetExplicitMinSize(BSize(150, B_SIZE_UNSET));
    fTimeRangeLabel->SetAlignment(B_ALIGN_RIGHT);   // Align towards slider
    
    // Get the label area height from the slider to align side labels with the track
    // We need to calculate this based on font metrics since slider isn't attached yet
    font_height fh;
    fTimeRangeLabel->GetFontHeight(&fh);
    float labelOffset = ceilf(fh.ascent + fh.descent) + 2.0f;  // Match kLabelGap
    
    // Create time range group with label aligned to slider track (not the labels above)
    fTimeRangeGroup = new BGroupView(B_HORIZONTAL, 0);
    BLayoutBuilder::Group<>(fTimeRangeGroup, B_HORIZONTAL, 0)
        .AddGroup(B_VERTICAL, 0)
            .AddStrut(labelOffset)
            .Add(fTimeRangeLabel)
        .End()
        .Add(fTimeRangeSlider, 1.0f)  // Slider takes available space
        .SetInsets(4, 2, 4, 6);
    
    // Apply initial visibility based on preference
    if (gReaderSettings != NULL && !gReaderSettings->ShowTimeRange()) {
        fTimeRangeGroup->Hide();
        fTimeRangeMenuItem->SetMarked(false);
    }
    
    // Create email list group (search bar + time range + list card view)
    BGroupView* emailListGroup = new BGroupView(B_VERTICAL, 0);
    BLayoutBuilder::Group<>(emailListGroup)
        .Add(searchGroup)
        .Add(fTimeRangeGroup)
        .Add(fEmailListCardView)
        .SetInsets(0);
    
    // Create vertical split view (email list group, preview container)
    fVerticalSplit = new BSplitView(B_VERTICAL, 0);
    fVerticalSplit->AddChild(emailListGroup);
    fVerticalSplit->AddChild(previewContainer);
    
    // Set initial weights: 50% for email list, 50% for preview
    fVerticalSplit->SetItemWeight(0, 0.5f, false);
    fVerticalSplit->SetItemWeight(1, 0.5f, false);
    
    // Create horizontal split view (query list + trash, email list + preview)
    fHorizontalSplit = new BSplitView(B_HORIZONTAL, 0);
    fHorizontalSplit->AddChild(queryContainer);
    fHorizontalSplit->AddChild(fVerticalSplit);
    
    // Set initial weights: 20% for query list, 80% for email/preview
    fHorizontalSplit->SetItemWeight(0, 0.2f, false);
    fHorizontalSplit->SetItemWeight(1, 0.8f, false);
    
    // Set the main layout with menu bar and toolbar spanning full width.
    // Negative left/right insets (-1) merge view borders with the window frame
    // for a clean edge-to-edge appearance (no double border). Bottom stays 0
    // to preserve the preview pane's bottom border above the window edge.
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(fMenuBar)
        .Add(fToolBar)
        .Add(new BSeparatorView(B_HORIZONTAL, B_PLAIN_BORDER))
        .AddGroup(B_VERTICAL, 0)
            .Add(fHorizontalSplit)
            .SetInsets(-1, 0, -1, -1)
        .End()
    .End();
    
    // Add keyboard shortcut for time range slider toggle (Cmd+Shift+T)
    AddShortcut('T', B_COMMAND_KEY | B_SHIFT_KEY, new BMessage(MSG_TOGGLE_TIME_RANGE));
    AddShortcut('A', B_COMMAND_KEY, new BMessage(MSG_SELECT_ALL_EMAILS));
    AddShortcut('S', B_COMMAND_KEY, new BMessage(MSG_FOCUS_SEARCH));
    AddShortcut('Z', B_COMMAND_KEY, new BMessage(MSG_UNDO_DELETE));
    
    // Alt+Up/Down navigates the query sidebar regardless of focus
    AddCommonFilter(new QueryNavFilter(this));
    
    // Load volume selection BEFORE loading queries (queries need selected volumes)
    LoadVolumeSelection();
    BuildVolumeMenu();
    
    // Load spam sender blocklist
    LoadSpamBlocklist();
    
    // Load queries (populates the query list sidebar - fast)
    LoadQueries();
    
    // Set up node monitoring for the queries directory to detect changes
    BPath queriesPath;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &queriesPath) == B_OK) {
        queriesPath.Append("EmailViews/queries");
        create_directory(queriesPath.Path(), 0755);
        
        // Initialize the MEMBER variable, not a local one
        if (fQueriesDir.SetTo(queriesPath.Path()) == B_OK) {
            if (fQueriesDir.GetNodeRef(&fQueriesDirRef) == B_OK) {
                // Start watching using the persistent node_ref
                watch_node(&fQueriesDirRef, B_WATCH_DIRECTORY | B_WATCH_NAME | B_WATCH_ATTR, this);
            }
        }
    }
    
    // Load saved window state (frame, splits - needed before Show so window appears
    // in correct position/size, but does NOT execute queries)
    LoadWindowState();
    
    // Set minimum window size based on toolbar's preferred width
    float minWidth = fToolBar->PreferredSize().Width();
    if (minWidth < 400)
        minWidth = 400;  // Reasonable minimum fallback
    SetSizeLimits(minWidth, B_SIZE_UNLIMITED, 300, B_SIZE_UNLIMITED);
    
    // Defer all heavy initialization (view restoration, query execution,
    // background queries) until after the window is shown. This ensures:
    // 1. Window appears instantly at saved position/size
    // 2. Layout is valid (needed for split weights to work correctly)
    // 3. Query loading can show progress in the already-visible UI
    PostMessage(MSG_DEFERRED_INIT);
    PostMessage(MSG_INIT_PREVIEW_PANE);
}

EmailViewsWindow::~EmailViewsWindow()
{
    // Stop watching for Mail app quit
    be_roster->StopWatching(BMessenger(this));
    
    // Stop watching volume mount/unmount
    fVolumeRoster.StopWatching();
    
    // Stop watching trash directory if active
    if (fShowTrashOnly)
        watch_node(&fTrashDirRef, B_STOP_WATCHING, this);
    
    // Stop trash loader thread if running
    fTrashLoaderStop = true;
    
    // Stop any running query
    if (fEmailList)
        fEmailList->StopQuery();
    
    // Clean up background new mail queries
    for (int32 i = 0; i < fBackgroundNewMailQueries.CountItems(); i++) {
        BQuery* query = fBackgroundNewMailQueries.ItemAt(i);
        if (query)
            query->Clear();
    }
    fBackgroundNewMailQueries.MakeEmpty();  // Owning list deletes items
    
    if (fBackgroundQueryHandler) {
        RemoveHandler(fBackgroundQueryHandler);
        delete fBackgroundQueryHandler;
        fBackgroundQueryHandler = NULL;
    }
    
    free(fHtmlBodyContent);
    delete fQueryReloadRunner;
    delete fQueryCountRunner;
    
    // Stop query count background thread
    fQueryCountStop = true;
    if (fQueryCountThread >= 0) {
        status_t result;
        wait_for_thread(fQueryCountThread, &result);
    }
    
    delete fTimeRangeFilterRunner;
    delete fRestoreFolderPanel;
    
    // Clean up pending restore refs
    for (int32 i = 0; i < fPendingRestoreRefs.CountItems(); i++) {
        delete (entry_ref*)fPendingRestoreRefs.ItemAt(i);
    }
    
    stop_watching(this);
}

void EmailViewsWindow::LoadQueries()
{
    // Save current selection before clearing
    BString selectedPath;
    int32 selectedIndex = fQueryList->CurrentSelection();
    if (selectedIndex >= 0) {
        QueryItem* selectedItem = dynamic_cast<QueryItem*>(fQueryList->ItemAt(selectedIndex));
        if (selectedItem) {
            selectedPath = selectedItem->GetPath();
        }
    }
    
    // Invalidate fCurrentViewItem before clearing - it points to items we're about to delete
    fCurrentViewItem = NULL;
    
    // Clear existing queries
    while (fQueryList->CountItems() > 0) {
        delete fQueryList->RemoveItem(int32(0));
    }
    
    // Load query icon
    // Helper function to load HVIF icon from app resources
    auto LoadIconFromResource = [](const char* resourceName, int32 iconSize = 24) -> BBitmap* {
        BResources* resources = BApplication::AppResources();
        if (resources == NULL)
            return nullptr;
        
        size_t size;
        const void* data = resources->LoadResource('VICN', resourceName, &size);
        if (data == NULL || size == 0)
            return nullptr;
        
        BBitmap* icon = new BBitmap(BRect(0, 0, iconSize - 1, iconSize - 1), B_RGBA32);
        if (BIconUtils::GetVectorIcon((const uint8*)data, size, icon) == B_OK) {
            return icon;
        }
        
        delete icon;
        return nullptr;
    };
    
    float maxWidth = 0;
    BFont font;
    fQueryList->GetFont(&font);
    
    // Add query-based smart views (hardcoded defaults).
    // All built-in queries include BEOS:TYPE to work on volumes where not all
    // BFS indices exist. Trash exclusion can't be done in the predicate because
    // BFS queries don't support path-based exclusion — it's handled by the
    // loader thread's post-query filtering.
    // The "[ATTACHMENTS]" prefix on "With attachments" is a special flag that
    // tells StartQuery() to enable attachmentsOnly filtering in the loader.
    // Each QueryItem owns its icon bitmap (freed in destructor).
    
    // Derive icon sizes from the system font size via ComposeIconSize() so
    // the sidebar scales correctly on HiDPI displays and at large font sizes.
    // Built-in queries use the same icon size as the toolbar; custom queries
    // use a smaller size to preserve the visual hierarchy.
    const int32 kBuiltInIconSize = (int32)be_control_look->ComposeIconSize(31).width + 1;
    const int32 kCustomIconSize  = (int32)be_control_look->ComposeIconSize(15).width + 1;

    // Text offsets: left margin + icon + gap, all derived from the icon sizes above.
    const float kLeftMargin       = be_control_look->DefaultLabelSpacing() * 2;
    const float kIconTextGap      = be_control_look->DefaultLabelSpacing();
    const float kBuiltInTextOffset = kLeftMargin + kBuiltInIconSize + kIconTextGap;
    const float kCustomTextOffset  = kLeftMargin + kCustomIconSize  + kIconTextGap;
    
    fQueryList->AddItem(new QueryItem(B_TRANSLATE("All emails"), "((BEOS:TYPE==\"text/x-email\")&&(MAIL:subject=*))", LoadIconFromResource("MailQueryAllEmails", kBuiltInIconSize), true, false, kBuiltInIconSize));
    float allMailWidth = font.StringWidth(B_TRANSLATE("All emails")) + kBuiltInTextOffset;
    if (allMailWidth > maxWidth) maxWidth = allMailWidth;
    
    // Unread emails second (includes New and Seen statuses)
    fQueryList->AddItem(new QueryItem(B_TRANSLATE("Unread emails"), "(BEOS:TYPE==\"text/x-email\")&&((MAIL:status==New)||(MAIL:status==Seen))", LoadIconFromResource("MailQueryUnreadEmpty", kBuiltInIconSize), true, false, kBuiltInIconSize));
    // Account for count text like " (999)"
    float unreadMailWidth = font.StringWidth(B_TRANSLATE("Unread emails")) + font.StringWidth(" (999)") + kBuiltInTextOffset;
    if (unreadMailWidth > maxWidth) maxWidth = unreadMailWidth;
    
    // Sent emails third
    fQueryList->AddItem(new QueryItem(B_TRANSLATE("Sent emails"), "(BEOS:TYPE==\"text/x-email\")&&(MAIL:status==Sent)", LoadIconFromResource("MailQuerySent", kBuiltInIconSize), true, false, kBuiltInIconSize));
    float sentMailWidth = font.StringWidth(B_TRANSLATE("Sent emails")) + font.StringWidth(" (999)") + kBuiltInTextOffset;
    if (sentMailWidth > maxWidth) maxWidth = sentMailWidth;
    
    // Emails with attachments fourth
    fQueryList->AddItem(new QueryItem(B_TRANSLATE("With attachments"), "[ATTACHMENTS]((BEOS:TYPE==\"text/x-email\")&&(MAIL:subject=*))", LoadIconFromResource("MailQueryAttachments", kBuiltInIconSize), true, false, kBuiltInIconSize));
    float attachMailWidth = font.StringWidth(B_TRANSLATE("With attachments")) + font.StringWidth(" (999)") + kBuiltInTextOffset;
    if (attachMailWidth > maxWidth) maxWidth = attachMailWidth;
    
    // Draft emails fifth
    fQueryList->AddItem(new QueryItem(B_TRANSLATE("Draft emails"), "MAIL:draft==1", LoadIconFromResource("MailQueryDrafts", kBuiltInIconSize), true, false, kBuiltInIconSize));
    float draftMailWidth = font.StringWidth(B_TRANSLATE("Draft emails")) + font.StringWidth(" (999)") + kBuiltInTextOffset;
    if (draftMailWidth > maxWidth) maxWidth = draftMailWidth;
    
    // Starred emails sixth
    fQueryList->AddItem(new QueryItem(B_TRANSLATE("Starred emails"), "((BEOS:TYPE==\"text/x-email\")&&(MAIL:subject=*)&&(FILE:starred==1))", LoadIconFromResource("MailQueryStarred", kBuiltInIconSize), true, false, kBuiltInIconSize));
    float starMailWidth = font.StringWidth(B_TRANSLATE("Starred emails")) + font.StringWidth(" (999)") + kBuiltInTextOffset;
    if (starMailWidth > maxWidth) maxWidth = starMailWidth;
    
    // Spam emails — uses MAIL:classification attribute set by spamdbm
    fQueryList->AddItem(new QueryItem(B_TRANSLATE("Spam"), "[SPAM]((BEOS:TYPE==\"text/x-email\")&&(MAIL:subject=*))", LoadIconFromResource("MailQuerySpam", kBuiltInIconSize), true, false, kBuiltInIconSize));
    float spamMailWidth = font.StringWidth(B_TRANSLATE("Spam")) + font.StringWidth(" (999)") + kBuiltInTextOffset;
    if (spamMailWidth > maxWidth) maxWidth = spamMailWidth;
    
    // Load custom query files from disk
    BPath queriesPath;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &queriesPath) == B_OK) {
        queriesPath.Append("EmailViews/queries");
        
        // Ensure the directory exists
        BDirectory queriesDir;
        if (queriesDir.SetTo(queriesPath.Path()) != B_OK) {
            create_directory(queriesPath.Path(), 0755);
            queriesDir.SetTo(queriesPath.Path());
        }
        
        // Check if there are any query files
        bool hasCustomQueries = false;
        BEntry entry;
        queriesDir.Rewind();
        while (queriesDir.GetNextEntry(&entry) == B_OK) {
            if (entry.IsFile()) {
                hasCustomQueries = true;
                break;
            }
        }
        
        // Add separator if there are custom queries
        if (hasCustomQueries) {
            fQueryList->AddItem(new SeparatorItem());
        }
        
        // Load query files from the directory.
        // Custom queries are stored as standard Haiku query files: the
        // _trk/qrystr attribute holds the BFS predicate string. This is
        // the same format Tracker uses, so queries created in Tracker can
        // be dropped into this directory and vice versa.
        queriesDir.Rewind();
        while (queriesDir.GetNextEntry(&entry) == B_OK) {
            if (!entry.IsFile())
                continue;
            
            // Read the query string from the file's _trk/qrystr attribute
            BNode node(&entry);
            if (node.InitCheck() != B_OK)
                continue;
            
            // Read the predicate string as B_STRING_TYPE
            char queryBuffer[512];
            ssize_t size = node.ReadAttr("_trk/qrystr", B_STRING_TYPE, 0, queryBuffer, sizeof(queryBuffer) - 1);
            if (size <= 0)
                continue;
            
            queryBuffer[size] = '\0';
            
            // Get the entry name and full path
            char name[B_FILE_NAME_LENGTH];
            entry.GetName(name);
            
            BPath queryFilePath;
            entry.GetPath(&queryFilePath);
            
            // Load icon
            BBitmap* icon = LoadIconFromResource("MailQueryCustom", kCustomIconSize);
            
            // Add to query list with the FILE PATH (not the query string)
            // This allows us to open the file in Tracker later
            fQueryList->AddItem(new QueryItem(name, queryFilePath.Path(), icon, true, true, kCustomIconSize));
            
            // Calculate width
            float itemWidth = font.StringWidth(name) + font.StringWidth(" (999)") + kCustomTextOffset;
            if (itemWidth > maxWidth) maxWidth = itemWidth;
        }
    }
    
    // Set the query list width based on the longest name
    // Add padding for scrollbar and margins
    maxWidth += 40;
    
    // Set minimum width (but allow it to be resized by splitter)
    if (maxWidth < 150) maxWidth = 150;
    
    // Restore selection based on saved path
    if (selectedPath.Length() > 0) {
        for (int32 i = 0; i < fQueryList->CountItems(); i++) {
            QueryItem* item = dynamic_cast<QueryItem*>(fQueryList->ItemAt(i));
            if (item && selectedPath == item->GetPath()) {
                fQueryList->Select(i);
                break;
            }
        }
    }
    
    // Note: Query counts are updated asynchronously after window is shown
    // via ScheduleQueryCountUpdate() in MSG_DEFERRED_INIT
}




// Helper to convert search text to case-insensitive pattern for BFS queries.
// BFS query predicates don't have a case-insensitive flag, so we expand each
// letter into a character class: "test" → "[tT][eE][sS][tT]".
// Square brackets are special in BFS unquoted syntax, so literal [] in
// search text are replaced with ? (single-char wildcard).
static BString MakeCaseInsensitivePattern(const char* text)
{
    BString result;
    for (int i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        if (isalpha(c)) {
            result << '[' << (char)tolower(c) << (char)toupper(c) << ']';
        } else if (c == '[' || c == ']') {
            // These are pattern characters in unquoted query syntax;
            // replace with ? (single-character wildcard) to match literally
            result << '?';
        } else {
            result << c;
        }
    }
    return result;
}


void EmailViewsWindow::ExecuteQuery()
{
    // Delegate to ApplySearchFilter which builds the full query
    // (combining fBaseQuery with search text and time range filters)
    // and calls fEmailList->StartQuery()
    ApplySearchFilter();
}


void EmailViewsWindow::ResolveBaseQuery(QueryItem* item)
{
    // Resolve the query predicate from a QueryItem into fBaseQuery.
    // For custom queries, GetPath() returns a file path so we read
    // the actual query string from the file's _trk/qrystr attribute.
    // For built-in queries, GetPath() IS the query predicate.
    if (item == NULL)
        return;

    if (item->IsCustomQuery()) {
        BNode node(item->GetPath());
        if (node.InitCheck() == B_OK) {
            char queryBuffer[512];
            ssize_t size = node.ReadAttr("_trk/qrystr", B_STRING_TYPE, 0,
                queryBuffer, sizeof(queryBuffer) - 1);
            if (size > 0) {
                queryBuffer[size] = '\0';
                fBaseQuery = queryBuffer;
            }
        }
    } else {
        fBaseQuery = item->GetPath();
    }
}


// Trash loader thread data
struct TrashLoaderData {
    BMessenger messenger;
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<BVolume, true> volumes;
#else
    BObjectList<BVolume> volumes;
#endif
    volatile bool* stopFlag;
};


void EmailViewsWindow::LoadTrashEmails()
{
    // Clear current list and prepare for loading
    fEmailList->StopQuery();
    fEmailList->InvalidateQueryId();  // Discard any pending batches from old query
    fEmailList->MakeEmpty();
    
    fEmailList->BeginBulkLoad();
    
    // Prepare loader data
    TrashLoaderData* data = new TrashLoaderData();
    data->messenger = BMessenger(this);
    data->stopFlag = &fTrashLoaderStop;
    fTrashLoaderStop = false;
    
    for (int32 i = 0; i < fSelectedVolumes.CountItems(); i++) {
        BVolume* vol = fSelectedVolumes.ItemAt(i);
        if (vol != NULL)
            data->volumes.AddItem(new BVolume(*vol));
    }
    
    thread_id thread = spawn_thread(_TrashLoaderThread, "trash_loader",
                                     B_NORMAL_PRIORITY, data);
    if (thread >= 0) {
        resume_thread(thread);
    } else {
        delete data;
        fEmailList->EndBulkLoad();
    }
}


/*static*/ int32
EmailViewsWindow::_TrashLoaderThread(void* data)
{
    TrashLoaderData* loaderData = (TrashLoaderData*)data;
    BMessenger messenger = loaderData->messenger;
    volatile bool* stopFlag = loaderData->stopFlag;
    
    const int32 kBatchSize = 50;
    int32 totalLoaded = 0;
    
    // Collect trash directories to scan (including subdirectories)
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<BString, true> dirsToScan(20);
#else
    BObjectList<BString> dirsToScan(20);
#endif
    
    for (int32 v = 0; v < loaderData->volumes.CountItems(); v++) {
        if (*stopFlag)
            break;
        
        BVolume* volume = loaderData->volumes.ItemAt(v);
        if (volume == NULL)
            continue;
        
        // Get trash path for this volume
        BPath trashPath;
        if (find_directory(B_TRASH_DIRECTORY, &trashPath, false, volume) != B_OK)
            continue;
        
        dirsToScan.AddItem(new BString(trashPath.Path()));
    }
    
    BMessage batch(MSG_TRASH_BATCH);
    int32 batchCount = 0;
    
    // Process directories (new subdirectories may be added during iteration)
    for (int32 d = 0; d < dirsToScan.CountItems(); d++) {
        if (*stopFlag)
            break;
        
        BDirectory dir(dirsToScan.ItemAt(d)->String());
        if (dir.InitCheck() != B_OK)
            continue;
        
        BEntry entry;
        while (dir.GetNextEntry(&entry) == B_OK) {
            if (*stopFlag)
                break;
            
            // If it's a directory, add it to the scan list
            if (entry.IsDirectory()) {
                BPath subPath;
                if (entry.GetPath(&subPath) == B_OK)
                    dirsToScan.AddItem(new BString(subPath.Path()));
                continue;
            }
            
            if (!entry.IsFile())
                continue;
            
            // Check MIME type - only process emails and drafts
            BNode node(&entry);
            if (node.InitCheck() != B_OK)
                continue;
            
            char mimeType[256];
            BNodeInfo nodeInfo(&node);
            if (nodeInfo.GetType(mimeType) != B_OK ||
                (strcmp(mimeType, "text/x-email") != 0
                && strcmp(mimeType, "text/x-vnd.Be-MailDraft") != 0))
                continue;
            
            // Create EmailRef
            entry_ref ref;
            if (entry.GetRef(&ref) != B_OK)
                continue;
            
            EmailRef* emailRef = new EmailRef(ref);
            batch.AddPointer("emailref", emailRef);
            batchCount++;
            totalLoaded++;
            
            if (batchCount >= kBatchSize) {
                messenger.SendMessage(&batch);
                batch.MakeEmpty();
                batch.what = MSG_TRASH_BATCH;
                batchCount = 0;
            }
        }
    }
    
    // Send remaining batch
    if (batchCount > 0) {
        messenger.SendMessage(&batch);
    }
    
    // Send done message
    BMessage done(MSG_TRASH_LOAD_DONE);
    done.AddInt32("count", totalLoaded);
    messenger.SendMessage(&done);
    
    delete loaderData;
    return 0;
}


/*static*/ int32
EmailViewsWindow::_QueryCountThread(void* data)
{
    QueryCountData* qcd = (QueryCountData*)data;
    BMessenger messenger = qcd->messenger;
    volatile bool* stopFlag = qcd->stopFlag;
    
    int32 newCount = 0;
    int32 draftCount = 0;
    int32 trashCount = 0;
    int32 spamCount = 0;
    
    for (int32 v = 0; v < qcd->volumes.CountItems(); v++) {
        if (*stopFlag)
            break;
        
        BVolume* volume = qcd->volumes.ItemAt(v);
        if (volume == NULL)
            continue;
        
        // Get trash path for this volume
        BPath volumeTrashPath;
        BString trashLower;
        if (find_directory(B_TRASH_DIRECTORY, &volumeTrashPath, false, volume) == B_OK) {
            trashLower = volumeTrashPath.Path();
            trashLower.ToLower();
        }
        
        // Count unread emails (excluding trash and spam)
        BQuery newQuery;
        newQuery.SetVolume(volume);
        newQuery.SetPredicate("(BEOS:TYPE==\"text/x-email\")&&((MAIL:status==New)||(MAIL:status==Seen))");
        if (newQuery.Fetch() == B_OK)
            newCount += _CountWithExclusions(newQuery, trashLower, stopFlag);
        
        if (*stopFlag) break;
        
        // Count draft emails (excluding trash and spam)
        BQuery draftQuery;
        draftQuery.SetVolume(volume);
        draftQuery.SetPredicate("MAIL:draft==1");
        if (draftQuery.Fetch() == B_OK)
            draftCount += _CountWithExclusions(draftQuery, trashLower, stopFlag);
        
        if (*stopFlag) break;
        
        // Count unread spam emails (excluding trash)
        {
            BQuery spamQuery;
            spamQuery.SetVolume(volume);
            spamQuery.SetPredicate("(BEOS:TYPE==\"text/x-email\")&&(MAIL:classification==Spam)&&(MAIL:status==New)");
            if (spamQuery.Fetch() == B_OK) {
                entry_ref ref;
                while (spamQuery.GetNextRef(&ref) == B_OK) {
                    if (stopFlag && *stopFlag)
                        break;
                    // Exclude spam emails that are in trash
                    BPath path(&ref);
                    BString pathStr(path.Path());
                    pathStr.ToLower();
                    if (trashLower.Length() > 0
                        && pathStr.FindFirst(trashLower) >= 0)
                        continue;
                    spamCount++;
                }
            }
        }
        
        if (*stopFlag) break;
        
        // Count trash emails by scanning trash directory recursively.
        // We can't use a BQuery filtered to trash because BFS queries don't
        // support path constraints. Instead we walk the directory tree and
        // check MIME type on each file.
        if (volumeTrashPath.InitCheck() == B_OK) {
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
            BObjectList<BString, true> dirsToScan(10);
#else
            BObjectList<BString> dirsToScan(10);
#endif
            dirsToScan.AddItem(new BString(volumeTrashPath.Path()));
            
            for (int32 d = 0; d < dirsToScan.CountItems(); d++) {
                if (*stopFlag) break;
                
                BDirectory trashDir(dirsToScan.ItemAt(d)->String());
                if (trashDir.InitCheck() != B_OK)
                    continue;
                
                BEntry trashEntry;
                while (trashDir.GetNextEntry(&trashEntry) == B_OK) {
                    if (*stopFlag) break;
                    
                    if (trashEntry.IsDirectory()) {
                        BPath subPath;
                        if (trashEntry.GetPath(&subPath) == B_OK)
                            dirsToScan.AddItem(new BString(subPath.Path()));
                        continue;
                    }
                    if (!trashEntry.IsFile())
                        continue;
                    BNode trashNode(&trashEntry);
                    if (trashNode.InitCheck() != B_OK)
                        continue;
                    char mimeType[256];
                    BNodeInfo nodeInfo(&trashNode);
                    if (nodeInfo.GetType(mimeType) == B_OK &&
                        (strcmp(mimeType, "text/x-email") == 0
                        || strcmp(mimeType, "text/x-vnd.Be-MailDraft") == 0)) {
                        trashCount++;
                    }
                }
            }
        }
    }
    
    if (*stopFlag) {
        delete qcd;
        return 0;
    }
    
    // Count custom query unread emails
    BMessage result(MSG_QUERY_COUNTS_READY);
    result.AddInt32("generation", qcd->generation);
    result.AddInt32("new_count", newCount);
    result.AddInt32("draft_count", draftCount);
    result.AddInt32("trash_count", trashCount);
    result.AddInt32("spam_count", spamCount);
    result.AddBool("show_trash_only", qcd->showTrashOnly);
    result.AddInt32("list_count", qcd->listCount);
    
    for (int32 c = 0; c < qcd->customQueries.CountItems(); c++) {
        if (*stopFlag) break;
        
        QueryCountCustomQuery* cq = qcd->customQueries.ItemAt(c);
        if (cq == NULL || cq->predicate.IsEmpty())
            continue;
        
        BString countPredicate;
        countPredicate << "(" << cq->predicate << ")&&((MAIL:status==\"New\")||(MAIL:status==\"Seen\"))";
        
        int32 customNewCount = 0;
        for (int32 v = 0; v < qcd->volumes.CountItems(); v++) {
            if (*stopFlag) break;
            
            BVolume* volume = qcd->volumes.ItemAt(v);
            if (volume == NULL)
                continue;
            
            BPath volTrashPath;
            BString volTrashLower;
            if (find_directory(B_TRASH_DIRECTORY, &volTrashPath, false, volume) == B_OK) {
                volTrashLower = volTrashPath.Path();
                volTrashLower.ToLower();
            }
            
            BQuery countQuery;
            countQuery.SetVolume(volume);
            countQuery.SetPredicate(countPredicate.String());
            if (countQuery.Fetch() == B_OK)
                customNewCount += _CountWithExclusions(countQuery, volTrashLower, stopFlag);
        }
        
        result.AddString("custom_path", cq->path);
        result.AddString("custom_base_name", cq->baseName);
        result.AddInt32("custom_count", customNewCount);
    }
    
    if (!*stopFlag)
        messenger.SendMessage(&result);
    
    delete qcd;
    return 0;
}


void EmailViewsWindow::ApplySearchFilter()
{
    BString searchText = fSearchField->Text();
    searchText.Trim();
    
    // Check if this is a body or full-text search
    SearchAttribute attr = fSearchField->GetSearchAttribute();
    bool isBodySearch = (attr == SEARCH_BODY || attr == SEARCH_FULLTEXT);
    
    // Build search predicate based on selected attribute and match mode.
    // For body/full-text search, no BFS predicate is built — the search
    // runs as a second pass after the BFS query populates the list.
    BString searchPredicate;
    
    if (searchText.Length() > 0 && !isBodySearch) {
        bool matchesMode = fSearchField->IsMatchesMode();
        
        if (matchesMode) {
            // Exact match - based on Haiku Mail's _LaunchQuery implementation
            BString escapedText = searchText;
            escapedText.ReplaceAll(" ", "*");  // Query system needs * for spaces
            escapedText.ReplaceAll("\"", "\\\"");
            
            switch (attr) {
                case SEARCH_SUBJECT:
                    searchPredicate << "(MAIL:subject==\"*" << escapedText << "*\")";
                    break;
                case SEARCH_FROM:
                    searchPredicate << "(MAIL:from==\"*" << escapedText << "*\")";
                    break;
                case SEARCH_TO:
                    searchPredicate << "(MAIL:to==\"*" << escapedText << "*\")";
                    break;
                case SEARCH_ACCOUNT:
                    searchPredicate << "(MAIL:account==\"" << escapedText << "\")";
                    break;
                default:
                    searchPredicate << "(MAIL:subject==\"*" << escapedText << "*\")";
                    break;
            }
        } else {
            // Contains match - use case-insensitive wildcard pattern
            BString ciPattern = MakeCaseInsensitivePattern(searchText.String());
            
            switch (attr) {
                case SEARCH_SUBJECT:
                    searchPredicate << "(MAIL:subject=*" << ciPattern << "*)";
                    break;
                case SEARCH_FROM:
                    searchPredicate << "(MAIL:from=*" << ciPattern << "*)";
                    break;
                case SEARCH_TO:
                    searchPredicate << "(MAIL:to=*" << ciPattern << "*)";
                    break;
                case SEARCH_ACCOUNT:
                    searchPredicate << "(MAIL:account=*" << ciPattern << "*)";
                    break;
                default:
                    searchPredicate << "(MAIL:subject=*" << ciPattern << "*)";
                    break;
            }
        }
    }
    
    // Build time range predicate if not full range.
    // Skip time range for Draft and Unread views because:
    // - Draft emails lack MAIL:when (would be missed by time filter)
    // - Unread emails should always show ALL unread regardless of age
    BString timePredicate;
    bool isDraftView = (fBaseQuery == "MAIL:draft==1");
    bool isUnreadView = (fBaseQuery.FindFirst("MAIL:status==New") >= 0 && fBaseQuery.FindFirst("MAIL:status==Seen") >= 0);
    
    if (!isDraftView && !isUnreadView && !fShowTrashOnly && fTimeRangeSlider && !fTimeRangeSlider->IsFullRange()) {
        time_t fromTime = fTimeRangeSlider->FromTime();
        time_t toTime = fTimeRangeSlider->ToTime();
        
        // Determine which time attribute to use
        // For Draft emails, use file modification time (last_modified)
        // For regular emails, use MAIL:when
        const char* timeAttr = "MAIL:when";
        if (fBaseQuery.FindFirst("MAIL:draft==1") >= 0) {
            timeAttr = "last_modified";
        }
        
        // Left handle (fromTime) is the more recent limit
        // Right handle (toTime) is the older limit
        // We want emails WHERE: toTime <= timeAttr <= fromTime
        // But if left handle is at "Now" or "Today", don't constrain upper bound
        // so new emails can appear and all of today's emails are included
        if (fTimeRangeSlider->IsLeftAtTodayOrNow()) {
            // Only constrain the older limit
            if (toTime > 0) {
                timePredicate << "(" << timeAttr << ">=" << toTime << ")";
            }
        } else if (fromTime > 0 && toTime > 0) {
            timePredicate << "((" << timeAttr << ">=" << toTime << ")&&(" << timeAttr << "<=" << fromTime << "))";
        } else if (fromTime > 0) {
            timePredicate << "(" << timeAttr << "<=" << fromTime << ")";
        } else if (toTime > 0) {
            timePredicate << "(" << timeAttr << ">=" << toTime << ")";
        }
    }
    
    // Combine all predicates: base query && search && time range
    BString fullQuery;
    
    // Start with base query
    if (fBaseQuery.Length() > 0) {
        fullQuery = fBaseQuery;
    }
    
    // Add search predicate
    if (searchPredicate.Length() > 0) {
        if (fullQuery.Length() > 0) {
            fullQuery.Prepend("(");
            fullQuery << ")&&" << searchPredicate;
        } else {
            fullQuery = searchPredicate;
        }
    }
    
    // Add time predicate
    if (timePredicate.Length() > 0) {
        if (fullQuery.Length() > 0) {
            fullQuery.Prepend("(");
            fullQuery << ")&&" << timePredicate;
        } else {
            fullQuery = timePredicate;
        }
    }
    
    // If we have no query at all, use a default that matches all emails
    if (fullQuery.Length() == 0) {
        fullQuery = "((BEOS:TYPE==\"text/x-email\")&&(MAIL:subject=*))";
    }
    
    // Attachments filtering is handled by StartQuery's attachmentsOnly parameter
    
    // Determine if any filter is active
    fIsSearchActive = (searchText.Length() > 0) || 
                      (fTimeRangeSlider && !fTimeRangeSlider->IsFullRange());
    
    // Set up pending body search if needed. The body search will launch
    // automatically when the BFS query completes (in kMsgLoadingUpdate).
    if (isBodySearch && searchText.Length() > 0) {
        fPendingBodySearch = true;
        fPendingBodySearchText = searchText;
        fPendingBodySearchCaseSensitive = fSearchField->IsMatchesMode();
        fPendingBodySearchFullText = (attr == SEARCH_FULLTEXT);
    } else {
        fPendingBodySearch = false;
        fPendingBodySearchText.SetTo("");
    }
    
    // Clear existing emails - new items will appear immediately via two-phase loading
    fEmailList->Clear();
    
    // Show appropriate card during loading.
    // For body/full-text search, keep the empty card visible during the BFS
    // loading phase so the full list doesn't flash before being replaced.
    if (fEmailListCardView != NULL) {
        if (fPendingBodySearch) {
            ShowEmptyListMessage(B_TRANSLATE("Searching" B_UTF8_ELLIPSIS));
        } else {
            fEmailListCardView->CardLayout()->SetVisibleItem((int32)1);
        }
    }
    
    // Clear the preview pane when switching folders
    ClearPreviewPane();

    // Execute the query via the new EmailListView component
    fEmailList->SetSpamBlocklist(fSpamBlocklist);
    fEmailList->StartQuery(fullQuery.String(), &fSelectedVolumes, fShowTrashOnly, fShowSpamOnly, fAttachmentsOnly);
}

void EmailViewsWindow::ApplyTimeRangeFilter()
{
    // Time range changed - reapply all filters
    ApplySearchFilter();
}

void EmailViewsWindow::UpdateEmailCountLabel()
{
    if (fEmailList == NULL)
        return;
    
    int32 count = fEmailList->CountItems();
    BString label;
    
    if (count == 1) {
        label = B_TRANSLATE("1 email");
    } else {
        // Format count with thousand separator
        BString countStr;
        if (count >= 1000) {
            countStr.SetToFormat("%ld,%03ld", (long)(count / 1000),
                (long)(count % 1000));
        } else {
            countStr.SetToFormat("%ld", (long)count);
        }
        label.SetToFormat(B_TRANSLATE("%s emails"), countStr.String());
    }
    
    fEmailList->SetStatusText(label.String());
}













void EmailViewsWindow::AddCustomQuery(const char* name, const char* query, const char* menuSelection, const char* searchText, bool selectIt)
{
    // Ensure queries directory exists
    BPath queriesPath;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &queriesPath) != B_OK)
        return;
    queriesPath.Append("EmailViews");
    create_directory(queriesPath.Path(), 0755);
    queriesPath.Append("queries");
    create_directory(queriesPath.Path(), 0755);
    
    // Create the query file
    BPath queryPath(queriesPath);
    queryPath.Append(name);
    BFile file(queryPath.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (file.InitCheck() != B_OK)
        return;
    
    // Set MIME type so Tracker recognizes it as a query
    BNodeInfo nodeInfo(&file);
    nodeInfo.SetType("application/x-vnd.Be-query");
    
    // Write Tracker query attributes
    BNode node(queryPath.Path());
    if (node.InitCheck() != B_OK)
        return;
    
    // 1. The formula string (THIS is what shows in "by formula" mode)
    node.WriteAttr("_trk/qrystr", B_STRING_TYPE, 0, query, strlen(query) + 1);
    
    // 2. The UI State Message - THIS IS THE SECRET SAUCE!
    // This tells Tracker how to populate the attribute dropdowns
    BMessage uiState;
    uiState.AddString("menuSelection", menuSelection);       // "From", "To", "Account", etc.
    uiState.AddString("subMenuSelection", "contains");       // The operator
    uiState.AddString("attrViewText", searchText);          // The search text
    
    // Flatten and write the UI state message
    BMallocIO flatData;
    uiState.Flatten(&flatData);
    node.WriteAttr("_trk/qryinitattrs", B_MESSAGE_TYPE, 0, flatData.Buffer(), flatData.BufferLength());
    
    // 3. Other required UI hints
    BString emailLabel("E-mail");
    node.WriteAttrString("_trk/qryinitmime", &emailLabel);
    
    int32 numAttrs = 1;  // We have one attribute row in the UI
    node.WriteAttr("_trk/qryinitnumattrs", B_INT32_TYPE, 0, &numAttrs, sizeof(numAttrs));
    
    int32 initMode = 0x46627961;  // Tracker's magic constant (appears to mean "by attribute" mode)
    node.WriteAttr("_trk/qryinitmode", B_INT32_TYPE, 0, &initMode, sizeof(initMode));
    
    // Don't write volume attributes - let Tracker default to All disks
    
    node.Sync();
    
    // Reload query list
    LoadQueries();
    
    // Select the new query if requested
    if (selectIt) {
        for (int32 i = 0; i < fQueryList->CountItems(); i++) {
            BListItem* listItem = fQueryList->ItemAt(i);
            QueryItem* item = dynamic_cast<QueryItem*>(listItem);
            if (item && strcmp(item->Text(), name) == 0) {
                fQueryList->Select(i);
                fBaseQuery = query;
                ExecuteQuery();
                break;
            }
        }
    }
}
void EmailViewsWindow::RemoveCustomQuery(QueryItem* item)
{
    if (!item || !item->IsCustomQuery())
        return;
    
    // Get the leaf name before deleting
    BPath itemPath(item->GetPath());
    if (itemPath.InitCheck() != B_OK)
        return;
    const char* leaf = itemPath.Leaf();
    
    // Delete the query file
    BEntry entry(item->GetPath());
    if (entry.Exists())
        entry.Remove();
    
    // Remove the item from the sidebar
    _RemoveQueryItemByName(leaf);
}


void EmailViewsWindow::_RemoveQueryItemByName(const char* leafName)
{
    for (int32 i = 0; i < fQueryList->CountItems(); i++) {
        QueryItem* item = dynamic_cast<QueryItem*>(fQueryList->ItemAt(i));
        if (item == NULL || !item->IsCustomQuery())
            continue;
        BPath itemPath(item->GetPath());
        if (itemPath.InitCheck() != B_OK
            || strcmp(itemPath.Leaf(), leafName) != 0)
            continue;

        bool wasSelected = (fQueryList->CurrentSelection() == i);

        fQueryList->SetSelectionMessage(NULL);
        fQueryList->RemoveItem(i);
        fQueryList->SetSelectionMessage(new BMessage(MSG_QUERY_SELECTED));
        delete item;

        if (wasSelected && fQueryList->CountItems() > 0)
            fQueryList->Select(0);
        return;
    }
}

void EmailViewsWindow::ScheduleQueryCountUpdate()
{
    // Debounce query count updates - wait 300ms for activity to settle
    // Note: For external changes that we can't track incrementally, 
    // the counts may be slightly stale until next full reload
    delete fQueryCountRunner;
    BMessage msg(MSG_UPDATE_QUERY_COUNTS);
    fQueryCountRunner = new BMessageRunner(BMessenger(this), &msg, 300000, 1);
}


void EmailViewsWindow::RefreshQueryCountDisplay()
{
    // Signal any existing count thread to stop
    fQueryCountStop = true;
    
    // Increment generation — old thread results will be ignored
    fQueryCountGeneration++;
    fQueryCountStop = false;
    
    // Prepare thread data
    QueryCountData* qcd = new QueryCountData();
    qcd->messenger = BMessenger(this);
    qcd->stopFlag = &fQueryCountStop;
    qcd->showTrashOnly = fShowTrashOnly;
    qcd->listCount = fEmailList->CountItems();
    qcd->generation = fQueryCountGeneration;
    
    // Copy volumes
    for (int32 i = 0; i < fSelectedVolumes.CountItems(); i++) {
        BVolume* vol = fSelectedVolumes.ItemAt(i);
        if (vol != NULL)
            qcd->volumes.AddItem(new BVolume(*vol));
    }
    
    // Collect custom query info (read predicates on UI thread — these are small reads)
    for (int32 i = 0; i < fQueryList->CountItems(); i++) {
        BListItem* listItem = fQueryList->ItemAt(i);
        QueryItem* item = dynamic_cast<QueryItem*>(listItem);
        if (item && item->IsQuery() && item->IsCustomQuery()) {
            BNode node(item->GetPath());
            if (node.InitCheck() == B_OK) {
                BString predicate;
                if (node.ReadAttrString("_trk/qrystr", &predicate) == B_OK && !predicate.IsEmpty()) {
                    QueryCountCustomQuery* cq = new QueryCountCustomQuery();
                    cq->path = item->GetPath();
                    cq->predicate = predicate;
                    cq->baseName = item->GetBaseName();
                    qcd->customQueries.AddItem(cq);
                }
            }
        }
    }
    
    // Spawn background thread
    fQueryCountThread = spawn_thread(_QueryCountThread, "query_count",
                                      B_LOW_PRIORITY, qcd);
    if (fQueryCountThread >= 0) {
        resume_thread(fQueryCountThread);
    } else {
        delete qcd;
    }
}

void EmailViewsWindow::UpdateQueryCounts()
{
    RefreshQueryCountDisplay();
}


void EmailViewsWindow::OpenEmailInViewer(const char* emailPath)
{
    BEntry entry(emailPath);
    entry_ref ref;
    if (entry.GetRef(&ref) != B_OK)
        return;
    
    // Check if this email is already open in a reader window
    EmailReaderWindow* existingWindow = gReaderSettings->FindWindow(ref);
    if (existingWindow != NULL) {
        // Bring existing window to front
        existingWindow->Activate();
        return;
    }
    
    // Check if this is a draft email (by MIME type or attribute)
    BFile file(&ref, B_READ_ONLY);
    bool openAsDraft = false;
    
    // Check MIME type first
    char mimeType[256];
    BNodeInfo fileInfo(&file);
    if (fileInfo.GetType(mimeType) == B_OK) {
        if (strcmp(mimeType, "text/x-vnd.Be-MailDraft") == 0) {
            openAsDraft = true;
        }
    }
    
    // Also check MAIL:draft attribute
    if (!openAsDraft) {
        int32 isDraft = 0;
        if (file.ReadAttr("MAIL:draft", B_INT32_TYPE, 0, &isDraft, sizeof(isDraft)) == sizeof(isDraft) && isDraft == 1) {
            openAsDraft = true;
        }
    }
    
    // Calculate window frame with cascading
    BRect frame = gReaderSettings->NewWindowFrame();
    
    if (openAsDraft) {
        // Open draft in compose mode - create window WITHOUT ref so fIncoming = false
        EmailReaderWindow* composeWindow = new EmailReaderWindow(
            frame,
            "Composer - Draft",
            NULL,     // No ref - creates compose window
            NULL,     // to
            &gReaderSettings->ContentFont(),
            false,    // resending
            this      // emailViewsWindow
        );
        // Now tell it to open the draft file
        composeWindow->OpenMessage(&ref, B_MAIL_UTF8_CONVERSION);
        composeWindow->MoveOnScreen();
        composeWindow->Show();
    } else {
        // Open regular email in reader mode
        EmailReaderWindow* readerWindow = new EmailReaderWindow(
            frame,
            "Viewer",  // Title will be set by SetTitleForMessage
            &ref,
            NULL,     // to - not composing
            &gReaderSettings->ContentFont(),
            false,    // resending
            this      // direct pointer for navigation
        );
        readerWindow->MoveOnScreen();
        readerWindow->Show();
    }
}

void EmailViewsWindow::DeleteEmail(const char* emailPath)
{
    // Find the row in the list before moving the file
    EmailItem* rowToDelete = NULL;
    BString oldStatus;
    for (int32 i = 0; i < fEmailList->CountItems(); i++) {
        EmailItem* row = fEmailList->ItemAt(i);
        if (row && strcmp(row->GetPath(), emailPath) == 0) {
            rowToDelete = row;
            oldStatus = row->GetStatus();
            break;
        }
    }
    
    // Move the file to Trash
    BEntry entry(emailPath);
    if (entry.InitCheck() == B_OK) {
        // Get the volume this email is on to find the correct trash
        BVolume entryVolume;
        entry.GetVolume(&entryVolume);
        
        BPath trashPath;
        if (find_directory(B_TRASH_DIRECTORY, &trashPath, true, &entryVolume) == B_OK) {
            BDirectory trashDir(trashPath.Path());
            if (trashDir.InitCheck() == B_OK) {
                // Store original path so Tracker can restore it
                BNode node(&entry);
                if (node.InitCheck() == B_OK) {
                    node.WriteAttr("_trk/original_path", B_STRING_TYPE, 0, 
                        emailPath, strlen(emailPath) + 1);
                }
                
                entry.MoveTo(&trashDir);
            }
        }
    }
    
    // Remove the row from the list
    if (rowToDelete) {
        fEmailList->RemoveRow(rowToDelete);
    }
    
    // Update email count display
    UpdateEmailCountLabel();
    
    // Refresh display - will query fresh counts
    RefreshQueryCountDisplay();
    
    // If list is now empty, show appropriate message
    if (fEmailList->CountItems() == 0) {
        if (fShowTrashOnly)
            ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
        else if (fIsSearchActive)
            ShowEmptyListMessage(B_TRANSLATE("No emails match your search."));
        else
            ShowEmptyListMessage(B_TRANSLATE("No emails found."));
    }
}

void EmailViewsWindow::MarkEmailAsRead(const char* emailPath, bool read)
{
    BNode node(emailPath);
    if (node.InitCheck() == B_OK) {
        // Read current status
        char oldStatus[32] = "";
        node.ReadAttr("MAIL:status", B_STRING_TYPE, 0, oldStatus, sizeof(oldStatus) - 1);
        
        if (read) {
            // Only change to "Read" if currently "New"
            // Other statuses (Replied, Forwarded, Sent, etc.) are already read
            if (strcmp(oldStatus, "New") == 0) {
                node.WriteAttr("MAIL:status", B_STRING_TYPE, 0, "Read", 5);
                int32 readFlag = B_READ;
                node.WriteAttr("MAIL:read", B_INT32_TYPE, 0, &readFlag, sizeof(readFlag));
            }
        } else {
            // Mark as unread - only change emails with "Read" status.
            // Other statuses (Sent, Replied, Forwarded, etc.) must be left untouched.
            if (strcmp(oldStatus, "Read") == 0) {
                node.WriteAttr("MAIL:status", B_STRING_TYPE, 0, "New", 4);
                int32 readFlag = B_UNREAD;
                node.WriteAttr("MAIL:read", B_INT32_TYPE, 0, &readFlag, sizeof(readFlag));
            }
        }
    }
}


void EmailViewsWindow::ComposeResponse(int32 opCode)
{
    // Iterate over all selected emails and open a compose window for each
    EmailItem* row = NULL;
    while ((row = fEmailList->CurrentSelection(row)) != NULL) {
        entry_ref ref;
        BEntry entry(row->GetPath(), true);
        if (entry.GetRef(&ref) != B_OK)
            continue;
        
        // Create compose window directly
        BRect frame = gReaderSettings->NewWindowFrame();
        EmailReaderWindow* composeWindow = new EmailReaderWindow(
            frame,
            "Composer",
            NULL,     // ref - NULL for compose window
            "",       // to
            &gReaderSettings->ContentFont(),
            false,    // resending
            this      // emailViewsWindow
        );
        
        // Call the appropriate method synchronously
        switch (opCode) {
            case OP_REPLY:
                composeWindow->ComposeReplyTo(&ref, M_REPLY);
                break;
            case OP_REPLY_ALL:
                composeWindow->ComposeReplyTo(&ref, M_REPLY_ALL);
                break;
            case OP_FORWARD:
                composeWindow->ComposeForwardOf(&ref, true);
                break;
        }
        
        composeWindow->MoveOnScreen();
        composeWindow->Show();
    }
}

void EmailViewsWindow::ClearPreviewPane()
{
    // Clear attachment strip
    if (fAttachmentStrip)
        fAttachmentStrip->ClearAttachments();
    
    // Disable navigation buttons and menu items (no selection)
    _UpdateNavigationButtons();
    _DisableToolBarIcons();
    _UpdateMenuItemStates(false);

    BMenuItem* item = fMenuBar->FindItem(MSG_DELETE_EMAIL);
    if (item != NULL)
        item->SetEnabled(false);

    fMarkSpamMenuItem->SetEnabled(false);
    fMarkReadMenuItem->SetEnabled(false);
    fMarkUnreadMenuItem->SetEnabled(false);

 // Show the empty preview message with default message
    ShowEmptyPreviewMessage(B_TRANSLATE("Select an email to preview its contents."));
}

void EmailViewsWindow::_UpdateNavigationButtons()
{
    int32 index = fEmailList->CurrentSelectionIndex();
    bool hasSelection = (index >= 0);
    bool hasNext = hasSelection && (index + 1 < fEmailList->CountItems());
    bool hasPrev = hasSelection && (index > 0);

    fToolBar->SetActionEnabled(MSG_NEXT_EMAIL, hasNext);
    fToolBar->SetActionEnabled(MSG_PREV_EMAIL, hasPrev);
}

void EmailViewsWindow::_UpdateMenuItemStates(bool state)
{
    static const uint32 which[] {
        MSG_REPLY, MSG_REPLY_ALL, MSG_FORWARD,
        MSG_FILTER_EXACT_SENDER, MSG_FILTER_EXACT_RECIPIENT, MSG_FILTER_EXACT_SUBJECT, MSG_FILTER_EXACT_ACCOUNT,
        MSG_FILTER_BY_SENDER, MSG_FILTER_BY_RECIPIENT, MSG_FILTER_BY_ACCOUNT
    };

    for (int32 i = 0; i < sizeof(which) / sizeof(which[0]); i++) {
        BMenuItem* item = fMenuBar->FindItem(which[i]);
        if (item != NULL)
            item->SetEnabled(state);
    }
}

void EmailViewsWindow::_DisableToolBarIcons()
{
    fToolBar->SetActionEnabled(MSG_MARK_READ, false);
    fToolBar->SetActionEnabled(MSG_MARK_UNREAD, false);
    fToolBar->SetActionEnabled(MSG_REPLY, false);
    fToolBar->SetActionEnabled(MSG_FORWARD, false);
    fToolBar->SetActionEnabled(MSG_DELETE_EMAIL, false);
    fToolBar->SetActionEnabled(MSG_NEXT_EMAIL, false);
    fToolBar->SetActionEnabled(MSG_PREV_EMAIL, false);
}

void EmailViewsWindow::_UpdateToolBar()
{
    if (gReaderSettings == NULL || fToolBar == NULL)
        return;
    
    uint8 showToolBar = gReaderSettings->ShowToolBar();
    // Treat kHideToolBar (0) as kShowToolBar (show with labels) for backwards compatibility
    bool showLabel = (showToolBar == kShowToolBar || showToolBar == kHideToolBar);
    
    static const uint32 kLabeledCommands[] = {
        MSG_CHECK_EMAIL, MSG_NEW_EMAIL, MSG_REPLY, MSG_FORWARD,
        MSG_MARK_READ, MSG_MARK_UNREAD, MSG_DELETE_EMAIL,
        MSG_NEXT_EMAIL, MSG_PREV_EMAIL
    };
    for (uint32 i = 0; i < sizeof(kLabeledCommands) / sizeof(kLabeledCommands[0]); i++) {
        ToolBarButton* button = fToolBar->FindButton(kLabeledCommands[i]);
        if (button != NULL) {
            button->SetLabelVisible(showLabel);
            button->SetToolTip(showLabel ? NULL : button->Label());
        }
    }
    fToolBar->UpdateLayout();
}

void EmailViewsWindow::ShowEmptyListMessage(const char* message)
{
    if (fEmptyListLabel == NULL || fEmailListCardView == NULL)
        return;
    
    // Disable backup button when showing an empty view message
    // (skip blank loading transitions — empty string used while switching views)
    if (message != NULL && message[0] != '\0')
        fSearchField->SetViewHasContent(false);
    
    // Set the message text (only if provided)
    if (message != NULL) {
        fEmptyListLabel->SetText(message);
    }
    
    // Switch to the empty message card (card 0)
    fEmailListCardView->CardLayout()->SetVisibleItem((int32)0);
}

void EmailViewsWindow::ShowEmailListContent()
{
    if (fEmailListCardView == NULL)
        return;
    
    // Update backup button state (disabled in trash view)
    fSearchField->SetViewHasContent(fEmailList->CountItems() > 0 && !fShowTrashOnly);
    
    // If list is empty, show appropriate message instead
    if (fEmailList->CountItems() == 0) {
        if (fShowTrashOnly)
            ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
        else if (fIsSearchActive)
            ShowEmptyListMessage(B_TRANSLATE("No emails match your search."));
        else
            ShowEmptyListMessage(B_TRANSLATE("No emails found."));
        return;
    }
    
    // Switch to the email list card (card 1)
    fEmailListCardView->CardLayout()->SetVisibleItem((int32)1);
}

void EmailViewsWindow::ShowEmptyPreviewMessage(const char* message)
{
    if (fEmptyPreviewLabel == NULL || fPreviewCardView == NULL)
        return;
    
    // Set the message text (only if provided)
    if (message != NULL) {
        fEmptyPreviewLabel->SetText(message);
    }
    
    // Hide HTML button on Card 0 (fHtmlMessageButton is on Card 0)
    // Note: fHtmlVersionButton is on Card 1, so we don't hide it here
    // (hiding it while Card 1 is invisible causes Show() to not work properly later)
    if (fHtmlMessageButton != NULL)
        fHtmlMessageButton->Hide();
    
    // Clear stored HTML content
    free(fHtmlBodyContent);
    fHtmlBodyContent = NULL;
    fHtmlBodyContentSize = 0;
    
    // Switch to the empty message card (card 0)
    fPreviewCardView->CardLayout()->SetVisibleItem((int32)0);
    
    // Clear the preview pane content
    fPreviewPane->SetText("");
}

void EmailViewsWindow::ShowHtmlPreviewMessage(const void* htmlContent, size_t size)
{
    if (fEmptyPreviewLabel == NULL || fPreviewCardView == NULL)
        return;
    
    // Store the HTML content for later viewing (raw bytes)
    free(fHtmlBodyContent);
    fHtmlBodyContentSize = size;
    fHtmlBodyContent = malloc(size);
    if (fHtmlBodyContent != NULL) {
        memcpy(fHtmlBodyContent, htmlContent, size);
    } else {
        fHtmlBodyContentSize = 0;
    }
    
    // Set the message text
    fEmptyPreviewLabel->SetText(B_TRANSLATE(
        "This message is HTML formatted.\n"
        "Click below to open it in the default browser."));
    
    // Show HTML button (in empty card)
    if (fHtmlMessageButton != NULL && fHtmlMessageButton->IsHidden())
        fHtmlMessageButton->Show();
    
    // Hide HTML version button (in preview card)
    if (fHtmlVersionButton != NULL && !fHtmlVersionButton->IsHidden())
        fHtmlVersionButton->Hide();
    
    // Switch to the empty message card (card 0)
    fPreviewCardView->CardLayout()->SetVisibleItem((int32)0);
    
    // Clear the preview pane content
    fPreviewPane->SetText("");
    
    // Hide attachment strip since we're not using it for HTML chip anymore
    if (fAttachmentStrip != NULL && !fAttachmentStrip->IsHidden())
        fAttachmentStrip->Hide();
}

void EmailViewsWindow::ShowPreviewContent()
{
    if (fPreviewCardView == NULL)
        return;
    
    // Switch to the preview content card (card 1)
    fPreviewCardView->CardLayout()->SetVisibleItem((int32)1);
}

void EmailViewsWindow::DisplayEmailPreview(const char* emailPath)
{
    // Open the email file using Mail Kit
    BFile file(emailPath, B_READ_ONLY);
    if (file.InitCheck() != B_OK) {
        ShowEmptyPreviewMessage(B_TRANSLATE("Could not open email file."));
        return;
    }
    
    // Switch to preview content card since we're showing content
    ShowPreviewContent();
    
    // Check if this is a draft email
    int32 isDraft = 0;
    file.ReadAttr("MAIL:draft", B_INT32_TYPE, 0, &isDraft, sizeof(isDraft));
    
    if (isDraft == 1) {
        // Handle draft email - read attributes and plain text body
        BString preview;
        char buffer[512];
        ssize_t size;
        
        // Read To
        size = file.ReadAttr("MAIL:to", B_STRING_TYPE, 0, buffer, sizeof(buffer) - 1);
        if (size > 0) {
            buffer[size] = '\0';
            preview << B_TRANSLATE("To:") << " " << buffer << "\n";
        }
        
        // Read Subject
        size = file.ReadAttr("MAIL:subject", B_STRING_TYPE, 0, buffer, sizeof(buffer) - 1);
        if (size > 0) {
            buffer[size] = '\0';
            preview << B_TRANSLATE("Subject:") << " " << buffer << "\n";
        }
        
        // Read Account
        size = file.ReadAttr("MAIL:account", B_STRING_TYPE, 0, buffer, sizeof(buffer) - 1);
        if (size > 0) {
            buffer[size] = '\0';
            preview << B_TRANSLATE("Account:") << " " << buffer << "\n";
        }
        
        preview << "\n" << "────────────────────────────────────────" << "\n\n";
        
        // Read body as plain text from file
        off_t fileSize;
        file.GetSize(&fileSize);
        
        if (fileSize > 0) {
            // Limit to reasonable size
            if (fileSize > 50000) fileSize = 50000;
            
            char* bodyBuffer = new char[fileSize + 1];
            ssize_t bytesRead = file.ReadAt(0, bodyBuffer, fileSize);
            if (bytesRead > 0) {
                bodyBuffer[bytesRead] = '\0';
                preview << bodyBuffer;
                if (fileSize >= 50000) {
                    preview << "\n\n" << B_TRANSLATE("[Message truncated...]");
                }
            }
            delete[] bodyBuffer;
        } else {
            preview << B_TRANSLATE("[The draft has no body text to display]");
        }
        
        // Set the text in the preview pane
        fPreviewPane->SetInsets(4, 4, 4, 4);
        fPreviewPane->SetAlignment(B_ALIGN_LEFT);
        fPreviewPane->SetText(preview.String());
        
        // Apply font and text color to all text
        rgb_color textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
        fPreviewPane->SelectAll();
        fPreviewPane->SetFontAndColor(&gReaderSettings->ContentFont(), B_FONT_ALL, &textColor);
        fPreviewPane->Select(0, 0);
        
        fPreviewPane->ScrollTo(0, 0);
        
        // Update attachment strip - drafts typically don't have attachments displayed
        fAttachmentStrip->SetEmailPath(NULL);
        fAttachmentStrip->Hide();
        return;
    }
    
    // Parse the email using BEmailMessage with entry_ref for proper body parsing
    BEntry entry(emailPath);
    entry_ref ref;
    if (entry.GetRef(&ref) != B_OK) {
        ShowEmptyPreviewMessage(B_TRANSLATE("Could not get email reference."));
        return;
    }
    BEmailMessage email(&ref);
    
    // Build the preview text with headers
    BString preview;
    
    // Add header section with styling
    BString from = email.From();
    if (from.Length() > 0) {
        preview << "From: " << from << "\n";
    }
    
    BString to = email.To();
    if (to.Length() > 0) {
        preview << "To: " << to << "\n";
    }
    
    BString cc = email.CC();
    if (cc.Length() > 0) {
        preview << "CC: " << cc << "\n";
    }
    
    BString subject = email.Subject();
    if (subject.Length() > 0) {
        preview << "Subject: " << subject << "\n";
    }
    
    // Get the date from attributes (more reliable than parsing headers)
    time_t when = 0;
    BNode node(emailPath);
    if (node.InitCheck() == B_OK) {
        ssize_t size = node.ReadAttr("MAIL:when", B_TIME_TYPE, 0, &when, sizeof(when));
        if (size == sizeof(when) && when > 0) {
            BString formattedDate;
            BDateTimeFormat formatter;
            if (formatter.Format(formattedDate, when, 
                               B_FULL_DATE_FORMAT, B_SHORT_TIME_FORMAT) == B_OK) {
                preview << "Date: " << formattedDate << "\n";
            }
        }
    }
    
    // Add separator
    preview << "\n" << "────────────────────────────────────────" << "\n\n";
    
    // Get the body text and extract attachments
    // First, try to get plain text content
    BString bodyText;
    
    // Try to extract text content from the message
    BTextMailComponent* textComponent = NULL;
    
    // Get body component from BEmailMessage
    BMailComponent* body = email.Body();
    if (body) {
        textComponent = dynamic_cast<BTextMailComponent*>(body);
    }
    
    // Update attachment strip using Mail Kit for parsing
    // The strip will filter out HTML alternatives (unnamed text/html)
    if (fAttachmentStrip) {
        fAttachmentStrip->SetEmailPath(emailPath);
        // Create a new BEmailMessage for the attachment strip to own
        BEmailMessage* attachmentEmail = new BEmailMessage(&ref);
        fAttachmentStrip->SetAttachments(attachmentEmail);
    }
    
    // Now check if there are REAL attachments (after filtering HTML alternatives)
    bool hasRealAttachments = fAttachmentStrip && fAttachmentStrip->HasAttachments();
    
    // Update the EMAILVIEWS:attachment cache attribute if it's wrong.
    // This makes the cache self-correcting: if an email was indexed before
    // we improved HTML alternative filtering, previewing it fixes the cache.
    // The attribute is checked by EmailRef during list loading for O(1)
    // attachment detection without parsing the MIME tree.
    {
        BFile cacheFile(emailPath, B_READ_ONLY);
        if (cacheFile.InitCheck() == B_OK) {
            int8 cachedValue = -1;
            ssize_t attrSize = cacheFile.ReadAttr("EMAILVIEWS:attachment", B_INT8_TYPE, 0,
                                                  &cachedValue, sizeof(cachedValue));
            
            // Update cache if it doesn't exist or is wrong
            if (attrSize != sizeof(cachedValue) || cachedValue != (hasRealAttachments ? 1 : 0)) {
                cacheFile.Unset();
                
                BFile writeFile(emailPath, B_READ_WRITE);
                if (writeFile.InitCheck() == B_OK) {
                    int8 newValue = hasRealAttachments ? 1 : 0;
                    writeFile.WriteAttr("EMAILVIEWS:attachment", B_INT8_TYPE, 0,
                                        &newValue, sizeof(newValue));
                }
                
                // Also update the list view row if it exists
                // The live query will detect the attribute change and update automatically
                // but we invalidate immediately for responsiveness
                for (int32 i = 0; i < fEmailList->CountItems(); i++) {
                    EmailItem* row = fEmailList->ItemAt(i);
                    if (row && strcmp(row->GetPath(), emailPath) == 0) {
                        fEmailList->InvalidateItem(i);
                        break;
                    }
                }
            }
        }
    }
    
    // Show/hide attachment strip based on real attachments
    if (fAttachmentStrip) {
        if (hasRealAttachments) {
            if (fAttachmentStrip->IsHidden())
                fAttachmentStrip->Show();
        } else {
            if (!fAttachmentStrip->IsHidden())
                fAttachmentStrip->Hide();
        }
    }
    
    if (textComponent != NULL) {
        // Get the text content
        const char* text = textComponent->Text();
        if (text != NULL) {
            bodyText = text;
        }
    }
    
    // If we couldn't get text through components, try reading the raw file
    if (bodyText.Length() == 0) {
        // Fallback: read the file and try to find the body after headers
        file.Seek(0, SEEK_SET);
        off_t size;
        file.GetSize(&size);
        
        if (size > 0 && size < 1024 * 1024) {  // Limit to 1MB
            char* buffer = new char[size + 1];
            ssize_t bytesRead = file.Read(buffer, size);
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                
                // Find the blank line that separates headers from body
                const char* bodyStart = strstr(buffer, "\r\n\r\n");
                if (bodyStart == NULL) {
                    bodyStart = strstr(buffer, "\n\n");
                }
                
                if (bodyStart != NULL) {
                    // Skip the blank line(s)
                    while (*bodyStart == '\r' || *bodyStart == '\n') {
                        bodyStart++;
                    }
                    bodyText = bodyStart;
                }
            }
            delete[] buffer;
        }
    }
    
    // Check if body is HTML content
    bool isHtmlBody = false;
    if (bodyText.Length() > 0) {
        // Look for HTML indicators (case-insensitive)
        BString lowerBody = bodyText;
        lowerBody.ToLower();
        if (lowerBody.FindFirst("<!doctype html") >= 0 ||
            lowerBody.FindFirst("<html") >= 0 ||
            (lowerBody.FindFirst("<head") >= 0 && lowerBody.FindFirst("<body") >= 0)) {
            isHtmlBody = true;
        }
    }
    
    // For HTML body, extract raw content for browser viewing but still show in preview
    void* rawHtmlContent = NULL;
    size_t rawHtmlContentSize = 0;
    
    if (isHtmlBody) {
        // Try to extract raw HTML to preserve original encoding (e.g., ISO-2022-JP)
        if (fAttachmentStrip != NULL) {
            // First try multipart extraction (in case it's multipart with HTML body)
            bool gotRawHtml = fAttachmentStrip->ExtractRawHtmlFromEmail(emailPath, &rawHtmlContent, &rawHtmlContentSize);
            // If that fails, try simple body extraction (non-multipart HTML email)
            if (!gotRawHtml) {
                fAttachmentStrip->ExtractRawBodyFromEmail(emailPath, &rawHtmlContent, &rawHtmlContentSize);
            }
        }
    }
    
    // Add body to preview
    if (bodyText.Length() > 0) {
        // Limit preview length to avoid performance issues with huge emails
        if (bodyText.Length() > 50000) {
            bodyText.Truncate(50000);
            bodyText << "\n\n[Message truncated...]";
        }
        preview << bodyText;
    } else {
        preview << "[Could not extract message body]";
    }
    
    // Set the text in the preview pane
    fPreviewPane->SetInsets(4, 4, 4, 4);
    fPreviewPane->SetAlignment(B_ALIGN_LEFT);
    fPreviewPane->SetText(preview.String());
    
    // Apply font and text color to all text
    rgb_color textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
    fPreviewPane->SelectAll();
    fPreviewPane->SetFontAndColor(&gReaderSettings->ContentFont(), B_FONT_ALL, &textColor);
    fPreviewPane->Select(0, 0);
    
    // Scroll to top
    fPreviewPane->ScrollTo(0, 0);
    
    // Check if there's HTML content available for browser viewing
    // Either from HTML-only body or from HTML alternative in multipart
    if (rawHtmlContent != NULL && rawHtmlContentSize > 0) {
        // HTML-only email - use the extracted raw content
        free(fHtmlBodyContent);
        fHtmlBodyContent = rawHtmlContent;
        fHtmlBodyContentSize = rawHtmlContentSize;
        if (fHtmlVersionButton != NULL && fHtmlVersionButton->IsHidden()) {
            fHtmlVersionButton->Show();
        }
    } else if (fAttachmentStrip && fAttachmentStrip->HasHtmlAlternative()) {
        // Multipart email with HTML alternative
        free(fHtmlBodyContent);
        fHtmlBodyContentSize = fAttachmentStrip->HtmlAlternativeSize();
        fHtmlBodyContent = malloc(fHtmlBodyContentSize);
        if (fHtmlBodyContent != NULL) {
            memcpy(fHtmlBodyContent, fAttachmentStrip->HtmlAlternative(), fHtmlBodyContentSize);
        } else {
            fHtmlBodyContentSize = 0;
        }
        if (fHtmlVersionButton != NULL && fHtmlVersionButton->IsHidden()) {
            fHtmlVersionButton->Show();
        }
    } else {
        // No HTML content - hide the button
        if (fHtmlVersionButton != NULL && !fHtmlVersionButton->IsHidden())
            fHtmlVersionButton->Hide();
        free(fHtmlBodyContent);
        fHtmlBodyContent = NULL;
        fHtmlBodyContentSize = 0;
    }
}

void EmailViewsWindow::SaveWindowState()
{
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
        return;
    path.Append("EmailViews");
    create_directory(path.Path(), 0755);
    path.Append("emailviews_settings");
    
    // Load existing settings to preserve column prefs for other views
    BMessage settings;
    BFile file(path.Path(), B_READ_ONLY);
    if (file.InitCheck() == B_OK) {
        settings.Unflatten(&file);
    }
    file.Unset();
    
    // Update window frame
    settings.RemoveName("window_frame");
    BRect frame = Frame();
    settings.AddRect("window_frame", frame);
    
    // Update split view positions (as proportional weights, not pixels)
    if (fHorizontalSplit) {
        float weight0 = fHorizontalSplit->ItemWeight((int32)0);
        float weight1 = fHorizontalSplit->ItemWeight(1);
        float total = weight0 + weight1;
        float proportion = (total > 0) ? (weight0 / total) : 0.2f;
        settings.RemoveName("horizontal_split");
        settings.AddFloat("horizontal_split", proportion);
    }
    
    if (fVerticalSplit) {
        float weight0 = fVerticalSplit->ItemWeight((int32)0);
        float weight1 = fVerticalSplit->ItemWeight(1);
        float total = weight0 + weight1;
        float proportion = (total > 0) ? (weight0 / total) : 0.5f;
        settings.RemoveName("vertical_split");
        settings.AddFloat("vertical_split", proportion);
    }
    
    // Update Deskbar preference
    settings.RemoveName("show_in_deskbar");
    settings.AddBool("show_in_deskbar", fShowInDeskbar);
    
    // Update time range slider values
    if (fTimeRangeSlider) {
        settings.RemoveName("time_range_left");
        settings.RemoveName("time_range_right");
        settings.AddFloat("time_range_left", fTimeRangeSlider->LeftValue());
        settings.AddFloat("time_range_right", fTimeRangeSlider->RightValue());
    }
    
    // Save selected view (query list item or trash)
    settings.RemoveName("selected_view");
    settings.RemoveName("selected_view_is_trash");
    if (fShowTrashOnly) {
        settings.AddBool("selected_view_is_trash", true);
    } else if (fCurrentViewItem != NULL) {
        settings.AddString("selected_view", fCurrentViewItem->GetPath());
        settings.AddBool("selected_view_is_trash", false);
    }
    
    // Save to settings file
    file.SetTo(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (file.InitCheck() == B_OK) {
        settings.Flatten(&file);
    }
}

void EmailViewsWindow::LoadWindowState()
{
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK) {
        return;
    }
    
    path.Append("EmailViews/emailviews_settings");
    BFile file(path.Path(), B_READ_ONLY);
    if (file.InitCheck() != B_OK) {
        return;
    }
    
    BMessage settings;
    if (settings.Unflatten(&file) != B_OK) {
        return;
    }
    
    // Restore window frame
    BRect frame;
    if (settings.FindRect("window_frame", &frame) == B_OK) {
        MoveTo(frame.LeftTop());
        ResizeTo(frame.Width(), frame.Height());
    }
    
    // Restore split view positions
    float horizontalSplit;
    if (settings.FindFloat("horizontal_split", &horizontalSplit) == B_OK && fHorizontalSplit) {
        // Allow any valid proportion (just check for sane values)
        if (horizontalSplit >= 0.0f && horizontalSplit <= 1.0f) {
            fHorizontalSplit->SetItemWeight(0, horizontalSplit, true);
            fHorizontalSplit->SetItemWeight(1, 1.0f - horizontalSplit, true);
        }
    }
    
    float verticalSplit;
    if (settings.FindFloat("vertical_split", &verticalSplit) == B_OK && fVerticalSplit) {
        // Allow any valid proportion (just check for sane values)
        if (verticalSplit >= 0.0f && verticalSplit <= 1.0f) {
            fVerticalSplit->SetItemWeight(0, verticalSplit, true);
            fVerticalSplit->SetItemWeight(1, 1.0f - verticalSplit, true);
        }
    }
    
    // Restore time range slider values
    if (fTimeRangeSlider) {
        float leftValue, rightValue;
        if (settings.FindFloat("time_range_left", &leftValue) == B_OK &&
            settings.FindFloat("time_range_right", &rightValue) == B_OK) {
            // Validate values are in range
            if (leftValue >= 0.0f && leftValue <= 1.0f &&
                rightValue >= 0.0f && rightValue <= 1.0f &&
                leftValue <= rightValue) {
                fTimeRangeSlider->SetLeftValue(leftValue);
                fTimeRangeSlider->SetRightValue(rightValue);
                // Time range filter will be applied when deferred init executes the query
            }
        }
    }
    
    // Note: Column states are loaded per-view in LoadColumnPrefsForView()
    
    // Note: We don't need to restore temporary filters here because they're
    // already saved as query files in ~/config/settings/EmailViews/queries/
    // and LoadQueries() will load them automatically
    
    // Note: Deskbar preference is restored in MSG_DEFERRED_INIT
}

BString EmailViewsWindow::GetColumnPrefsKey(QueryItem* item)
{
    // Generate a unique key for built-in views
    if (!item)
        return "trash";  // Trash view
    
    if (item->IsCustomQuery()) {
        // Custom queries use file attributes, not settings file
        return "";
    }
    
    // For built-in queries and folders, use a sanitized path
    BString key(item->GetPath());
    key.ReplaceAll("/", "_");
    key.ReplaceAll(":", "_");
    key.ReplaceAll("=", "_");
    key.ReplaceAll("*", "_");
    key.Prepend("view_");
    return key;
}

void EmailViewsWindow::SaveColumnPrefsForView(QueryItem* item)
{
    if (!fEmailList)
        return;
    
    // Use new EmailListView's column state persistence
    BMessage prefs;
    fEmailList->SaveColumnState(&prefs);
    
    // For custom queries, save to file attribute
    if (item && item->IsCustomQuery()) {
        BNode node(item->GetPath());
        if (node.InitCheck() == B_OK) {
            ssize_t size = prefs.FlattenedSize();
            char* buffer = new char[size];
            if (prefs.Flatten(buffer, size) == B_OK) {
                node.WriteAttr("EmailViews:columns", B_MESSAGE_TYPE, 0, buffer, size);
            }
            delete[] buffer;
        }
        return;
    }
    
    // For built-in views, save to emailviews_settings file
    BString key = GetColumnPrefsKey(item);
    if (key.Length() == 0)
        return;
    
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
        return;
    path.Append("EmailViews");
    
    // Ensure directory exists
    create_directory(path.Path(), 0755);
    
    path.Append("emailviews_settings");
    
    // Load existing settings file
    BMessage settings;
    BFile file(path.Path(), B_READ_ONLY);
    if (file.InitCheck() == B_OK) {
        settings.Unflatten(&file);
    }
    file.Unset();
    
    // Get or create column_prefs sub-message
    BMessage columnPrefs;
    settings.FindMessage("column_prefs", &columnPrefs);
    
    // Update prefs for this view
    columnPrefs.RemoveName(key.String());
    columnPrefs.AddMessage(key.String(), &prefs);
    
    // Update column_prefs in settings
    settings.RemoveName("column_prefs");
    settings.AddMessage("column_prefs", &columnPrefs);
    
    // Save back
    file.SetTo(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (file.InitCheck() == B_OK) {
        settings.Flatten(&file);
    }
}

void EmailViewsWindow::LoadColumnPrefsForView(QueryItem* item)
{
    if (!fEmailList)
        return;
    
    BMessage prefs;
    bool loaded = false;
    
    // For custom queries, load from file attribute
    if (item && item->IsCustomQuery()) {
        BNode node(item->GetPath());
        if (node.InitCheck() == B_OK) {
            attr_info info;
            if (node.GetAttrInfo("EmailViews:columns", &info) == B_OK) {
                char* buffer = new char[info.size];
                if (node.ReadAttr("EmailViews:columns", B_MESSAGE_TYPE, 0, buffer, info.size) == info.size) {
                    if (prefs.Unflatten(buffer) == B_OK) {
                        loaded = true;
                    }
                }
                delete[] buffer;
            }
        }
    } else {
        // For built-in views, load from emailviews_settings file
        BString key = GetColumnPrefsKey(item);
        if (key.Length() > 0) {
            BPath path;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
                path.Append("EmailViews/emailviews_settings");
                BFile file(path.Path(), B_READ_ONLY);
                if (file.InitCheck() == B_OK) {
                    BMessage settings;
                    if (settings.Unflatten(&file) == B_OK) {
                        BMessage columnPrefs;
                        if (settings.FindMessage("column_prefs", &columnPrefs) == B_OK) {
                            if (columnPrefs.FindMessage(key.String(), &prefs) == B_OK) {
                                loaded = true;
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Apply prefs using new EmailListView's column state persistence
    if (loaded) {
        fEmailList->RestoreColumnState(&prefs);
    }
}


void EmailViewsWindow::MessageReceived(BMessage* message)
{
    // B_QUERY_UPDATE is now handled internally by EmailListView
    
    // Handle background query updates (triggers count refresh only)
    if (message->what == MSG_BACKGROUND_QUERY_UPDATE) {
        ScheduleQueryCountUpdate();
        return;
    }
    
    // Email attribute change monitoring is now handled internally by EmailListView.
    // Only directory monitoring (queries dir, trash dir) still comes to the window.
    
    // Handle Tracker scripting for Mail Next/Previous navigation
    if (message->what == B_GET_PROPERTY) {
        if (_HandleTrackerScripting(message))
            return;
    }
    
    // Handle selection updates from Mail
    if (message->what == B_SET_PROPERTY) {
        if (_HandleSetSelection(message))
            return;
    }
    switch (message->what) {
        case B_COLORS_UPDATED: {
            // System colors changed - update preview pane text color
            if (fPreviewPane != NULL) {
                rgb_color textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
                fPreviewPane->SelectAll();
                fPreviewPane->SetFontAndColor(NULL, B_FONT_ALL, &textColor);
                fPreviewPane->Select(0, 0);
                fPreviewPane->Invalidate();
            }
            // Theme colors are handled internally by EmailListView
            break;
        }
        
        case MSG_NAVIGATE_QUERY_LIST: {
            int32 direction;
            if (message->FindInt32("direction", &direction) == B_OK)
                _NavigateQueryList(direction);
            break;
        }
        
        case MSG_QUERY_SELECTED: {
            int32 selection = fQueryList->CurrentSelection();
            if (selection >= 0) {
                BListItem* listItem = fQueryList->ItemAt(selection);
                QueryItem* item = dynamic_cast<QueryItem*>(listItem);
                if (item) {
                    // Skip if same view is already selected (avoid unnecessary reload)
                    if (item == fCurrentViewItem && !fShowTrashOnly && !fShowSpamOnly)
                        break;
                    
                    // Save column prefs for current view before switching
                    SaveColumnPrefsForView(fCurrentViewItem);
                    
                    // Stop watching trash directory if we were viewing trash
                    if (fShowTrashOnly) {
                        watch_node(&fTrashDirRef, B_STOP_WATCHING, this);
                    }
                    
                    // Deselect trash item when folder is selected
                    fTrashItem->SetSelected(false);
                    fShowTrashOnly = false;
                    fShowSpamOnly = false;
                    fEmailList->SetShowingTrash(false);
                    fEmailList->SetShowingSpam(false);
                    
                    // Clear search field when switching folders (but keep time range)
                    fSearchField->SetText("");
                    fSearchField->SetHasResults(false);
                    fIsSearchActive = false;
                    
                    // Clear the preview pane when switching folders
                    ClearPreviewPane();

                    // Show blank card while loading (empty string preserves backup button state)
                    ShowEmptyListMessage("");
                    
                    // Load column prefs for new view
                    LoadColumnPrefsForView(item);
                    fCurrentViewItem = item;
                    
                    // Check for attachments-only view
                    fAttachmentsOnly = false;
                    fShowSpamOnly = false;
                    
                    if (item->IsQuery()) {
                        // For custom queries, GetPath() returns the file path
                        // We need to read the query string from the file's attribute
                        if (item->IsCustomQuery()) {
                            BNode node(item->GetPath());
                            if (node.InitCheck() == B_OK) {
                                char queryBuffer[512];
                                ssize_t size = node.ReadAttr("_trk/qrystr", B_STRING_TYPE, 0, queryBuffer, sizeof(queryBuffer) - 1);
                                if (size > 0) {
                                    queryBuffer[size] = '\0';
                                    fBaseQuery = queryBuffer;  // Store for search filtering
                                }
                            }
                        } else {
                            // For built-in queries, GetPath() returns the query string
                            BString queryPath = item->GetPath();
                            // Check for [ATTACHMENTS] prefix
                            if (queryPath.StartsWith("[ATTACHMENTS]")) {
                                fAttachmentsOnly = true;
                                queryPath.RemoveFirst("[ATTACHMENTS]");
                            }
                            // Check for [SPAM] prefix
                            if (queryPath.StartsWith("[SPAM]")) {
                                fShowSpamOnly = true;
                                queryPath.RemoveFirst("[SPAM]");
                            }
                            fBaseQuery = queryPath;  // Store for search filtering
                        }
                        
                        // Update time range slider visibility based on view type
                        if (fTimeRangeGroup) {
                            bool isDraftView = (fBaseQuery == "MAIL:draft==1");
                            bool isUnreadView = (fBaseQuery.FindFirst("MAIL:status==New") >= 0 
                                && fBaseQuery.FindFirst("MAIL:status==Seen") >= 0);
                            
                            if (isDraftView || isUnreadView) {
                                // Always hide for Draft and Unread views
                                if (!fTimeRangeGroup->IsHidden()) {
                                    fTimeRangeGroup->Hide();
                                    fTimeRangeMenuItem->SetMarked(false);
                                }
                           } else {
                                // Respect user preference for other views
                                bool shouldShow = (gReaderSettings == NULL || gReaderSettings->ShowTimeRange());
                                if (shouldShow && fTimeRangeGroup->IsHidden()) {
                                    fTimeRangeGroup->Show();
                                    fTimeRangeMenuItem->SetMarked(true);
                                } else if (!shouldShow && !fTimeRangeGroup->IsHidden()) {
                                    fTimeRangeGroup->Hide();
                                    fTimeRangeMenuItem->SetMarked(false);
                                }
                            }
                        }
                        
                        // Use ApplySearchFilter to include time range predicate
                        fEmailList->SetShowingSpam(fShowSpamOnly);
                        ApplySearchFilter();
                    }
                    
                    // Give the email list keyboard focus so Alt+A and keyboard
                    // navigation work immediately without requiring a click
                    fEmailList->MakeFocus(true);
                    
                    // Clear undo stack on view switch — restored emails would
                    // appear in a different view context
                    fUndoStack.clear();
                    fUndoMenuItem->SetEnabled(false);
                }
            }
            break;
        }
        
        case MSG_TRASH_SELECTED: {
            // Save column prefs for current view before switching
            SaveColumnPrefsForView(fCurrentViewItem);
            
            // Deselect folder list when trash is selected
            fQueryList->DeselectAll();
            fTrashItem->SetSelected(true);
            fShowTrashOnly = true;
            fShowSpamOnly = false;
            fEmailList->SetShowingTrash(true);
            fEmailList->SetShowingSpam(false);
            
            // Start watching trash directory for file additions
            watch_node(&fTrashDirRef, B_WATCH_DIRECTORY, this);
            
            // Clear search field when switching to trash
            fSearchField->SetText("");
            fSearchField->SetHasResults(false);
            fIsSearchActive = false;
            
            // Clear the preview pane
            ClearPreviewPane();
            
            // Disable backup in trash view
            fSearchField->SetViewHasContent(false);
            
            // Show empty list message for trash view (defensive - Trash item is only
            // Show blank card while loading (hides column headers from previous view)
            ShowEmptyListMessage("");
            
            // Load column prefs for trash view (NULL item = trash)
            LoadColumnPrefsForView(NULL);
            fCurrentViewItem = NULL;
            
            // Always hide time range slider for Trash view
            if (fTimeRangeGroup && !fTimeRangeGroup->IsHidden()) {
                fTimeRangeGroup->Hide();
            fTimeRangeMenuItem->SetMarked(false);
            }
            
            // Load trash emails by scanning trash directories directly
            LoadTrashEmails();
            
            // Give the email list keyboard focus so Alt+A and keyboard
            // navigation work immediately without requiring a click
            fEmailList->MakeFocus(true);
            
            // Clear undo stack — undo is not available in Trash view
            // (the user has direct restore functionality there)
            fUndoStack.clear();
            fUndoMenuItem->SetEnabled(false);
            break;
        }
        
        case MSG_EMAIL_SELECTED: {
            // Check status of all selected emails to determine button states
            int32 selectionCount = 0;
            message->FindInt32("count", &selectionCount);
            
            bool hasSelection = (selectionCount > 0);
            bool hasNew = false;     // Any email with "New" status (can be marked read)
            bool hasRead = false;    // Any email with "Read" status (can be marked unread)
            
            EmailItem* row = NULL;
            while ((row = fEmailList->CurrentSelection(row)) != NULL) {
                const char* status = row->GetStatus();
                if (strcmp(status, "New") == 0) {
                    hasNew = true;
                } else if (strcmp(status, "Read") == 0) {
                    hasRead = true;
                }
            }
            
            // "Mark as read" only enabled if there are unread (New/Seen) emails selected
            // (disabled in Trash view - marking status doesn't apply there)
            fToolBar->SetActionEnabled(MSG_MARK_READ, hasNew && !fShowTrashOnly);
            fMarkReadMenuItem->SetEnabled(hasNew && !fShowTrashOnly);
            // "Mark as unread" only enabled if there are Read emails selected
            fToolBar->SetActionEnabled(MSG_MARK_UNREAD, hasRead && !fShowTrashOnly);
            fMarkUnreadMenuItem->SetEnabled(hasRead && !fShowTrashOnly);
            // Reply/Forward enabled if any email is selected
            fToolBar->SetActionEnabled(MSG_REPLY, hasSelection);
            fToolBar->SetActionEnabled(MSG_FORWARD, hasSelection);
            fToolBar->SetActionEnabled(MSG_DELETE_EMAIL, hasSelection);
            // "Mark as spam" in normal views, "Not spam" in spam view
            fMarkSpamMenuItem->SetEnabled(hasSelection && !fShowTrashOnly && !fShowSpamOnly);
            fUnmarkSpamMenuItem->SetEnabled(hasSelection && fShowSpamOnly);
            // Next/Previous enabled based on position in list
            _UpdateNavigationButtons();
            // Filter and query and reply/forward menu items need a single email selected
            _UpdateMenuItemStates(selectionCount == 1);
            // Move to Trash works on multiple selected emails
            BMenuItem* item = fMenuBar->FindItem(MSG_DELETE_EMAIL);
            if (item != NULL)
                item->SetEnabled(hasSelection);

            // Show email list content if we have results
            ShowEmailListContent();
            
            // Display preview of selected email
            if (selectionCount == 1) {
                entry_ref ref;
                if (message->FindRef("ref", &ref) == B_OK) {
                    BEntry entry(&ref);
                    BPath path;
                    if (entry.GetPath(&path) == B_OK) {
                        DisplayEmailPreview(path.Path());
                    }
                }
            } else if (selectionCount > 1) {
                // Multiple emails selected - show message instead of preview
                if (fAttachmentStrip)
                    fAttachmentStrip->ClearAttachments();
                
                BString statusText;
                statusText.SetToFormat(B_TRANSLATE("%d emails selected."), selectionCount);
                ShowEmptyPreviewMessage(statusText.String());
            } else {
                ClearPreviewPane();
            }
            break;
        }
        
        case MSG_EMAIL_INVOKED: {
            entry_ref ref;
            int32 index = 0;
            while (message->FindRef("ref", index++, &ref) == B_OK) {
                BEntry entry(&ref);
                BPath path;
                if (entry.GetPath(&path) == B_OK) {
                    OpenEmailInViewer(path.Path());
                }
            }
            break;
        }
        
        case MSG_REPLY: {
            ComposeResponse(OP_REPLY);
            break;
        }
        
        case MSG_REPLY_ALL: {
            ComposeResponse(OP_REPLY_ALL);
            break;
        }
        
        case MSG_FORWARD: {
            ComposeResponse(OP_FORWARD);
            break;
        }
        
        case MSG_SEND_AS_ATTACHMENT: {
            // Collect refs for all selected emails
            BMessage refsMsg(REFS_RECEIVED);
            EmailItem* row = NULL;
            while ((row = fEmailList->CurrentSelection(row)) != NULL) {
                BEntry entry(row->GetPath());
                if (entry.InitCheck() == B_OK) {
                    entry_ref ref;
                    if (entry.GetRef(&ref) == B_OK) {
                        refsMsg.AddRef("refs", &ref);
                    }
                }
            }
            
            // Open a new compose window
            BMessage newMsg(M_NEW);
            be_app->PostMessage(&newMsg);
            
            // Give the window a moment to open, then send it the attachments
            // We need to find the new window and post the refs to it
            snooze(100000);  // 100ms
            
            // Find the most recently opened reader window in compose mode
            for (int32 i = be_app->CountWindows() - 1; i >= 0; i--) {
                BWindow* window = be_app->WindowAt(i);
                if (window != NULL) {
                    EmailReaderWindow* readerWin = dynamic_cast<EmailReaderWindow*>(window);
                    if (readerWin != NULL) {
                        // Post the refs to attach
                        readerWin->PostMessage(&refsMsg);
                        break;
                    }
                }
            }
            break;
        }
        
        case MSG_NEXT_EMAIL: {
            int32 index = fEmailList->CurrentSelectionIndex();
            if (index >= 0 && index + 1 < fEmailList->CountItems()) {
                fEmailList->Select(index + 1);
                fEmailList->ScrollToItem(index + 1);
            }
            break;
        }
        
        case MSG_PREV_EMAIL: {
            int32 index = fEmailList->CurrentSelectionIndex();
            if (index > 0) {
                fEmailList->Select(index - 1);
                fEmailList->ScrollToItem(index - 1);
            }
            break;
        }
        
        case MSG_SELECT_ALL_EMAILS: {
            // Ignore during loading to avoid freeze from mass selection
            // while the loader thread is still populating the list
            if (fEmailList->IsLoading())
                break;
            int32 count = fEmailList->CountItems();
            if (count > 0) {
                fEmailList->SelectRange(0, count - 1);
                
                // Update toolbar and menu item states for select-all.
                // With everything selected we assume a mixed set of statuses,
                // so both Mark Read and Mark Unread are enabled (their handlers
                // are already status-aware and won't touch Sent/Replied/etc.).
                // Reply and Forward are disabled — bulk compose is rarely
                // intentional and Alt+A is easy to hit accidentally.
                // Next/Previous are disabled — navigation is meaningless
                // when everything is selected.
                fToolBar->SetActionEnabled(MSG_DELETE_EMAIL, !fShowTrashOnly);
                fToolBar->SetActionEnabled(MSG_MARK_READ, !fShowTrashOnly);
                fToolBar->SetActionEnabled(MSG_MARK_UNREAD, !fShowTrashOnly);
                fMarkReadMenuItem->SetEnabled(!fShowTrashOnly);
                fMarkUnreadMenuItem->SetEnabled(!fShowTrashOnly);
                fToolBar->SetActionEnabled(MSG_REPLY, false);
                fToolBar->SetActionEnabled(MSG_FORWARD, false);
                fToolBar->SetActionEnabled(MSG_NEXT_EMAIL, false);
                fToolBar->SetActionEnabled(MSG_PREV_EMAIL, false);
            }
            break;
        }
        
        case MSG_FOCUS_SEARCH: {
            fSearchField->MakeFocus(true);
            break;
        }
        
        case MSG_UNDO_DELETE: {
            // Restore the most recently trashed batch of emails (Alt+Z).
            if (fUndoStack.empty() || fShowTrashOnly)
                break;
            
            // Pop the most recent delete operation
            std::vector<node_ref> nrefs = std::move(fUndoStack.back());
            fUndoStack.pop_back();
            fUndoMenuItem->SetEnabled(!fUndoStack.empty());
            
            // For each node_ref, find the file (now in Trash) and restore it
            // using its _trk/original_path attribute.
            for (const node_ref& nref : nrefs) {
                // Use the node_ref's device to find the correct trash directory
                // for that volume — avoids scanning the wrong volume's trash.
                BVolume vol(nref.device);
                BPath trashPath;
                if (vol.InitCheck() != B_OK
                    || find_directory(B_TRASH_DIRECTORY, &trashPath, false, &vol) != B_OK)
                    continue;
                
                BDirectory trashDir(trashPath.Path());
                BEntry trashEntry;
                bool found = false;
                while (trashDir.GetNextEntry(&trashEntry) == B_OK) {
                    node_ref entryNref;
                    trashEntry.GetNodeRef(&entryNref);
                    if (entryNref == nref) {
                        found = true;
                        break;
                    }
                }
                
                if (!found)
                    continue;
                
                // Read the original path attribute
                BNode trashNode(&trashEntry);
                char originalPath[B_PATH_NAME_LENGTH];
                ssize_t size = trashNode.ReadAttr("_trk/original_path", B_STRING_TYPE, 0,
                    originalPath, sizeof(originalPath) - 1);
                if (size <= 0)
                    continue;
                originalPath[size] = '\0';
                
                // Get the destination directory from the original path
                BPath origPath(originalPath);
                BPath parentPath;
                if (origPath.GetParent(&parentPath) != B_OK)
                    continue;
                
                // Ensure destination directory exists
                create_directory(parentPath.Path(), 0755);
                BDirectory destDir(parentPath.Path());
                if (destDir.InitCheck() != B_OK)
                    continue;
                
                // Move back to original location
                trashEntry.MoveTo(&destDir);
            }
            
            // Update counts after restore
            ScheduleQueryCountUpdate();
            break;
        }
        
        case MSG_DELETE_EMAIL: {
            // Get all selected rows
            BList selectedRows;
            EmailItem* row = NULL;
            for (int32 i = 0; (row = fEmailList->CurrentSelection(row)) != NULL; i++) {
                selectedRows.AddItem(row);
            }
            
            int32 deleteCount = selectedRows.CountItems();
            if (deleteCount == 0)
                break;
            
            // In Trash view: permanently delete with confirmation
            if (fShowTrashOnly) {
                BString confirmMsg;
                static BStringFormat format(B_TRANSLATE("{0, plural,"
                    "one{Permanently delete # email?\n\nThis cannot be undone.}"
                    "other{Permanently delete # emails?\n\nThis cannot be undone.}}"));
                format.Format(confirmMsg, deleteCount);
                
                BAlert* alert = new BAlert(B_TRANSLATE("Delete emails"),
                    confirmMsg.String(),
                    B_TRANSLATE("Cancel"), B_TRANSLATE("Delete"), NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
                alert->SetShortcut(0, B_ESCAPE);
                
                if (alert->Go() != 1) {
                    // User cancelled
                    break;
                }
                
                // Show "Deleting..." feedback immediately
                ShowEmptyListMessage(B_TRANSLATE("Deleting emails…"));
                
                // User confirmed - spawn background thread for deletion
                // Find bottommost selected index for post-delete selection
                int32 lastSelectedIndex = -1;
                for (int32 i = 0; i < selectedRows.CountItems(); i++) {
                    EmailItem* sel = (EmailItem*)selectedRows.ItemAt(i);
                    if (sel) {
                        int32 idx = fEmailList->IndexOf(sel);
                        if (idx > lastSelectedIndex)
                            lastSelectedIndex = idx;
                    }
                }
                
                BList* pathsToDelete = new BList();
                for (int32 i = 0; i < selectedRows.CountItems(); i++) {
                    EmailItem* selectedRow = (EmailItem*)selectedRows.ItemAt(i);
                    if (selectedRow) {
                        pathsToDelete->AddItem(new BString(selectedRow->GetPath()));
                    }
                }
                
                PermanentDeleteData* deleteData = new PermanentDeleteData();
                deleteData->emailPaths = pathsToDelete;
                deleteData->messenger = BMessenger(this);
                deleteData->firstSelectedIndex = lastSelectedIndex - (deleteCount - 1);
                
                thread_id thread = spawn_thread(PermanentDeleteThread,
                    "permanent_delete", B_NORMAL_PRIORITY, deleteData);
                if (thread >= 0) {
                    resume_thread(thread);
                } else {
                    for (int32 i = 0; i < pathsToDelete->CountItems(); i++) {
                        delete (BString*)pathsToDelete->ItemAt(i);
                    }
                    delete pathsToDelete;
                    delete deleteData;
                }
                // UI update happens in MSG_PERMANENT_DELETE_DONE
            } else {
                // Normal view: remove rows immediately, move files in background.
                // We can't rely on B_ENTRY_REMOVED from live queries during
                // loading (they may not be started yet), so we remove rows
                // ourselves. If a live query B_ENTRY_REMOVED arrives later,
                // RemoveEmail will be a no-op (not found in HashMap).
                
                // Confirm before moving a large number of emails to Trash
                if (deleteCount >= 50) {
                    BString confirmMsg;
                    static BStringFormat format(B_TRANSLATE("{0, plural,"
                        "one{Are you sure you want to move # email to Trash?}"
                        "other{Are you sure you want to move # emails to Trash?}}"));
                    format.Format(confirmMsg, deleteCount);

                    BAlert* alert = new BAlert(B_TRANSLATE("Move to Trash"),
                        confirmMsg.String(),
                        B_TRANSLATE("Cancel"), B_TRANSLATE("Move to Trash"), NULL,
                        B_WIDTH_AS_USUAL, B_WARNING_ALERT);
                    alert->SetShortcut(0, B_ESCAPE);
                    
                    if (alert->Go() != 1)
                        break;
                }
                
                // Remember index of first selected row for auto-select after removal
                int32 firstIndex = -1;
                if (deleteCount == 1) {
                    EmailItem* first = (EmailItem*)selectedRows.ItemAt(0);
                    if (first)
                        firstIndex = fEmailList->IndexOf(first);
                }
                
                BList* pathsToTrash = new BList();
                std::vector<node_ref> undoEntry;  // Captured before row removal
                for (int32 i = 0; i < selectedRows.CountItems(); i++) {
                    EmailItem* selectedRow = (EmailItem*)selectedRows.ItemAt(i);
                    if (selectedRow) {
                        pathsToTrash->AddItem(new BString(selectedRow->GetPath()));
                        // Capture node_ref now — EmailItem is deleted by RemoveRow() below
                        const node_ref* nref = selectedRow->GetNodeRef();
                        if (nref)
                            undoEntry.push_back(*nref);
                    }
                }
                
                // Remove rows from list immediately
                fEmailList->DeselectAll();
                fEmailList->BeginBatchRemove();
                for (int32 i = 0; i < selectedRows.CountItems(); i++) {
                    EmailItem* selectedRow = (EmailItem*)selectedRows.ItemAt(i);
                    if (selectedRow)
                        fEmailList->RemoveRow(selectedRow);
                }
                fEmailList->EndBatchRemove();
                
                // Auto-select next email after single deletion
                if (deleteCount == 1 && firstIndex >= 0
                    && fEmailList->CountItems() > 0) {
                    if (firstIndex >= fEmailList->CountItems())
                        firstIndex = fEmailList->CountItems() - 1;
                    fEmailList->Select(firstIndex);
                    fEmailList->ScrollToItem(firstIndex);
                }
                
                // Spawn background thread for file move
                MoveToTrashData* trashData = new MoveToTrashData();
                trashData->emailPaths = pathsToTrash;
                trashData->messenger = BMessenger(this);
                
                // Push pre-captured node_refs onto undo stack so Alt+Z can restore them.
                // node_refs were captured above before RemoveRow() deleted the EmailItems.
                // At undo time we use the node_ref to find the file in Trash and
                // restore it via its _trk/original_path attribute.
                // Cap at 10 levels — drop the oldest entry if full.
                if (!undoEntry.empty()) {
                    if (fUndoStack.size() >= 10)
                        fUndoStack.erase(fUndoStack.begin());
                    fUndoStack.push_back(std::move(undoEntry));
                    fUndoMenuItem->SetEnabled(true);
                }
                
                thread_id thread = spawn_thread(MoveToTrashThread,
                    "move_to_trash", B_NORMAL_PRIORITY, trashData);
                if (thread >= 0) {
                    resume_thread(thread);
                } else {
                    for (int32 i = 0; i < pathsToTrash->CountItems(); i++)
                        delete (BString*)pathsToTrash->ItemAt(i);
                    delete pathsToTrash;
                    delete trashData;
                }
            }
            break;
        }
        
        case MSG_RESTORE_EMAIL: {
            // Get index of first (topmost) selected row for repositioning after restore
            int32 firstSelectedIndex = -1;
            EmailItem* firstRow = fEmailList->CurrentSelection();
            if (firstRow) {
                firstSelectedIndex = fEmailList->IndexOf(firstRow);
            }
            
            // Find the actual minimum index among all selected rows
            EmailItem* row = NULL;
            for (int32 i = 0; (row = fEmailList->CurrentSelection(row)) != NULL; i++) {
                int32 idx = fEmailList->IndexOf(row);
                if (firstSelectedIndex == -1 || idx < firstSelectedIndex) {
                    firstSelectedIndex = idx;
                }
            }
            
            // Get all selected rows and collect their entry_refs
            BList* emailRefs = new BList();
            row = NULL;
            for (int32 i = 0; (row = fEmailList->CurrentSelection(row)) != NULL; i++) {
                emailRefs->AddItem(new entry_ref(row->GetEntryRef()));
            }
            
            if (emailRefs->CountItems() == 0) {
                delete emailRefs;
                break;
            }
            
            // Clear any pending restore refs from previous operation
            for (int32 i = 0; i < fPendingRestoreRefs.CountItems(); i++) {
                delete (entry_ref*)fPendingRestoreRefs.ItemAt(i);
            }
            fPendingRestoreRefs.MakeEmpty();
            
            // Copy account map for thread use
            std::map<int32, BString>* accountMapCopy = new std::map<int32, BString>(EmailAccountMap::Instance().GetMap());
            
            // Spawn background thread to do actual restore
            // List stays visible - rows will be removed when thread completes
            RestoreThreadData* restoreData = new RestoreThreadData();
            restoreData->emailRefs = emailRefs;
            restoreData->messenger = BMessenger(this);
            restoreData->firstSelectedIndex = firstSelectedIndex;
            restoreData->accountMap = accountMapCopy;
            
            thread_id thread = spawn_thread(RestoreThread,
                "restore_emails", B_NORMAL_PRIORITY, restoreData);
            if (thread >= 0) {
                resume_thread(thread);
            } else {
                // Thread creation failed - clean up
                for (int32 i = 0; i < emailRefs->CountItems(); i++) {
                    delete (entry_ref*)emailRefs->ItemAt(i);
                }
                delete emailRefs;
                delete accountMapCopy;
                delete restoreData;
            }
            // UI update happens in MSG_RESTORE_DONE
            break;
        }
        
        case MSG_RESTORE_DONE: {
            // Background thread finished restoring - update UI
            BList* results = NULL;
            BList* orphanedRefs = NULL;
            int32 firstSelectedIndex = -1;
            
            message->FindPointer("results", (void**)&results);
            message->FindPointer("orphaned_refs", (void**)&orphanedRefs);
            message->FindInt32("first_index", &firstSelectedIndex);
            
            // Find and remove rows for successfully restored emails
            if (results) {
                BList rowsToRemove;
                for (int32 i = 0; i < results->CountItems(); i++) {
                    RestoreResult* result = (RestoreResult*)results->ItemAt(i);
                    if (!result)
                        continue;
                    
                    if (result->success) {
                        // Find the row using node_ref
                        for (int32 j = 0; j < fEmailList->CountItems(); j++) {
                            EmailItem* row = fEmailList->ItemAt(j);
                            if (row) {
                                const node_ref* rowNref = row->GetNodeRef();
                                if (rowNref->device == result->nref.device && 
                                    rowNref->node == result->nref.node) {
                                    rowsToRemove.AddItem(row);
                                    break;
                                }
                            }
                        }
                    }
                    delete result;
                }
                delete results;
                
                // Remove rows in batch
                if (rowsToRemove.CountItems() > 0) {
                    bool useBatch = rowsToRemove.CountItems() > 10;
                    if (useBatch)
                        fEmailList->BeginBatchRemove();
                    for (int32 i = 0; i < rowsToRemove.CountItems(); i++) {
                        EmailItem* row = (EmailItem*)rowsToRemove.ItemAt(i);
                        if (row) {
                            fEmailList->RemoveRow(row);
                        }
                    }
                    if (useBatch)
                        fEmailList->EndBatchRemove();
                }
            }
            
            // Handle orphaned emails (unknown account)
            if (orphanedRefs && orphanedRefs->CountItems() > 0) {
                // Transfer to fPendingRestoreRefs
                for (int32 i = 0; i < orphanedRefs->CountItems(); i++) {
                    entry_ref* ref = (entry_ref*)orphanedRefs->ItemAt(i);
                    if (ref) {
                        fPendingRestoreRefs.AddItem(ref);
                    }
                }
                delete orphanedRefs;
                
                // Show explanation alert
                int32 count = fPendingRestoreRefs.CountItems();
                BString alertText;
                static BStringFormat format(B_TRANSLATE("{0, plural,"
                    "one{You are trying to restore a message that belongs to an account that is "
                        "not configured in your system. Please select the destination folder where "
                        "you want to restore this email.}"
                    "other{You are trying to restore # messages that belong to an account that is "
                        "not configured in your system. Please select the destination folder where "
                        "you want to restore these emails.}}"));
                format.Format(alertText, count);
                
                BAlert* alert = new BAlert(B_TRANSLATE("Restore emails"),
                    alertText.String(),
                    B_TRANSLATE("Cancel"), B_TRANSLATE("Select folder"), NULL,
                    B_WIDTH_AS_USUAL, B_INFO_ALERT);
                alert->SetShortcut(0, B_ESCAPE);
                
                // Use async alert with invoker to handle response
                BMessage* alertMsg = new BMessage(MSG_RESTORE_SHOW_FOLDER_PANEL);
                alert->Go(new BInvoker(alertMsg, this));
            } else if (orphanedRefs) {
                delete orphanedRefs;
            }
            
            // Update counts
            UpdateEmailCountLabel();
            RefreshQueryCountDisplay();
            
            // Show empty message if list is now empty
            if (fEmailList->CountItems() == 0) {
                ClearPreviewPane();
                if (fShowTrashOnly)
                    ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
                else if (fIsSearchActive)
                    ShowEmptyListMessage(B_TRANSLATE("No emails match your search."));
                else
                    ShowEmptyListMessage(B_TRANSLATE("No emails found."));
            } else {
                // Select next available row at the topmost restored position
                if (firstSelectedIndex >= 0) {
                    if (firstSelectedIndex < fEmailList->CountItems()) {
                        EmailItem* nextRow = fEmailList->ItemAt(firstSelectedIndex);
                        if (nextRow) {
                            int32 idx = fEmailList->IndexOf(nextRow);
                            fEmailList->Select(idx);
                            fEmailList->ScrollToItem(idx);
                        }
                    } else {
                        // We restored at the end, select the new last row
                        EmailItem* lastRow = fEmailList->ItemAt(fEmailList->CountItems() - 1);
                        if (lastRow) {
                            int32 idx = fEmailList->IndexOf(lastRow);
                            fEmailList->Select(idx);
                            fEmailList->ScrollToItem(idx);
                        }
                    }
                }
            }
            break;
        }
        
        case MSG_PERMANENT_DELETE_DONE: {
            // Background thread finished deleting - update UI
            BList* deletedPaths = NULL;
            if (message->FindPointer("deleted_paths", (void**)&deletedPaths) == B_OK && deletedPaths) {
                // Find and remove rows for deleted emails
                BList rowsToRemove;
                for (int32 i = 0; i < deletedPaths->CountItems(); i++) {
                    BString* path = (BString*)deletedPaths->ItemAt(i);
                    if (!path)
                        continue;
                    
                    // Find the row with this path
                    for (int32 j = 0; j < fEmailList->CountItems(); j++) {
                        EmailItem* row = fEmailList->ItemAt(j);
                        if (row && strcmp(row->GetPath(), path->String()) == 0) {
                            rowsToRemove.AddItem(row);
                            break;
                        }
                    }
                    delete path;
                }
                delete deletedPaths;
                
                // Remove rows in batch
                fEmailList->BeginBatchRemove();
                for (int32 i = 0; i < rowsToRemove.CountItems(); i++) {
                    EmailItem* row = (EmailItem*)rowsToRemove.ItemAt(i);
                    if (row) {
                        fEmailList->RemoveRow(row);
                    }
                }
                fEmailList->EndBatchRemove();
            }
            
            // Update counts and UI
            UpdateEmailCountLabel();
            RefreshQueryCountDisplay();
            
            // Show empty message if list is now empty
            if (fEmailList->CountItems() == 0) {
                ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
                ClearPreviewPane();
            }
            
            // Select next available row
            int32 firstIndex = -1;
            message->FindInt32("first_index", &firstIndex);
            
            if (firstIndex >= 0 && fEmailList->CountItems() > 0) {
                if (firstIndex < fEmailList->CountItems()) {
                    EmailItem* nextRow = fEmailList->ItemAt(firstIndex);
                    if (nextRow) {
                        fEmailList->Select(firstIndex);
                        fEmailList->ScrollToItem(firstIndex);
                    }
                } else {
                    int32 lastIdx = fEmailList->CountItems() - 1;
                    fEmailList->Select(lastIdx);
                    fEmailList->ScrollToItem(lastIdx);
                }
            }
            break;
        }
        
        case MSG_MOVE_TO_TRASH_DONE: {
            // Background move-to-trash completed. Rows were already removed
            // by B_ENTRY_REMOVED via live queries. Just update counts.
            UpdateEmailCountLabel();
            RefreshQueryCountDisplay();
            
            if (fEmailList->CountItems() == 0) {
                ClearPreviewPane();
                if (fIsSearchActive)
                    ShowEmptyListMessage(B_TRANSLATE("No emails match your search."));
                else
                    ShowEmptyListMessage(B_TRANSLATE("No emails found."));
            }
            break;
        }
        
        case MSG_RESTORE_SHOW_FOLDER_PANEL: {
            // Handle alert response - button 1 is "Select folder"
            int32 which;
            if (message->FindInt32("which", &which) == B_OK && which == 1) {
                // User clicked "Select folder" - show the file panel
                if (fPendingRestoreRefs.CountItems() > 0) {
                    // Create file panel if needed
                    if (fRestoreFolderPanel == NULL) {
                        fRestoreFolderPanel = new BFilePanel(B_OPEN_PANEL,
                            new BMessenger(this),
                            NULL,  // Start directory set below
                            B_DIRECTORY_NODE,
                            false,  // Single selection
                            NULL,   // Message set below
                            NULL,
                            true,   // Modal
                            true);  // Hide when done
                        fRestoreFolderPanel->Window()->SetTitle(B_TRANSLATE("Select destination folder"));
                    }
                    
                    // Set starting directory to mail
                    BPath mailPath;
                    if (find_directory(B_USER_DIRECTORY, &mailPath) == B_OK) {
                        mailPath.Append("mail");
                        fRestoreFolderPanel->SetPanelDirectory(mailPath.Path());
                    }
                    
                    // Set message (email refs are stored in fPendingRestoreRefs)
                    BMessage* panelMessage = new BMessage(MSG_RESTORE_FOLDER_SELECTED);
                    fRestoreFolderPanel->SetMessage(panelMessage);
                    
                    fRestoreFolderPanel->Show();
                }
            } else {
                // User clicked "Cancel" - clear pending refs
                for (int32 i = 0; i < fPendingRestoreRefs.CountItems(); i++) {
                    delete (entry_ref*)fPendingRestoreRefs.ItemAt(i);
                }
                fPendingRestoreRefs.MakeEmpty();
            }
            break;
        }
        
        case MSG_RESTORE_FOLDER_SELECTED: {
            // Handle folder selection from restore panel
            entry_ref dirRef;
            if (message->FindRef("refs", &dirRef) == B_OK) {
                BPath destPath(&dirRef);
                if (destPath.InitCheck() == B_OK) {
                    // Ensure destination directory exists
                    create_directory(destPath.Path(), 0755);
                    BDirectory destDir(destPath.Path());
                    
                    // Track which rows were successfully restored
                    BList rowsToRemove;
                    
                    // First pass: move files
                    for (int32 i = 0; i < fPendingRestoreRefs.CountItems(); i++) {
                        entry_ref* ref = (entry_ref*)fPendingRestoreRefs.ItemAt(i);
                        if (ref) {
                            // Get node_ref for row lookup (doesn't trigger lazy loading)
                            BNode node(ref);
                            node_ref nref;
                            if (node.InitCheck() == B_OK && node.GetNodeRef(&nref) == B_OK) {
                                // Find the row using node_ref
                                EmailItem* rowToRestore = NULL;
                                for (int32 j = 0; j < fEmailList->CountItems(); j++) {
                                    EmailItem* row = fEmailList->ItemAt(j);
                                    if (row) {
                                        const node_ref* rowNref = row->GetNodeRef();
                                        if (rowNref->device == nref.device && rowNref->node == nref.node) {
                                            rowToRestore = row;
                                            break;
                                        }
                                    }
                                }
                                
                                // Move file
                                BEntry entry(ref);
                                if (entry.InitCheck() == B_OK && destDir.InitCheck() == B_OK) {
                                    if (entry.MoveTo(&destDir) == B_OK && rowToRestore) {
                                        rowsToRemove.AddItem(rowToRestore);
                                    }
                                }
                            }
                        }
                    }
                    
                    // Second pass: remove rows in batch
                    fEmailList->BeginBatchRemove();
                    for (int32 i = 0; i < rowsToRemove.CountItems(); i++) {
                        EmailItem* row = (EmailItem*)rowsToRemove.ItemAt(i);
                        if (row) {
                            fEmailList->RemoveRow(row);
                        }
                    }
                    fEmailList->EndBatchRemove();
                    
                    // Update UI once after all restores
                    UpdateEmailCountLabel();
                    RefreshQueryCountDisplay();
                    
                    // Show empty message if list is now empty
                    if (fEmailList->CountItems() == 0) {
                        ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
                    }
                }
            }
            
            // Clear pending refs
            for (int32 i = 0; i < fPendingRestoreRefs.CountItems(); i++) {
                delete (entry_ref*)fPendingRestoreRefs.ItemAt(i);
            }
            fPendingRestoreRefs.MakeEmpty();
            break;
        }
        
        case MSG_EMPTY_TRASH: {
            // Use cached trash count from sidebar for instant dialog
            int32 trashCount = fCachedTrashCount;
            if (trashCount <= 0) {
                BAlert* alert = new BAlert(B_TRANSLATE("Delete emails in Trash"),
                    B_TRANSLATE("There are no emails in the Trash."),
                    B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
                alert->Go();
                break;
            }
            
            // Confirm with user first (no query needed yet)
            BString confirmMsg;
            static BStringFormat format(B_TRANSLATE("{0, plural,"
                "one{Permanently delete # email from Trash?\n\nThis cannot be undone.}"
                "other{Permanently delete # emails from Trash?\n\nThis cannot be undone.}}"));
            format.Format(confirmMsg, trashCount);

            BAlert* alert = new BAlert(B_TRANSLATE("Delete emails in Trash"),
                confirmMsg.String(),
                B_TRANSLATE("Cancel"), B_TRANSLATE("Delete"), NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            
            if (alert->Go() == 1) {
                // Suppress node monitor events during emptying
                fEmptyingTrash = true;
                fTrashLoaderStop = true;

                // Immediately update UI - clear list and show empty message
                if (fTrashItem->IsSelected()) {
                    fEmailList->Clear();
                    ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
                    UpdateEmailCountLabel();
                }

                // User confirmed - spawn background thread to query and delete
                TrashEmptyData* emptyData = new TrashEmptyData();
                emptyData->emailRefs = NULL;  // Thread will query for refs
                emptyData->messenger = BMessenger(this);
                for (int32 v = 0; v < fSelectedVolumes.CountItems(); v++) {
                    BVolume* vol = fSelectedVolumes.ItemAt(v);
                    if (vol != NULL)
                        emptyData->volumes.AddItem(vol);
                }
                
                thread_id emptyThread = spawn_thread(TrashEmptyThread,
                    "trash_empty", B_NORMAL_PRIORITY, emptyData);
                if (emptyThread >= 0) {
                    resume_thread(emptyThread);
                } else {
                    delete emptyData;
                }
            }
            break;
        }
        
        case MSG_TRASH_BATCH: {
            // Batch of trash emails from loader thread
            // Show the list card on first batch
            if (fEmailListCardView != NULL)
                fEmailListCardView->CardLayout()->SetVisibleItem((int32)1);
            void* ptr;
            int32 index = 0;
            while (message->FindPointer("emailref", index++, &ptr) == B_OK) {
                EmailRef* emailRef = (EmailRef*)ptr;
                if (emailRef != NULL) {
                    fEmailList->AddEmailSorted(emailRef);
                }
            }
            UpdateEmailCountLabel();
            break;
        }
        
        case MSG_TRASH_LOAD_DONE: {
            // Trash loading complete
            fEmailList->EndBulkLoad();
            fEmailList->SortItems();
            
            if (fEmailList->CountItems() == 0) {
                ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
            } else {
                ShowEmailListContent();
            }
            UpdateEmailCountLabel();
            break;
        }
        
        case MSG_TRASH_EMPTIED: {
            // Background thread finished emptying trash - update UI
            fEmptyingTrash = false;
            if (fShowTrashOnly) {
                fEmailList->Clear();
                ClearPreviewPane();
            }
            // Refresh counts - will query fresh
            RefreshQueryCountDisplay();
            
            // Clear undo stack — the emails no longer exist in Trash
            fUndoStack.clear();
            fUndoMenuItem->SetEnabled(false);
            break;
        }
        
        case EmailListView::kMsgLoadingUpdate: {
            // Loading progress update from EmailListView
            bool complete = false;
            message->FindBool("complete", &complete);
            
            if (!complete) {
                // Update status bar with running count during loading.
                // Skip during body search — its own handlers manage the status text.
                if (!fEmailList->IsBodySearchRunning())
                    UpdateEmailCountLabel();
                
                // Start loading dots only during an actual load, not on
                // lightweight count-change notifications from live queries.
                bool loading = false;
                message->FindBool("loading", &loading);
                if (loading)
                    fEmailList->StartLoadingDots();
                
                // Signal loading in progress (disables backup button)
                fSearchField->SetLoading(true);
                
                // Switch to email list view as soon as content arrives.
                // Skip this during BFS loading when a body search is pending
                // — the full list would flash before being replaced.
                if (fEmailList->CountItems() > 0 && !fPendingBodySearch) {
                    ShowEmailListContent();
                    if (!fShowTrashOnly)
                        fSearchField->SetViewHasContent(true);
                }
                
                // Refresh sidebar counts on live query changes
                // (e.g. new draft saved, email deleted)
                if (!fEmailList->IsLoading())
                    ScheduleQueryCountUpdate();
            }
            
            if (complete) {
                // Check if a body search is pending (BFS query was just the
                // first phase to populate the search scope).
                if (fPendingBodySearch && fPendingBodySearchText.Length() > 0) {
                    fPendingBodySearch = false;
                    
                    if (fEmailList->CountItems() == 0) {
                        // Nothing to search — go straight to empty result
                        fEmailList->StopLoadingDots();
                        fSearchField->SetLoading(false);
                        fSearchField->SetHasResults(false);
                        ShowEmptyListMessage(B_TRANSLATE("No emails match your search."));
                        UpdateEmailCountLabel();
                    } else {
                        // Launch body search on the populated list.
                        // Don't stop loading dots — body search will keep them going.
                        fSearchField->SetSearchExecuted(true);
                        fSearchField->SetBodySearchRunning(true);
                        fEmailList->StartBodySearch(
                            fPendingBodySearchText.String(),
                            fPendingBodySearchCaseSensitive,
                            fPendingBodySearchFullText);
                    }
                    break;
                }
                
                // Loading finished — stop loading dots and re-enable backup button
                fEmailList->StopLoadingDots();
                fSearchField->SetLoading(false);
                fSearchField->SetBodySearchRunning(false);
                
                if (fIsSearchActive) {
                    fSearchField->SetHasResults(fEmailList->CountItems() > 0);
                }
                
                if (fEmailList->CountItems() == 0) {
                    if (fIsSearchActive)
                        ShowEmptyListMessage(B_TRANSLATE("No emails match your search."));
                    else if (fShowTrashOnly)
                        ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
                    else
                        ShowEmptyListMessage(B_TRANSLATE("No emails found."));
                } else {
                    ShowEmailListContent();
                }
                
                UpdateEmailCountLabel();
                ScheduleQueryCountUpdate();
                
                // After a body/full-text search completes, override the
                // status text from "N emails" to "N found" so it's clear
                // these are search results, not the full list.
                if (fSearchField->IsBodySearch() && fIsSearchActive) {
                    int32 count = fEmailList->CountItems();
                    BString label;
                    if (count == 1)
                        label = B_TRANSLATE("1 email found");
                    else {
                        BString countStr;
                        if (count >= 1000)
                            countStr.SetToFormat("%ld,%03ld",
                                (long)(count / 1000), (long)(count % 1000));
                        else
                            countStr.SetToFormat("%ld", (long)count);
                        label.SetToFormat(B_TRANSLATE("%s emails found"),
                            countStr.String());
                    }
                    fEmailList->SetStatusText(label.String());
                }
                
                // Give the email list keyboard focus when loading completes
                // so Alt+A and keyboard navigation work without requiring a click
                fEmailList->MakeFocus(true);
            }
            break;
        }
        
        case MSG_MARK_READ: {
            // Mark all selected emails as read - write attributes, live query auto-updates display
            EmailItem* row = NULL;
            while ((row = fEmailList->CurrentSelection(row)) != NULL) {
                MarkEmailAsRead(row->GetPath(), true);
            }
            
            // Re-scan selection to set accurate button states.
            // Some emails may still be "Read" (were already read) or have other
            // statuses (Sent, Replied) that were left untouched.
            {
                bool hasNew = false;
                bool hasRead = false;
                row = NULL;
                while ((row = fEmailList->CurrentSelection(row)) != NULL) {
                    const char* status = row->GetStatus();
                    if (strcmp(status, "New") == 0)
                        hasNew = true;
                    else if (strcmp(status, "Read") == 0)
                        hasRead = true;
                }
                fToolBar->SetActionEnabled(MSG_MARK_READ, hasNew);
                fMarkReadMenuItem->SetEnabled(hasNew);
                fToolBar->SetActionEnabled(MSG_MARK_UNREAD, hasRead);
                fMarkUnreadMenuItem->SetEnabled(hasRead);
            }
            
            // Update folder unread counts
            ScheduleQueryCountUpdate();
            break;
        }
        
        case MSG_MARK_UNREAD: {
            // Mark all selected emails as unread - write attributes, live query auto-updates display
            EmailItem* row = NULL;
            while ((row = fEmailList->CurrentSelection(row)) != NULL) {
                MarkEmailAsRead(row->GetPath(), false);
            }
            
            // Re-scan selection to set accurate button states.
            {
                bool hasNew = false;
                bool hasRead = false;
                row = NULL;
                while ((row = fEmailList->CurrentSelection(row)) != NULL) {
                    const char* status = row->GetStatus();
                    if (strcmp(status, "New") == 0)
                        hasNew = true;
                    else if (strcmp(status, "Read") == 0)
                        hasRead = true;
                }
                fToolBar->SetActionEnabled(MSG_MARK_READ, hasNew);
                fMarkReadMenuItem->SetEnabled(hasNew);
                fToolBar->SetActionEnabled(MSG_MARK_UNREAD, hasRead);
                fMarkUnreadMenuItem->SetEnabled(hasRead);
            }
            
            // Update folder unread counts
            ScheduleQueryCountUpdate();
            break;
        }
        
        case MSG_MARK_SENT: {
            // Development/maintenance utility: bulk-set MAIL:status to "Sent".
            // Not exposed in the UI; kept for manual corrections via scripting
            // or message injection (e.g. fixing mis-categorized emails).
            EmailItem* row = NULL;
            while ((row = fEmailList->CurrentSelection(row)) != NULL) {
                BNode node(row->GetPath());
                if (node.InitCheck() == B_OK) {
                    node.WriteAttr("MAIL:status", B_STRING_TYPE, 0, "Sent", 5);
                    int32 readFlag = B_READ;
                    node.WriteAttr("MAIL:read", B_INT32_TYPE, 0, &readFlag, sizeof(readFlag));
                }
            }
            
            // Update folder counts
            ScheduleQueryCountUpdate();
            break;
        }
        
        case MSG_MARK_SPAM: {
            // Collect selected rows first (iteration won't work after removal)
            BList selectedRows;
            EmailItem* row = NULL;
            while ((row = fEmailList->CurrentSelection(row)) != NULL)
                selectedRows.AddItem(row);

            int32 count = selectedRows.CountItems();
            if (count == 0)
                break;

            // Remember index of first selected row for auto-select
            int32 firstIndex = -1;
            if (count == 1) {
                EmailItem* first = (EmailItem*)selectedRows.ItemAt(0);
                if (first)
                    firstIndex = fEmailList->IndexOf(first);
            }

            // Write attributes and update blocklist
            std::set<BString> blockedSenders;
            for (int32 i = 0; i < count; i++) {
                EmailItem* sel = (EmailItem*)selectedRows.ItemAt(i);
                if (sel == NULL)
                    continue;
                BNode node(sel->GetPath());
                if (node.InitCheck() == B_OK) {
                    BString spam("Spam");
                    node.WriteAttrString("MAIL:classification", &spam);
                }
                const char* from = sel->GetFrom();
                if (from != NULL && from[0] != '\0') {
                    BString addr = _ExtractEmailAddress(from);
                    AddToSpamBlocklist(addr.String());
                    blockedSenders.insert(addr);
                }
            }

            // Remove all rows from blocked senders from the list
            fEmailList->DeselectAll();
            fEmailList->BeginBatchRemove();

            // Collect all rows to remove (selected + same sender)
            BList rowsToRemove;
            for (int32 i = 0; i < count; i++)
                rowsToRemove.AddItem(selectedRows.ItemAt(i));

            // Find other rows from the same senders
            for (int32 i = fEmailList->CountItems() - 1; i >= 0; i--) {
                EmailItem* item = fEmailList->ItemAt(i);
                if (item == NULL)
                    continue;
                if (rowsToRemove.HasItem(item))
                    continue;
                const char* from = item->GetFrom();
                if (from != NULL && from[0] != '\0') {
                    BString addr = _ExtractEmailAddress(from);
                    if (blockedSenders.count(addr) > 0) {
                        // Also write spam attribute on this email
                        BNode node(item->GetPath());
                        if (node.InitCheck() == B_OK) {
                            BString spam("Spam");
                            node.WriteAttrString("MAIL:classification", &spam);
                        }
                        rowsToRemove.AddItem(item);
                    }
                }
            }

            for (int32 i = 0; i < rowsToRemove.CountItems(); i++) {
                EmailItem* item = (EmailItem*)rowsToRemove.ItemAt(i);
                if (item)
                    fEmailList->RemoveRow(item);
            }
            fEmailList->EndBatchRemove();

            // Auto-select next email or clear preview
            if (fEmailList->CountItems() > 0) {
                int32 selectIndex = firstIndex;
                if (selectIndex >= fEmailList->CountItems())
                    selectIndex = fEmailList->CountItems() - 1;
                if (selectIndex >= 0) {
                    fEmailList->Select(selectIndex);
                    fEmailList->ScrollToItem(selectIndex);
                }
            } else {
                ClearPreviewPane();
            }

            UpdateEmailCountLabel();
            ScheduleQueryCountUpdate();
            break;
        }
        
        case MSG_UNMARK_SPAM: {
            // Collect selected rows first
            BList selectedRows;
            EmailItem* row = NULL;
            while ((row = fEmailList->CurrentSelection(row)) != NULL)
                selectedRows.AddItem(row);

            int32 count = selectedRows.CountItems();
            if (count == 0)
                break;

            int32 firstIndex = -1;
            if (count == 1) {
                EmailItem* first = (EmailItem*)selectedRows.ItemAt(0);
                if (first)
                    firstIndex = fEmailList->IndexOf(first);
            }

            // Remove classification and update blocklist
            // Collect unique sender addresses being unblocked
            std::set<BString> unblockedSenders;
            for (int32 i = 0; i < count; i++) {
                EmailItem* sel = (EmailItem*)selectedRows.ItemAt(i);
                if (sel == NULL)
                    continue;
                BNode node(sel->GetPath());
                if (node.InitCheck() == B_OK)
                    node.RemoveAttr("MAIL:classification");
                const char* from = sel->GetFrom();
                if (from != NULL && from[0] != '\0') {
                    BString addr = _ExtractEmailAddress(from);
                    RemoveFromSpamBlocklist(addr.String());
                    unblockedSenders.insert(addr);
                }
            }

            // Clear MAIL:classification on ALL emails from unblocked senders
            if (!unblockedSenders.empty()) {
                BVolumeRoster volumeRoster;
                BVolume volume;
                volumeRoster.GetBootVolume(&volume);

                BQuery query;
                query.SetVolume(&volume);
                query.SetPredicate("(BEOS:TYPE==\"text/x-email\")"
                    "&&(MAIL:classification==Spam)");

                // Collect all matching refs first, then modify attributes
                // (avoids index update race during iteration)
                BList matchingRefs;
                if (query.Fetch() == B_OK) {
                    entry_ref ref;
                    while (query.GetNextRef(&ref) == B_OK) {
                        BNode qNode(&ref);
                        if (qNode.InitCheck() != B_OK)
                            continue;
                        char fromBuf[256] = "";
                        qNode.ReadAttr("MAIL:from", B_STRING_TYPE, 0,
                            fromBuf, sizeof(fromBuf) - 1);
                        if (fromBuf[0] == '\0')
                            continue;
                        BString addr = _ExtractEmailAddress(fromBuf);
                        if (unblockedSenders.count(addr) > 0)
                            matchingRefs.AddItem(new entry_ref(ref));
                    }
                }

                // Now clear attributes on collected refs
                for (int32 i = 0; i < matchingRefs.CountItems(); i++) {
                    entry_ref* ref = (entry_ref*)matchingRefs.ItemAt(i);
                    BNode qNode(ref);
                    if (qNode.InitCheck() == B_OK)
                        qNode.RemoveAttr("MAIL:classification");
                    delete ref;
                }
            }

            // Remove all rows from unblocked senders from the list
            fEmailList->DeselectAll();
            fEmailList->BeginBatchRemove();

            // First collect all rows to remove (selected + same sender)
            BList rowsToRemove;
            for (int32 i = 0; i < count; i++)
                rowsToRemove.AddItem(selectedRows.ItemAt(i));

            // Find other rows from the same senders
            for (int32 i = fEmailList->CountItems() - 1; i >= 0; i--) {
                EmailItem* item = fEmailList->ItemAt(i);
                if (item == NULL)
                    continue;
                // Skip already-collected rows
                if (rowsToRemove.HasItem(item))
                    continue;
                const char* from = item->GetFrom();
                if (from != NULL && from[0] != '\0') {
                    BString addr = _ExtractEmailAddress(from);
                    if (unblockedSenders.count(addr) > 0)
                        rowsToRemove.AddItem(item);
                }
            }

            for (int32 i = 0; i < rowsToRemove.CountItems(); i++) {
                EmailItem* item = (EmailItem*)rowsToRemove.ItemAt(i);
                if (item)
                    fEmailList->RemoveRow(item);
            }
            fEmailList->EndBatchRemove();

            // Auto-select next email or clear preview
            if (fEmailList->CountItems() > 0) {
                int32 selectIndex = firstIndex;
                if (selectIndex >= fEmailList->CountItems())
                    selectIndex = fEmailList->CountItems() - 1;
                if (selectIndex >= 0) {
                    fEmailList->Select(selectIndex);
                    fEmailList->ScrollToItem(selectIndex);
                }
            } else {
                ClearPreviewPane();
            }

            UpdateEmailCountLabel();
            ScheduleQueryCountUpdate();
            break;
        }
        
        case MSG_STAR_EMAIL: {
            // Toggle star on all selected emails - write attributes, live query auto-updates
            bool hasStarred = false;
            bool hasUnstarred = false;
            EmailItem* checkRow = NULL;
            while ((checkRow = fEmailList->CurrentSelection(checkRow)) != NULL) {
                if (checkRow->IsStarred()) {
                    hasStarred = true;
                } else {
                    hasUnstarred = true;
                }
            }
            
            // Decide action: if all starred, unstar; otherwise star all
            bool newStarState = !(hasStarred && !hasUnstarred);
            
            // Write attributes to files - live query handles display updates automatically
            EmailItem* row = NULL;
            while ((row = fEmailList->CurrentSelection(row)) != NULL) {
                BNode node(row->GetPath());
                if (node.InitCheck() == B_OK) {
                    int32 starValue = newStarState ? 1 : 0;
                    node.WriteAttr("FILE:starred", B_INT32_TYPE, 0, &starValue, sizeof(starValue));
                }
            }
            break;
        }
        
        case MSG_EMAIL_SENDER: {
            EmailItem* row = fEmailList->CurrentSelection();
            if (row) {
                // Read the From attribute
                BFile file(row->GetPath(), B_READ_ONLY);
                char fromBuffer[256] = "";
                ssize_t size = file.ReadAttr("MAIL:from", B_STRING_TYPE, 0, fromBuffer, sizeof(fromBuffer) - 1);
                if (size > 0) {
                    fromBuffer[size] = '\0';
                    
                    // Extract email address (get part between < and >)
                    BString emailAddr(fromBuffer);
                    int32 angleStart = emailAddr.FindFirst("<");
                    int32 angleEnd = emailAddr.FindFirst(">");
                    if (angleStart >= 0 && angleEnd > angleStart) {
                        emailAddr.Remove(0, angleStart + 1);
                        emailAddr.Truncate(angleEnd - angleStart - 1);
                    }
                    
                    // Open compose window with recipient pre-filled
                    BMessage newMsg(M_NEW);
                    newMsg.AddString("to", emailAddr.String());
                    be_app->PostMessage(&newMsg);
                }
            }
            break;
        }
        
        case MSG_CREATE_PERSON: {
            const char* address;
            const char* name;
            if (message->FindString("address", &address) != B_OK)
                break;
            if (message->FindString("name", &name) != B_OK)
                name = "";
            
            // Search for existing Person file with this email address
            BVolumeRoster volumeRoster;
            BVolume volume;
            BQuery query;
            BEntry entry;
            bool foundEntry = false;
            
            BString predicate("META:email=");
            predicate << address;
            
            while (volumeRoster.GetNextVolume(&volume) == B_NO_ERROR) {
                if (!volume.KnowsQuery())
                    continue;
                
                query.SetVolume(&volume);
                query.SetPredicate(predicate.String());
                query.Fetch();
                
                if (query.GetNextEntry(&entry) == B_NO_ERROR) {
                    // Found existing Person file - open it via Tracker
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
                query.Clear();
            }
            
            if (!foundEntry) {
                // No existing Person found - launch People app with prefilled data
                // 'newp' is the message code People app expects for new person with data
                BMessage launchMsg('newp');
                launchMsg.AddString("META:name", name);
                launchMsg.AddString("META:email", address);
                
                status_t result = be_roster->Launch("application/x-person", &launchMsg);
                if (result != B_OK && result != B_ALREADY_RUNNING) {
                    BAlert* alert = new BAlert("",
                        B_TRANSLATE("Sorry, could not find an application that "
                        "supports the 'Person' data type."),
                        B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);
                    alert->Go();
                }
            }
            break;
        }
        
        case MSG_FILTER_BY_SENDER: {
            EmailItem* row = fEmailList->CurrentSelection();
            if (row) {
                // Read the From attribute
                BFile file(row->GetPath(), B_READ_ONLY);
                char fromBuffer[256] = "";
                ssize_t size = file.ReadAttr("MAIL:from", B_STRING_TYPE, 0, fromBuffer, sizeof(fromBuffer) - 1);
                if (size > 0) {
                    fromBuffer[size] = '\0';
                    
                    // Escape any double quotes in the sender string
                    BString sender(fromBuffer);
                    sender.CharacterEscape("\"", '\\');
                    
                    // Format as exact match query (no wildcards for structured fields)
                    BString query;
                    query.SetToFormat("((MAIL:from==\"%s\")&&(BEOS:TYPE==\"text/x-email\"))", sender.String());
                    
                    // Extract email address for the filter name (get part between < and >)
                    BString emailAddr(fromBuffer);
                    int32 angleStart = emailAddr.FindFirst("<");
                    int32 angleEnd = emailAddr.FindFirst(">");
                    if (angleStart >= 0 && angleEnd > angleStart) {
                        emailAddr.Remove(0, angleStart + 1);
                        emailAddr.Truncate(angleEnd - angleStart - 1);
                    }
                    
                    BString name;
                    name.SetToFormat("'From' is: %s", emailAddr.String());
                    AddCustomQuery(name.String(), query.String(), "From", emailAddr.String());
                }
            }
            break;
        }
        
        case MSG_FILTER_BY_RECIPIENT: {
            EmailItem* row = fEmailList->CurrentSelection();
            if (row) {
                // Read the To attribute
                BFile file(row->GetPath(), B_READ_ONLY);
                char toBuffer[256] = "";
                ssize_t size = file.ReadAttr("MAIL:to", B_STRING_TYPE, 0, toBuffer, sizeof(toBuffer) - 1);
                if (size > 0) {
                    toBuffer[size] = '\0';
                    
                    // Escape any double quotes in the recipient string
                    BString recipient(toBuffer);
                    recipient.CharacterEscape("\"", '\\');
                    
                    // Format as exact match query (no wildcards for structured fields)
                    BString query;
                    query.SetToFormat("((MAIL:to==\"%s\")&&(BEOS:TYPE==\"text/x-email\"))", recipient.String());
                    
                    // Extract email address for the filter name (get part between < and >)
                    BString emailAddr(toBuffer);
                    int32 angleStart = emailAddr.FindFirst("<");
                    int32 angleEnd = emailAddr.FindFirst(">");
                    if (angleStart >= 0 && angleEnd > angleStart) {
                        emailAddr.Remove(0, angleStart + 1);
                        emailAddr.Truncate(angleEnd - angleStart - 1);
                    }
                    
                    BString name;
                    name.SetToFormat("'To' is: %s", emailAddr.String());
                    AddCustomQuery(name.String(), query.String(), "To", emailAddr.String());
                }
            }
            break;
        }
        
        case MSG_FILTER_BY_ACCOUNT: {
            EmailItem* row = fEmailList->CurrentSelection();
            if (row) {
                BFile file(row->GetPath(), B_READ_ONLY);
                BString accountName;
                
                // Check attribute type first to handle mail_daemon bug
                attr_info attrInfo;
                if (file.GetAttrInfo("MAIL:account", &attrInfo) == B_OK) {
                    if (attrInfo.type == B_STRING_TYPE) {
                        char accountBuffer[256] = "";
                        ssize_t size = file.ReadAttr("MAIL:account", B_STRING_TYPE, 0, accountBuffer, sizeof(accountBuffer) - 1);
                        if (size > 0) {
                            accountBuffer[size] = '\0';
                            accountName = accountBuffer;
                        }
                    } else if (attrInfo.type == B_INT32_TYPE) {
                        // Workaround for mail_daemon bug: sent mail stores account as int32
                        int32 accountId;
                        if (file.ReadAttr("MAIL:account", B_INT32_TYPE, 0, &accountId, sizeof(accountId)) == sizeof(accountId)) {
                            accountName = EmailAccountMap::Instance().GetAccountName(accountId);
                            // Self-repair: write correct string attribute
                            if (!accountName.IsEmpty() && accountName != BString() << accountId) {
                                BNode node(row->GetPath());
                                if (node.InitCheck() == B_OK) {
                                    node.RemoveAttr("MAIL:account");
                                    node.WriteAttr("MAIL:account", B_STRING_TYPE, 0, 
                                        accountName.String(), accountName.Length() + 1);
                                }
                            }
                        }
                    }
                }
                
                if (!accountName.IsEmpty()) {
                    // Escape account name for query
                    BString escapedName(accountName);
                    escapedName.CharacterEscape("\"", '\\');
                    
                    // All emails are repaired on load, so we only need string matching
                    BString query;
                    query.SetToFormat("((MAIL:account==\"%s\")&&(BEOS:TYPE==\"text/x-email\"))", 
                        escapedName.String());
                    
                    BString name;
                    name.SetToFormat("'Account' is: %s", accountName.String());
                    AddCustomQuery(name.String(), query.String(), "Account", accountName.String());
                }
            }
            break;
        }
        
        case MSG_REMOVE_FILTER: {
            int32 selection = fQueryList->CurrentSelection();
            if (selection >= 0) {
                BListItem* listItem = fQueryList->ItemAt(selection);
                QueryItem* item = dynamic_cast<QueryItem*>(listItem);
                RemoveCustomQuery(item);
            }
            break;
        }
        
        case MSG_FILTER_EXACT_SENDER: {
            EmailItem* row = fEmailList->CurrentSelection();
            if (row) {
                // Read the From attribute
                BFile file(row->GetPath(), B_READ_ONLY);
                char fromBuffer[256] = "";
                ssize_t size = file.ReadAttr("MAIL:from", B_STRING_TYPE, 0, fromBuffer, sizeof(fromBuffer) - 1);
                if (size > 0) {
                    fromBuffer[size] = '\0';
                    
                    // Set up search bar for exact match filter
                    fSearchField->SetSearchAttribute(SEARCH_FROM);
                    fSearchField->SetMatchesMode(true);
                    fSearchField->SetText(fromBuffer);
                    ApplySearchFilter();
                    fSearchField->SetSearchExecuted(true);
                }
            }
            break;
        }
        
        case MSG_FILTER_EXACT_RECIPIENT: {
            EmailItem* row = fEmailList->CurrentSelection();
            if (row) {
                // Read the To attribute
                BFile file(row->GetPath(), B_READ_ONLY);
                char toBuffer[256] = "";
                ssize_t size = file.ReadAttr("MAIL:to", B_STRING_TYPE, 0, toBuffer, sizeof(toBuffer) - 1);
                if (size > 0) {
                    toBuffer[size] = '\0';
                    
                    // Set up search bar for exact match filter
                    fSearchField->SetSearchAttribute(SEARCH_TO);
                    fSearchField->SetMatchesMode(true);
                    fSearchField->SetText(toBuffer);
                    ApplySearchFilter();
                    fSearchField->SetSearchExecuted(true);
                }
            }
            break;
        }
        
        case MSG_FILTER_EXACT_SUBJECT: {
            EmailItem* row = fEmailList->CurrentSelection();
            if (row) {
                // Read the Subject attribute
                BFile file(row->GetPath(), B_READ_ONLY);
                char subjectBuffer[256] = "";
                ssize_t size = file.ReadAttr("MAIL:subject", B_STRING_TYPE, 0, subjectBuffer, sizeof(subjectBuffer) - 1);
                if (size > 0) {
                    subjectBuffer[size] = '\0';
                    
                    // Set up search bar for matches filter (text is exact from email attribute)
                    fSearchField->SetSearchAttribute(SEARCH_SUBJECT);
                    fSearchField->SetMatchesMode(true);
                    fSearchField->SetText(subjectBuffer);
                    ApplySearchFilter();
                    fSearchField->SetSearchExecuted(true);
                }
            }
            break;
        }
        
        case MSG_FILTER_EXACT_ACCOUNT: {
            EmailItem* row = fEmailList->CurrentSelection();
            if (row) {
                const char* account = row->GetAccount();
                if (account != NULL && account[0] != '\0') {
                    // Set up search bar for exact match filter
                    fSearchField->SetSearchAttribute(SEARCH_ACCOUNT);
                    fSearchField->SetMatchesMode(true);
                    fSearchField->SetText(account);
                    ApplySearchFilter();
                    fSearchField->SetSearchExecuted(true);
                }
            }
            break;
        }

        case MSG_EDIT_QUERY: {
            // Get the currently selected item in the left pane
            int32 selection = fQueryList->CurrentSelection();
            if (selection >= 0) {
                QueryItem* item = (QueryItem*)fQueryList->ItemAt(selection);
                
                if (item && item->IsCustomQuery()) {
                    // Get entry_ref for the query file (path is already stored in QueryItem)
                    BEntry entry(item->GetPath());
                    entry_ref ref;
                    if (entry.GetRef(&ref) == B_OK) {
                        // Send the query file to Tracker
                        // Tracker automatically opens .query files in the query editor
                        BMessenger tracker("application/x-vnd.Be-TRAK");
                        BMessage openMsg(B_REFS_RECEIVED);
                        openMsg.AddRef("refs", &ref);
                        tracker.SendMessage(&openMsg);
                    }
                }
            }
            break;
        }
        
        case MSG_OPEN_QUERIES_FOLDER: {
            // Open the queries folder in Tracker
            BPath queriesPath;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &queriesPath) == B_OK) {
                queriesPath.Append("EmailViews/queries");
                
                // Ensure the directory exists
                create_directory(queriesPath.Path(), 0755);
                
                BEntry entry(queriesPath.Path());
                entry_ref ref;
                if (entry.GetRef(&ref) == B_OK) {
                    BMessenger tracker("application/x-vnd.Be-TRAK");
                    BMessage openMsg(B_REFS_RECEIVED);
                    openMsg.AddRef("refs", &ref);
                    tracker.SendMessage(&openMsg);
                }
            }
            break;
        }
        
        case B_NODE_MONITOR: {
            int32 opcode;
            if (message->FindInt32("opcode", &opcode) != B_OK)
                break;
            
            // Handle volume mount/unmount events
            if (opcode == B_DEVICE_MOUNTED) {
                // New volume mounted - rebuild volume menu
                BuildVolumeMenu();
                
                // Check if this volume was previously selected
                dev_t device;
                if (message->FindInt32("new device", &device) == B_OK) {
                    bool wasSelected = IsVolumeSelected(device);
                    if (wasSelected) {
                        // Re-add to selected volumes and reload
                        BVolume* volume = new BVolume(device);
                        if (volume->InitCheck() == B_OK) {
                            fSelectedVolumes.AddItem(volume);
                            // Reload current view
                            if (fShowTrashOnly) {
                                LoadTrashEmails();
                            } else if (fCurrentViewItem) {
                                ResolveBaseQuery(fCurrentViewItem);
                                ExecuteQuery();
                            }
                        } else {
                            delete volume;
                        }
                    }
                    SaveVolumeSelection();
                }
                break;
            } else if (opcode == B_DEVICE_UNMOUNTED) {
                // Volume unmounted - rebuild menu and remove from selection
                BuildVolumeMenu();
                
                dev_t device;
                if (message->FindInt32("device", &device) == B_OK) {
                    // Remove from selected volumes
                    bool wasSelected = IsVolumeSelected(device);
                    SetVolumeSelected(device, false);
                    
                    if (wasSelected) {
                        // Reload current view without that volume
                        if (fShowTrashOnly) {
                            LoadTrashEmails();
                        } else if (fCurrentViewItem) {
                            ResolveBaseQuery(fCurrentViewItem);
                            ExecuteQuery();
                        }
                    }
                }
                break;
            }
            
            // Handle trash directory notifications when viewing trash
            if (fShowTrashOnly) {
                // Skip all trash node events while emptying trash
                if (fEmptyingTrash)
                    break;

                if (opcode == B_ENTRY_CREATED) {
                    // For B_ENTRY_CREATED, check if directory matches trash
                    ino_t directory;
                    dev_t device;
                    if (message->FindInt64("directory", &directory) == B_OK &&
                        message->FindInt32("device", &device) == B_OK &&
                        device == fTrashDirRef.device && directory == fTrashDirRef.node) {
                        // Something created in trash
                        const char* name;
                        if (message->FindString("name", &name) == B_OK) {
                            entry_ref ref(device, directory, name);
                            BEntry entry(&ref);
                            if (entry.IsFile()) {
                                // Check if it's an email
                                BNode fileNode(&ref);
                                if (fileNode.InitCheck() == B_OK) {
                                    attr_info info;
                                    if (fileNode.GetAttrInfo("MAIL:subject", &info) == B_OK) {
                                        EmailRef* emailRef = new EmailRef(ref);
                                        fEmailList->AddEmailSorted(emailRef);
                                        UpdateEmailCountLabel();
                                        ShowEmailListContent();
                                    }
                                }
                            } else if (entry.IsDirectory()) {
                                // Folder moved to trash - reload to pick up contained emails
                                LoadTrashEmails();
                            }
                        }
                        ScheduleQueryCountUpdate();
                        break;
                    }
                } else if (opcode == B_ENTRY_MOVED) {
                    // For B_ENTRY_MOVED, check "to directory" for files moved INTO trash
                    ino_t toDir;
                    dev_t device;
                    if (message->FindInt64("to directory", &toDir) == B_OK &&
                        message->FindInt32("device", &device) == B_OK &&
                        device == fTrashDirRef.device && toDir == fTrashDirRef.node) {
                        // Something moved to trash
                        const char* name;
                        if (message->FindString("name", &name) == B_OK) {
                            entry_ref ref(device, toDir, name);
                            BEntry entry(&ref);
                            if (entry.IsFile()) {
                                // Check if it's an email
                                BNode fileNode(&ref);
                                if (fileNode.InitCheck() == B_OK) {
                                    attr_info info;
                                    if (fileNode.GetAttrInfo("MAIL:subject", &info) == B_OK) {
                                        EmailRef* emailRef = new EmailRef(ref);
                                        fEmailList->AddEmailSorted(emailRef);
                                        UpdateEmailCountLabel();
                                        ShowEmailListContent();
                                    }
                                }
                            } else if (entry.IsDirectory()) {
                                // Folder moved to trash - reload to pick up contained emails
                                LoadTrashEmails();
                            }
                        }
                        ScheduleQueryCountUpdate();
                        break;
                    }
                    
                    // Also check "from directory" for files moved OUT of trash (restored)
                    ino_t fromDir;
                    if (message->FindInt64("from directory", &fromDir) == B_OK &&
                        message->FindInt32("device", &device) == B_OK &&
                        device == fTrashDirRef.device && fromDir == fTrashDirRef.node) {
                        // Something moved out of trash
                        const char* name;
                        if (message->FindString("name", &name) == B_OK) {
                            // Check if it's a directory (folder restored from trash)
                            ino_t toDir;
                            message->FindInt64("to directory", &toDir);
                            entry_ref ref(device, toDir, name);
                            BEntry entry(&ref);
                            if (entry.IsDirectory()) {
                                // Folder restored - reload to remove contained emails
                                LoadTrashEmails();
                                ScheduleQueryCountUpdate();
                                break;
                            }
                        }
                        // Individual file moved out - remove from list
                        ino_t node;
                        if (message->FindInt64("node", &node) == B_OK) {
                            node_ref nodeRef;
                            nodeRef.device = device;
                            nodeRef.node = node;
                            fEmailList->RemoveEmail(nodeRef);
                            UpdateEmailCountLabel();
                            if (fEmailList->CountItems() == 0)
                                ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
                        }
                        ScheduleQueryCountUpdate();
                        break;
                    }
                } else if (opcode == B_ENTRY_REMOVED) {
                    // File or folder permanently deleted from trash
                    ino_t directory;
                    dev_t device;
                    if (message->FindInt64("directory", &directory) == B_OK &&
                        message->FindInt32("device", &device) == B_OK &&
                        device == fTrashDirRef.device && directory == fTrashDirRef.node) {
                        ino_t node;
                        if (message->FindInt64("node", &node) == B_OK) {
                            node_ref nodeRef;
                            nodeRef.device = device;
                            nodeRef.node = node;
                            int32 countBefore = fEmailList->CountItems();
                            fEmailList->RemoveEmail(nodeRef);
                            if (fEmailList->CountItems() == countBefore) {
                                // Node wasn't in our list - likely a directory
                                // Reload to remove emails that were inside it
                                LoadTrashEmails();
                            } else {
                                UpdateEmailCountLabel();
                                if (fEmailList->CountItems() == 0)
                                    ShowEmptyListMessage(B_TRANSLATE("No emails in Trash."));
                            }
                        }
                        ScheduleQueryCountUpdate();
                        break;
                    }
                }
            }
            
            // Handle all filesystem events for our watched queries directory
            if (opcode == B_ENTRY_REMOVED) {
                // Remove just the deleted query item (handles Tracker
                // deletions; in-app deletions are already handled by
                // RemoveCustomQuery so this is a no-op in that case)
                const char* name;
                if (message->FindString("name", &name) == B_OK)
                    _RemoveQueryItemByName(name);
            } else if (opcode == B_ENTRY_CREATED || opcode == B_ENTRY_MOVED) {
                delete fQueryReloadRunner;
                BMessage reload(MSG_RELOAD_QUERIES_DEBOUNCED);
                fQueryReloadRunner = new BMessageRunner(BMessenger(this), &reload, 200000, 1);
            }
            // Don't reload on ATTR_CHANGED for queries directory - too noisy during renames
            break;
        }
        
        // MSG_PROCESS_QUERY_BATCH removed - query batching is handled by EmailListView
        
        case MSG_RELOAD_QUERIES_DEBOUNCED:
        case MSG_RELOAD_QUERIES: {
            // Clean up the runner now that it has fired (only for debounced case)
            if (message->what == MSG_RELOAD_QUERIES_DEBOUNCED) {
                delete fQueryReloadRunner;
                fQueryReloadRunner = NULL;
            }
            
            // 1. Stop watching the queries directory specifically (not all watches)
            watch_node(&fQueriesDirRef, B_STOP_WATCHING, this);
            
            // 2. Refresh the directory handle and node_ref
            BPath queriesPath;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &queriesPath) == B_OK) {
                queriesPath.Append("EmailViews/queries");
                
                if (fQueriesDir.SetTo(queriesPath.Path()) == B_OK) {
                    if (fQueriesDir.GetNodeRef(&fQueriesDirRef) == B_OK) {
                        // 3. Re-establish the watch
                        watch_node(&fQueriesDirRef, B_WATCH_DIRECTORY | B_WATCH_NAME | B_WATCH_ATTR, this);
                    }
                }
            }
            
            // 4. Reload the folder list
            LoadQueries();
            break;
        }
        
        case MSG_UPDATE_QUERY_COUNTS: {
            // Clean up the runner
            delete fQueryCountRunner;
            fQueryCountRunner = NULL;
            
            // Now actually update the counts
            UpdateQueryCounts();
            break;
        }
        
        case MSG_QUERY_COUNTS_READY: {
            // Background thread finished counting — update UI
            fQueryCountThread = -1;
            
            // Ignore stale results from superseded threads
            int32 generation = 0;
            message->FindInt32("generation", &generation);
            if (generation != fQueryCountGeneration)
                break;
            
            int32 newCount = 0, draftCount = 0, trashCount = 0, spamCount = 0;
            bool showTrashOnly = false;
            int32 listCount = 0;
            message->FindInt32("new_count", &newCount);
            message->FindInt32("draft_count", &draftCount);
            message->FindInt32("trash_count", &trashCount);
            message->FindInt32("spam_count", &spamCount);
            message->FindBool("show_trash_only", &showTrashOnly);
            message->FindInt32("list_count", &listCount);
            
            fCachedNewCount = newCount;
            fCachedTrashCount = showTrashOnly ? listCount : trashCount;
            
            // Update trash item count
            fTrashItem->SetCount(fCachedTrashCount);
            
            // Update query item labels
            for (int32 i = 0; i < fQueryList->CountItems(); i++) {
                BListItem* listItem = fQueryList->ItemAt(i);
                QueryItem* item = dynamic_cast<QueryItem*>(listItem);
                if (item && item->IsQuery()) {
                    BString label;
                    if (strstr(item->GetPath(), "MAIL:status==New") != NULL || 
                        strstr(item->GetPath(), "MAIL:status==Seen") != NULL) {
                        if (fCachedNewCount > 0) {
                            label.SetToFormat(B_TRANSLATE("Unread emails (%ld)"), fCachedNewCount);
                        } else {
                            label = B_TRANSLATE("Unread emails");
                        }
                        item->SetText(label.String());
                        
                        // Swap icon based on unread count
                        const char* iconName = (fCachedNewCount > 0)
                            ? "MailQueryUnread" : "MailQueryUnreadEmpty";
                        BResources* resources = BApplication::AppResources();
                        if (resources != NULL) {
                            size_t size;
                            const void* data = resources->LoadResource('VICN', iconName, &size);
                            if (data != NULL && size > 0) {
                                float iconSize = item->IconSize();
                                BBitmap* icon = new BBitmap(BRect(0, 0, iconSize - 1, iconSize - 1), B_RGBA32);
                                if (BIconUtils::GetVectorIcon((const uint8*)data, size, icon) == B_OK) {
                                    item->SetIcon(icon);
                                } else {
                                    delete icon;
                                }
                            }
                        }
                    } else if (strstr(item->GetPath(), "MAIL:draft==1") != NULL) {
                        if (draftCount > 0) {
                            label.SetToFormat(B_TRANSLATE("Draft emails (%ld)"), draftCount);
                        } else {
                            label = B_TRANSLATE("Draft emails");
                        }
                        item->SetText(label.String());
                    } else if (strstr(item->GetPath(), "[SPAM]") != NULL) {
                        if (spamCount > 0) {
                            label.SetToFormat(B_TRANSLATE("Spam (%ld)"), spamCount);
                        } else {
                            label = B_TRANSLATE("Spam");
                        }
                        item->SetText(label.String());
                    }
                    // Custom queries handled below
                }
            }
            
            // Update custom query labels from thread results
            BString customPath, customBaseName;
            int32 customCount;
            int32 ci = 0;
            while (message->FindString("custom_path", ci, &customPath) == B_OK &&
                   message->FindString("custom_base_name", ci, &customBaseName) == B_OK &&
                   message->FindInt32("custom_count", ci, &customCount) == B_OK) {
                // Find matching QueryItem by path
                for (int32 i = 0; i < fQueryList->CountItems(); i++) {
                    BListItem* listItem = fQueryList->ItemAt(i);
                    QueryItem* item = dynamic_cast<QueryItem*>(listItem);
                    if (item && item->IsCustomQuery() && customPath == item->GetPath()) {
                        BString label;
                        if (customCount > 0) {
                            label.SetToFormat("%s (%ld)", customBaseName.String(), customCount);
                        } else {
                            label = customBaseName;
                        }
                        item->SetText(label.String());
                        break;
                    }
                }
                ci++;
            }
            
            fQueryList->Invalidate();
            
            // Update window title with new email count
            BString title(B_TRANSLATE_SYSTEM_NAME(kAppName));
            title << " - ";
            static BStringFormat format(B_TRANSLATE("{0, plural,"
                "one{# new email}"
                "other{# new emails}}"));
            format.Format(title, fCachedNewCount);
            SetTitle(title.String());
            break;
        }
        
        case MSG_ABOUT: {
            // Load the app icon at 64x64
            BBitmap* icon = NULL;
            BResources* resources = BApplication::AppResources();
            if (resources) {
                size_t size;
                const void* data = resources->LoadResource('VICN', "BEOS:ICON", &size);
                if (data && size > 0) {
                    icon = new BBitmap(BRect(0, 0, 63, 63), B_RGBA32);
                    if (BIconUtils::GetVectorIcon((const uint8*)data, size, icon) != B_OK) {
                        delete icon;
                        icon = NULL;
                    }
                }
            }
            
            AboutWindow* aboutWindow = new AboutWindow(icon);
            aboutWindow->Show();
            break;
        }
        
        case MSG_VIEW_HTML_MESSAGE: {
            // Open stored HTML content in Web+
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
        
        case MSG_NEW_EMAIL:
        case MSG_CREATE_EMAIL: {
            // Open our compose window
            be_app->PostMessage(M_NEW);
            break;
        }
        
        case MSG_EMAIL_SETTINGS: {
            // Launch Haiku's E-mail preferences
            be_roster->Launch("application/x-vnd.Haiku-Mail");
            break;
        }
        
        case MSG_CHECK_EMAIL: {
            // Check for mail and send queued messages
            BMailDaemon().CheckAndSendQueuedMail();
            break;
        }
        
        case MSG_SEARCH_MODIFIED: {
            ApplySearchFilter();
            // Keep focus in search field if text is empty (user cleared it)
            if (!fSearchField->HasText())
                fSearchField->TextView()->MakeFocus(true);
            break;
        }
        
        case MSG_SEARCH_CLEAR: {
            // If a body search is running, abort it first (keep results).
            // The user presses × again to clear the search entirely.
            if (fEmailList->IsBodySearchRunning()) {
                fEmailList->StopBodySearch();
                fEmailList->StopLoadingDots();
                fSearchField->SetLoading(false);
                fSearchField->SetBodySearchRunning(false);
                fSearchField->SetHasResults(fEmailList->CountItems() > 0);
                UpdateEmailCountLabel();
                if (fEmailList->CountItems() == 0)
                    ShowEmptyListMessage(B_TRANSLATE("No emails match your search."));
                else
                    ShowEmailListContent();
                break;
            }
            
            // Also cancel any pending body search that hasn't started yet
            fPendingBodySearch = false;
            fPendingBodySearchText.SetTo("");
            
            bool wasSearchExecuted = fSearchField->IsSearchExecuted();
            fSearchField->SetText("");
            fSearchField->SetSearchExecuted(false);
            fSearchField->SetMatchesMode(false);  // Reset to "contains" mode
            // Only reload if a search was actually executed
            if (wasSearchExecuted)
                ApplySearchFilter();
            // Restore focus to search field for new search
            fSearchField->TextView()->MakeFocus(true);
            break;
        }
        
        case MSG_TIME_RANGE_CHANGED: {
            // Debounce time range changes to avoid race conditions when
            // dragging quickly through multiple steps
            delete fTimeRangeFilterRunner;
            BMessage filterMsg(MSG_APPLY_TIME_RANGE_FILTER);
            fTimeRangeFilterRunner = new BMessageRunner(BMessenger(this), &filterMsg, 100000, 1);  // 100ms delay
            break;
        }
        
        case MSG_APPLY_TIME_RANGE_FILTER: {
            delete fTimeRangeFilterRunner;
            fTimeRangeFilterRunner = NULL;
            ApplyTimeRangeFilter();
            break;
        }
        
        case MSG_DEFERRED_INIT: {
            // Deferred initialization - runs after window is shown for fast startup
            
            // Check for saved view preference before selecting default
            BPath settingsPath;
            BString savedView;
            bool savedIsTrash = false;
            bool hasSavedView = false;
            
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &settingsPath) == B_OK) {
                settingsPath.Append("EmailViews/emailviews_settings");
                BFile settingsFile(settingsPath.Path(), B_READ_ONLY);
                if (settingsFile.InitCheck() == B_OK) {
                    BMessage settings;
                    if (settings.Unflatten(&settingsFile) == B_OK) {
                        if (settings.FindBool("selected_view_is_trash", &savedIsTrash) == B_OK) {
                            hasSavedView = true;
                            if (!savedIsTrash) {
                                settings.FindString("selected_view", &savedView);
                            }
                        }
                    }
                }
            }
            
            // Select saved view or default to "All Emails" (first item)
            bool viewRestored = false;
            
            if (hasSavedView && savedIsTrash) {
                // Restore Trash view
                fTrashItem->SetSelected(true);
                fQueryList->DeselectAll();
                fShowTrashOnly = true;
                fShowSpamOnly = false;
                fEmailList->SetShowingTrash(true);
                fEmailList->SetShowingSpam(false);
                fCurrentViewItem = NULL;
                LoadColumnPrefsForView(NULL);
                LoadTrashEmails();
                viewRestored = true;
            } else if (hasSavedView && savedView.Length() > 0) {
                // Try to find and select the saved view
                for (int32 i = 0; i < fQueryList->CountItems(); i++) {
                    BListItem* listItem = fQueryList->ItemAt(i);
                    QueryItem* item = dynamic_cast<QueryItem*>(listItem);
                    if (item && item->IsQuery() && savedView == item->GetPath()) {
                        fQueryList->Select(i);
                        fCurrentViewItem = item;
                        LoadColumnPrefsForView(item);
                        
                        // For custom queries, read the actual query string from file attribute
                        if (item->IsCustomQuery()) {
                            BNode node(item->GetPath());
                            if (node.InitCheck() == B_OK) {
                                char queryBuffer[512];
                                ssize_t size = node.ReadAttr("_trk/qrystr", B_STRING_TYPE, 0, queryBuffer, sizeof(queryBuffer) - 1);
                                if (size > 0) {
                                    queryBuffer[size] = '\0';
                                    fBaseQuery = queryBuffer;
                                    ApplySearchFilter();
                                }
                            }
                        } else {
                            // Built-in queries - GetPath() is the query string
                            BString queryPath = item->GetPath();
                            if (queryPath.StartsWith("[ATTACHMENTS]")) {
                                fAttachmentsOnly = true;
                                queryPath.RemoveFirst("[ATTACHMENTS]");
                            }
                            if (queryPath.StartsWith("[SPAM]")) {
                                fShowSpamOnly = true;
                                queryPath.RemoveFirst("[SPAM]");
                            }
                            fBaseQuery = queryPath;
                            fEmailList->SetShowingSpam(fShowSpamOnly);
                            ApplySearchFilter();
                        }
                        viewRestored = true;
                        break;
                    }
                }
            }
            
            // Fall back to "All Emails" if saved view not found
            if (!viewRestored && fQueryList->CountItems() > 0) {
                fQueryList->Select(0);
                BListItem* listItem = fQueryList->ItemAt(0);
                QueryItem* item = dynamic_cast<QueryItem*>(listItem);
                if (item && item->IsQuery()) {
                    fCurrentViewItem = item;
                    LoadColumnPrefsForView(item);
                    fBaseQuery = item->GetPath();
                    fprintf(stderr, "MSG_DEFERRED_INIT: Fallback to All Emails, query: %s\n", fBaseQuery.String());
                    ApplySearchFilter();
                }
            }
            
            // Start watching for Mail app quit (for deferred read marking)
            be_roster->StartWatching(BMessenger(this), B_REQUEST_QUIT);
            
            // Start watching for volume mount/unmount events
            fVolumeRoster.StartWatching(BMessenger(this));
            
            // Start background live queries for new mail count updates
            fBackgroundQueryHandler = new BackgroundQueryHandler(this);
            AddHandler(fBackgroundQueryHandler);
            
            // Spawn a thread to create and consume initial query results
            thread_id bgThread = spawn_thread(_InitBackgroundQueriesThread,
                "bg_query_init", B_LOW_PRIORITY, this);
            if (bgThread >= 0)
                resume_thread(bgThread);
            
            // Restore Deskbar preference
            BPath deskbarPath;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &deskbarPath) == B_OK) {
                deskbarPath.Append("EmailViews/emailviews_settings");
                BFile deskbarFile(deskbarPath.Path(), B_READ_ONLY);
                if (deskbarFile.InitCheck() == B_OK) {
                    BMessage deskbarSettings;
                    if (deskbarSettings.Unflatten(&deskbarFile) == B_OK) {
                        if (deskbarSettings.FindBool("show_in_deskbar", &fShowInDeskbar) == B_OK) {
                            if (fDeskbarMenuItem)
                                fDeskbarMenuItem->SetMarked(fShowInDeskbar);
                        }
                    }
                }
            }
            
            // Update sidebar query counts (deferred so window is already visible)
            ScheduleQueryCountUpdate();
            
            // Trigger a mail check after a short delay so the UI is fully loaded first
            BMessage checkMsg(MSG_CHECK_EMAIL);
            BMessageRunner::StartSending(BMessenger(this), &checkMsg, 2000000, 1);
            break;
        }
        
        case MSG_INIT_PREVIEW_PANE: {
            // Initialize preview pane if no email is selected
            if (fEmailList->CurrentSelection() == NULL) {
                ClearPreviewPane();
            }
            break;
        }
        
        case MSG_SEARCH_ADD_QUERY: {
            // Create a query from the current search
            BString searchText = fSearchField->Text();
            if (searchText.IsEmpty())
                break;
            
            // Get the search attribute for naming
            SearchAttribute attr = fSearchField->GetSearchAttribute();
            const char* attrName;
            switch (attr) {
                case SEARCH_FROM:
                    attrName = B_TRANSLATE_COMMENT("From", "Attribute name");
                    break;
                case SEARCH_TO:
                    attrName = B_TRANSLATE_COMMENT("To", "Attribute name");
                    break;
                case SEARCH_ACCOUNT:
                    attrName = B_TRANSLATE_COMMENT("Account", "Attribute name");
                    break;
                case SEARCH_SUBJECT:
                default:
                    attrName = B_TRANSLATE_COMMENT("Subject", "Attribute name");
                    break;
            }
            
            // Create default query name from attribute and search text
            BString defaultName = B_TRANSLATE_COMMENT("'%attributename%' %operator%: '%searchtext%'",
                "Default query filename, e.g. 'Subject' contains 'Some text'");
            const char* operatorStr = fSearchField->IsMatchesMode()
                ? B_TRANSLATE_COMMENT("matches", "The filter operator")
                : B_TRANSLATE_COMMENT("contains", "The filter operator");
            defaultName.ReplaceFirst("%attributename%", attrName);
            defaultName.ReplaceFirst("%operator%", operatorStr);
            defaultName.ReplaceFirst("%searchtext%", searchText);
            // Truncate if too long
            if (defaultName.Length() > 60)
                defaultName.Truncate(57).Append("…");
            
            // Ask user for query name
            QueryNameDialog* dialog = new QueryNameDialog(
                B_TRANSLATE("Save filter as query"),
                B_TRANSLATE("Query name:"),
                defaultName.String());
            
            BString queryName;
            if (!dialog->Go(queryName))
                break;  // User cancelled
            
            if (queryName.IsEmpty())
                break;
            
            // Sanitize the filename (remove invalid characters)
            // '/' is the path separator and cannot be in filenames
            queryName.ReplaceAll("/", "_");
            
            // Get the queries directory path
            BPath queriesPath;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &queriesPath) != B_OK)
                break;
            queriesPath.Append("EmailViews/queries");
            create_directory(queriesPath.Path(), 0755);
            
            // Create the query file path
            BPath queryFilePath(queriesPath);
            if (queryFilePath.Append(queryName.String()) != B_OK)
                break;
            
            // Build the query predicate first (needed for duplicate check)
            BString predicate = MakeCaseInsensitivePattern(searchText.String());
            BString queryPredicate;
            bool matchesMode = fSearchField->IsMatchesMode();
            
            if (matchesMode) {
                // Exact match mode
                BString escapedText = searchText;
                escapedText.ReplaceAll("\"", "\\\"");
                
                switch (attr) {
                    case SEARCH_SUBJECT:
                        queryPredicate << "(MAIL:subject==\"*" << escapedText << "*\")";
                        break;
                    case SEARCH_FROM:
                        queryPredicate << "(MAIL:from==\"*" << escapedText << "*\")";
                        break;
                    case SEARCH_TO:
                        queryPredicate << "(MAIL:to==\"*" << escapedText << "*\")";
                        break;
                    case SEARCH_ACCOUNT:
                        queryPredicate << "(MAIL:account==\"" << escapedText << "\")";
                        break;
                    default:
                        queryPredicate << "(MAIL:subject==\"*" << escapedText << "*\")";
                        break;
                }
            } else {
                // Contains mode - use case-insensitive wildcard pattern
                switch (attr) {
                    case SEARCH_SUBJECT:
                        queryPredicate << "(MAIL:subject==\"*" << predicate << "*\")";
                        break;
                    case SEARCH_FROM:
                        queryPredicate << "(MAIL:from==\"*" << predicate << "*\")";
                        break;
                    case SEARCH_TO:
                        queryPredicate << "(MAIL:to==\"*" << predicate << "*\")";
                        break;
                    case SEARCH_ACCOUNT:
                        queryPredicate << "(MAIL:account==\"*" << predicate << "*\")";
                        break;
                    default:
                        queryPredicate << "(MAIL:subject==\"*" << predicate << "*\")";
                        break;
                }
            }
            
            // Abort if we couldn't build a valid predicate
            if (queryPredicate.IsEmpty())
                break;
            
            // Check for duplicate name
            BEntry existingEntry(queryFilePath.Path());
            bool nameExists = existingEntry.Exists();
            
            // Check for duplicate criteria in existing queries
            BString existingQueryWithSameCriteria;
            BDirectory queriesDir(queriesPath.Path());
            if (queriesDir.InitCheck() == B_OK) {
                BEntry entry;
                while (queriesDir.GetNextEntry(&entry) == B_OK) {
                    BNode node(&entry);
                    if (node.InitCheck() == B_OK) {
                        BString existingPredicate;
                        if (node.ReadAttrString("_trk/qrystr", &existingPredicate) == B_OK) {
                            if (existingPredicate == queryPredicate) {
                                char name[B_FILE_NAME_LENGTH];
                                entry.GetName(name);
                                existingQueryWithSameCriteria = name;
                                break;
                            }
                        }
                    }
                }
            }
            
            // Show warning if duplicate found
            if (nameExists || existingQueryWithSameCriteria.Length() > 0) {
                BString warningText;
                if (existingQueryWithSameCriteria.Length() > 0) {
                    BString fmt(B_TRANSLATE("A query with the same search criteria already exists: '%name%'"));
                    fmt.ReplaceAll("%name%", existingQueryWithSameCriteria);
                    warningText = fmt;
                } else {
                    BString fmt(B_TRANSLATE("A query named '%name%' already exists."));
                    fmt.ReplaceAll("%name%", queryName);
                    warningText = fmt;
                }
                
                BAlert* alert = new BAlert(B_TRANSLATE("Query exists"),
                    warningText.String(),
                    B_TRANSLATE("Cancel"), B_TRANSLATE("Create anyway"), NULL,
                    B_WIDTH_AS_USUAL, B_WARNING_ALERT);
                int32 result = alert->Go();
                if (result == 0)  // Cancel
                    break;
            }
            
            // Create the query file
            BFile queryFile(queryFilePath.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
            if (queryFile.InitCheck() != B_OK) {
                BAlert* errAlert = new BAlert(B_TRANSLATE("Error"),
                    B_TRANSLATE("Failed to create query file."), B_TRANSLATE("OK"),
                    NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);
                errAlert->Go();
                break;
            }
            
            // Set the query attributes
            queryFile.WriteAttrString("_trk/qrystr", &queryPredicate);
            queryFile.WriteAttrString("_trk/qryinitstr", &queryPredicate);
            
            // Set MIME type to search only emails
            BString emailMime("E-mail");
            queryFile.WriteAttrString("_trk/qryinitmime", &emailMime);
            
            // Set mode to "by formula" (Tracker's 'Fbyq' constant)
            int32 formulaMode = 'Fbyq';
            queryFile.WriteAttr("_trk/qryinitmode", B_INT32_TYPE, 0, &formulaMode, sizeof(formulaMode));
            
            // Don't write volume attributes - let Tracker default to All disks
            
            // Set the file type to query
            BNodeInfo nodeInfo(&queryFile);
            nodeInfo.SetType("application/x-vnd.Be-query");
            
            // Close the query file
            queryFile.Unset();
            
            // Reset search state
            fSearchField->SetText("");
            fSearchField->SetSearchExecuted(false);
            fSearchField->SetHasResults(false);
            
            // Force immediate folder list reload
            LoadQueries();
            
            // Select the newly created query
            BString newQueryPath = queryFilePath.Path();
            for (int32 i = 0; i < fQueryList->CountItems(); i++) {
                QueryItem* item = dynamic_cast<QueryItem*>(fQueryList->ItemAt(i));
                if (item && newQueryPath == item->GetPath()) {
                    fQueryList->Select(i);
                    // Trigger folder selection to load the query results
                    BMessage selectMsg(MSG_QUERY_SELECTED);
                    PostMessage(&selectMsg);
                    break;
                }
            }
            
            break;
        }
        
        case MSG_BACKUP_EMAILS: {
            // Create ZIP backup of current search results
            int32 count = fEmailList->CountItems();
            if (count == 0)
                break;
            
            // Generate default filename with current date
            time_t now = time(NULL);
            struct tm* tm = localtime(&now);
            char dateStr[32];
            strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", tm);
            
            BString defaultName(B_TRANSLATE("Email backup"));
            defaultName << " - " << dateStr << ".zip";
            
            // Show save file panel
            BFilePanel* savePanel = new BFilePanel(B_SAVE_PANEL, 
                new BMessenger(this), NULL, 0, false, 
                new BMessage('svbk'), NULL, true, true);
            savePanel->SetSaveText(defaultName.String());
            savePanel->Window()->SetTitle(B_TRANSLATE("Save backup as"));
            savePanel->Show();
            break;
        }
        
        case 'svbk': {
            // Handle save panel result for backup
            entry_ref dirRef;
            BString fileName;
            if (message->FindRef("directory", &dirRef) != B_OK)
                break;
            if (message->FindString("name", &fileName) != B_OK)
                break;
            
            // Ensure .zip extension (case-insensitive check)
            int32 len = fileName.Length();
            bool hasZipExt = (len >= 4 && fileName.IFindLast(".zip") == len - 4);
            if (!hasZipExt)
                fileName.Append(".zip");
            
            BPath savePath(&dirRef);
            savePath.Append(fileName.String());
            
            // Collect email file paths
            int32 count = fEmailList->CountItems();
            if (count == 0)
                break;
            
            // Collect paths into a BString array for the worker thread
            BString* paths = new BString[count];
            for (int32 i = 0; i < count; i++) {
                EmailItem* row = fEmailList->ItemAt(i);
                if (row)
                    paths[i] = row->GetPath();
            }
            
            // Create data structure for zip worker thread
            ZipWorkerData* workerData = new ZipWorkerData();
            workerData->savePath = savePath.Path();
            workerData->paths = paths;
            workerData->count = count;
            workerData->messenger = BMessenger(this);
            
            // Spawn worker thread to run zip with piped input
            // This avoids command line length limits by using zip's -@ option
            thread_id workerThread = spawn_thread(ZipWorkerThread,
                "zip_worker", B_NORMAL_PRIORITY, workerData);
            
            if (workerThread >= 0) {
                resume_thread(workerThread);
                // Show backup-in-progress animation on search bar
                fSearchField->SetBackupActive(true);
            } else {
                delete[] paths;
                delete workerData;
                BAlert* alert = new BAlert(B_TRANSLATE("Backup failed"), 
                    B_TRANSLATE("Failed to start backup thread."), 
                    B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);
                alert->Go();
            }
            break;
        }
        
        case MSG_BACKUP_FINISHED: {
            // Zip worker thread finished — stop backup animation
            fSearchField->SetBackupActive(false);
            break;
        }
        
        case MSG_TOGGLE_DESKBAR: {
            fShowInDeskbar = !fShowInDeskbar;
            fDeskbarMenuItem->SetMarked(fShowInDeskbar);
            
            if (fShowInDeskbar) {
                _AddToDeskbar();
            } else {
                _RemoveFromDeskbar();
            }
            break;
        }
        
        case 'dhid': {
            // Deskbar replicant was hidden via its context menu
            fShowInDeskbar = false;
            fDeskbarMenuItem->SetMarked(false);
            break;
        }
        
        case MSG_QUIT: {
            PostMessage(B_QUIT_REQUESTED);
            break;
        }
        
        case MSG_TOGGLE_TIME_RANGE: {
            if (gReaderSettings != NULL && fTimeRangeGroup != NULL) {
                bool show = !gReaderSettings->ShowTimeRange();
                gReaderSettings->SetShowTimeRange(show);
                
                if (show) {
                    fTimeRangeGroup->Show();
                    fTimeRangeMenuItem->SetMarked(true);
                } else {
                    // Check if filtering was active before resetting
                    bool wasFiltering = !fTimeRangeSlider->IsFullRange();
                    
                    // Reset to full range before hiding
                    fTimeRangeSlider->SetLeftValue(0.0f);
                    fTimeRangeSlider->SetRightValue(1.0f);
                    fTimeRangeGroup->Hide();
                    fTimeRangeMenuItem->SetMarked(false);
                    
                    // Reload emails only if filtering was active
                    if (wasFiltering) {
                        ApplyTimeRangeFilter();
                    }
               }
            }
            break;
        }
        
        case M_FONT: {
            // Update preview pane font when font settings change
            if (fPreviewPane != NULL && gReaderSettings != NULL) {
                rgb_color textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
                fPreviewPane->SelectAll();
                fPreviewPane->SetFontAndColor(&gReaderSettings->ContentFont(), B_FONT_ALL, &textColor);
                fPreviewPane->Select(0, 0);
            }
            break;
        }
        
        case PREFS_CHANGED: {
            // Update toolbar when preferences change
            _UpdateToolBar();
            
            // Reload spam blocklist — only update view if it changed
            std::set<BString> oldBlocklist = fSpamBlocklist;
            LoadSpamBlocklist();
            
            if (fSpamBlocklist != oldBlocklist && fCurrentViewItem != NULL
                && !fShowTrashOnly) {
                // Compute added and removed entries
                std::set<BString> added;    // newly blocked senders
                std::set<BString> removed;  // newly unblocked senders

                for (auto it = fSpamBlocklist.begin();
                    it != fSpamBlocklist.end(); ++it) {
                    if (oldBlocklist.count(*it) == 0)
                        added.insert(*it);
                }
                for (auto it = oldBlocklist.begin();
                    it != oldBlocklist.end(); ++it) {
                    if (fSpamBlocklist.count(*it) == 0)
                        removed.insert(*it);
                }

                // In normal views: remove newly blocked, add newly unblocked
                // In Spam view: remove newly unblocked, add newly blocked
                std::set<BString>& sendersToRemove =
                    fShowSpamOnly ? removed : added;
                std::set<BString>& sendersToAdd =
                    fShowSpamOnly ? added : removed;

                // Remove rows matching sendersToRemove (in-place, no query)
                if (!sendersToRemove.empty()) {
                    fEmailList->DeselectAll();
                    fEmailList->BeginBatchRemove();

                    BList rowsToRemove;
                    for (int32 i = fEmailList->CountItems() - 1; i >= 0; i--) {
                        EmailItem* item = fEmailList->ItemAt(i);
                        if (item == NULL)
                            continue;
                        const char* from = item->GetFrom();
                        if (from == NULL || from[0] == '\0')
                            continue;
                        BString addr = _ExtractEmailAddress(from);
                        if (sendersToRemove.count(addr) > 0)
                            rowsToRemove.AddItem(item);
                    }

                    for (int32 i = 0; i < rowsToRemove.CountItems(); i++) {
                        EmailItem* item = (EmailItem*)rowsToRemove.ItemAt(i);
                        if (item)
                            fEmailList->RemoveRow(item);
                    }
                    fEmailList->EndBatchRemove();

                    UpdateEmailCountLabel();
                    if (fEmailList->CountItems() == 0) {
                        if (fShowSpamOnly)
                            ShowEmptyListMessage(
                                B_TRANSLATE("No spam emails."));
                        else
                            ShowEmptyListMessage(
                                B_TRANSLATE("No emails match this query."));
                    }
                }

                // Add emails from sendersToAdd into the visible list
                if (!sendersToAdd.empty()) {
                    // Build trash path for exclusion
                    BPath trashPath;
                    find_directory(B_TRASH_DIRECTORY, &trashPath);
                    BString trashStr(trashPath.Path());

                    // Query using the current view's base predicate
                    for (int32 v = 0; v < fSelectedVolumes.CountItems();
                        v++) {
                        BVolume* vol = fSelectedVolumes.ItemAt(v);
                        if (vol == NULL || !vol->KnowsQuery())
                            continue;

                        BQuery query;
                        query.SetVolume(vol);
                        query.SetPredicate(fBaseQuery.String());

                        if (query.Fetch() != B_OK)
                            continue;

                        entry_ref ref;
                        while (query.GetNextRef(&ref) == B_OK) {
                            // Skip if in Trash
                            BEntry entry(&ref);
                            BPath path;
                            if (entry.GetPath(&path) != B_OK)
                                continue;
                            BString pathStr(path.Path());
                            if (pathStr.FindFirst(trashStr) >= 0)
                                continue;

                            BNode node(&ref);
                            if (node.InitCheck() != B_OK)
                                continue;

                            // Check sender matches sendersToAdd
                            char fromBuf[256] = "";
                            node.ReadAttr("MAIL:from", B_STRING_TYPE, 0,
                                fromBuf, sizeof(fromBuf) - 1);
                            if (fromBuf[0] == '\0')
                                continue;
                            BString addr = _ExtractEmailAddress(fromBuf);
                            if (sendersToAdd.count(addr) == 0) {
                                // Also check @domain match
                                int32 at = addr.FindFirst('@');
                                if (at < 0)
                                    continue;
                                BString domain;
                                addr.CopyInto(domain, at,
                                    addr.Length() - at);
                                if (sendersToAdd.count(domain) == 0)
                                    continue;
                            }

                            // Check spam classification to match view
                            BString classification;
                            bool isSpam =
                                (node.ReadAttrString("MAIL:classification",
                                    &classification) == B_OK
                                && classification.ICompare("Spam") == 0);
                            if (fShowSpamOnly && !isSpam)
                                continue;
                            if (!fShowSpamOnly && isSpam)
                                continue;

                            // Skip if already in list
                            node_ref nodeRef;
                            entry.GetNodeRef(&nodeRef);
                            if (fEmailList->ItemFor(nodeRef) != NULL)
                                continue;

                            EmailRef* emailRef = new EmailRef(ref);
                            fEmailList->AddEmailSorted(emailRef);
                        }
                    }

                    ShowEmailListContent();
                    UpdateEmailCountLabel();
                    fEmailList->Invalidate();
                }

                ScheduleQueryCountUpdate();
            }
            
            // Update time range slider visibility
            if (gReaderSettings != NULL && fTimeRangeGroup != NULL) {
                bool show = gReaderSettings->ShowTimeRange();
                if (show && fTimeRangeGroup->IsHidden()) {
                    fTimeRangeGroup->Show();
                    fTimeRangeMenuItem->SetMarked(true);
                } else if (!show && !fTimeRangeGroup->IsHidden()) {
                    // Check if filtering was active before hiding
                    bool wasFiltering = !fTimeRangeSlider->IsFullRange();
                    
                    // Reset to full range
                    fTimeRangeSlider->SetLeftValue(0.0f);
                    fTimeRangeSlider->SetRightValue(1.0f);
                    
                    fTimeRangeGroup->Hide();
                    fTimeRangeMenuItem->SetMarked(false);
                   
                    // Refresh email list only if filtering was active
                    if (wasFiltering && fCurrentViewItem != NULL) {
                        ResolveBaseQuery(fCurrentViewItem);
                        ExecuteQuery();
                    }
                }
            }
            break;
        }
        
        case MSG_VOLUME_SELECTED: {
            int32 device;
            if (message->FindInt32("device", &device) == B_OK) {
                bool wasSelected = IsVolumeSelected(device);
                SetVolumeSelected(device, !wasSelected);
                UpdateVolumeMenu();
                SaveVolumeSelection();
                
                // Reload current view with new volume selection
                if (fShowTrashOnly) {
                    LoadTrashEmails();
                } else if (fCurrentViewItem) {
                    ResolveBaseQuery(fCurrentViewItem);
                    ExecuteQuery();
                }
                
                // Clear existing background queries (fast — just frees memory)
                for (int32 i = 0; i < fBackgroundNewMailQueries.CountItems(); i++) {
                    BQuery* query = fBackgroundNewMailQueries.ItemAt(i);
                    if (query)
                        query->Clear();
                }
                fBackgroundNewMailQueries.MakeEmpty();
                
                // Restart background queries on a background thread
                // (consuming initial results is heavy I/O on cold volumes)
                thread_id bgThread = spawn_thread(_InitBackgroundQueriesThread,
                    "bg_query_reinit", B_LOW_PRIORITY, this);
                if (bgThread >= 0)
                    resume_thread(bgThread);
            }
            break;
        }
        
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool EmailViewsWindow::QuitRequested()
{
    // Save column prefs for current view
    SaveColumnPrefsForView(fCurrentViewItem);
    
    SaveWindowState();
    
    // Clean up all attachment temp files
    BPath tempPath;
    if (find_directory(B_SYSTEM_TEMP_DIRECTORY, &tempPath) == B_OK) {
        tempPath.Append("EmailViews_attachments");
        BDirectory dir(tempPath.Path());
        if (dir.InitCheck() == B_OK) {
            BEntry entry;
            while (dir.GetNextEntry(&entry) == B_OK) {
                entry.Remove();
            }
        }
    }
    
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}


// Property info for Tracker-compatible scripting (Mail Next/Previous navigation)
static const property_info kEmailViewsPropertyList[] = {
    {
        "Entry",
        { B_GET_PROPERTY, 0 },
        { kNextSpecifier, kPreviousSpecifier, 0 },
        "get Entry [next|previous] # returns next/previous email entry",
        0,
        { B_REF_TYPE },
        {},
        {}
    },
    {
        "Selection",
        { B_SET_PROPERTY, 0 },
        { B_DIRECT_SPECIFIER, 0 },
        "set Selection to entry_ref # selects the specified entry",
        0,
        {},
        {},
        {}
    },
    { 0 }
};


BHandler*
EmailViewsWindow::ResolveSpecifier(BMessage* message, int32 index,
    BMessage* specifier, int32 form, const char* property)
{
    BPropertyInfo propertyInfo(const_cast<property_info*>(kEmailViewsPropertyList));
    
    int32 result = propertyInfo.FindMatch(message, index, specifier, form, property);
    if (result >= 0)
        return this;
    
    return BWindow::ResolveSpecifier(message, index, specifier, form, property);
}


// Tracker scripting support for Mail Next/Previous navigation
bool EmailViewsWindow::_HandleTrackerScripting(BMessage* message)
{
    BMessage specifier;
    int32 form;
    const char* property;
    
    if (message->GetCurrentSpecifier(NULL, &specifier, &form, &property) != B_OK)
        return false;
    
    // Only handle "Entry" property with next/previous specifiers
    if (strcmp(property, "Entry") != 0)
        return false;
    
    if (form != (int32)kNextSpecifier && form != (int32)kPreviousSpecifier)
        return false;
    
    // Get the reference entry_ref from the specifier
    entry_ref ref;
    if (specifier.FindRef("data", &ref) != B_OK) {
        BMessage reply(B_REPLY);
        reply.AddInt32("error", B_BAD_VALUE);
        message->SendReply(&reply);
        return true;
    }
    
    // Find the row with this ref
    int32 index = _FindEmailRowIndexByRef(&ref);
    if (index < 0) {
        BMessage reply(B_REPLY);
        reply.AddInt32("error", B_ENTRY_NOT_FOUND);
        message->SendReply(&reply);
        return true;
    }
    
    // Get next or previous index
    if (form == (int32)kNextSpecifier)
        index++;
    else
        index--;
    
    // Get the row at the new index
    EmailItem* row = fEmailList->ItemAt(index);
    if (row == NULL) {
        BMessage reply(B_REPLY);
        reply.AddInt32("error", B_ENTRY_NOT_FOUND);
        message->SendReply(&reply);
        return true;
    }
    
    // Get entry_ref from the row's path
    BEntry entry(row->GetPath());
    entry_ref resultRef;
    if (entry.GetRef(&resultRef) != B_OK) {
        BMessage reply(B_REPLY);
        reply.AddInt32("error", B_ENTRY_NOT_FOUND);
        message->SendReply(&reply);
        return true;
    }
    
    // Send successful reply
    BMessage reply(B_REPLY);
    reply.AddRef("result", &resultRef);
    reply.AddInt32("index", index);
    message->SendReply(&reply);
    return true;
}


int32 EmailViewsWindow::_FindEmailRowIndexByRef(const entry_ref* ref) const
{
    if (ref == NULL)
        return -1;
    
    // Try O(1) lookup by node_ref first
    BEntry entry(ref);
    node_ref nref;
    if (entry.GetNodeRef(&nref) == B_OK) {
        int32 index = fEmailList->IndexOf(nref);
        if (index >= 0)
            return index;
    }
    
    // Fallback: search by path
    BPath path;
    if (entry.GetPath(&path) != B_OK)
        return -1;
    
    const char* refPath = path.Path();
    if (refPath == NULL)
        return -1;
    
    int32 count = fEmailList->CountItems();
    for (int32 i = 0; i < count; i++) {
        EmailItem* row = fEmailList->ItemAt(i);
        if (row && strcmp(row->GetPath(), refPath) == 0)
            return i;
    }
    
    return -1;
}


bool EmailViewsWindow::GetNextEmailRef(const entry_ref* current, entry_ref* next)
{
    if (current == NULL || next == NULL)
        return false;
    
    // Must lock window when accessing views from another thread
    if (!LockLooper())
        return false;
    
    int32 index = _FindEmailRowIndexByRef(current);
    if (index < 0) {
        UnlockLooper();
        return false;
    }
    
    // Get next row (index + 1)
    int32 nextIndex = index + 1;
    if (nextIndex >= fEmailList->CountItems()) {
        UnlockLooper();
        return false;
    }
    
    EmailItem* nextRow = fEmailList->ItemAt(nextIndex);
    if (nextRow == NULL) {
        UnlockLooper();
        return false;
    }
    
    BEntry entry(nextRow->GetPath());
    bool result = entry.GetRef(next) == B_OK;
    UnlockLooper();
    return result;
}


bool EmailViewsWindow::GetPrevEmailRef(const entry_ref* current, entry_ref* prev)
{
    if (current == NULL || prev == NULL)
        return false;
    
    // Must lock window when accessing views from another thread
    if (!LockLooper())
        return false;
    
    int32 index = _FindEmailRowIndexByRef(current);
    if (index < 0) {
        UnlockLooper();
        return false;
    }
    
    // Get previous row (index - 1)
    int32 prevIndex = index - 1;
    if (prevIndex < 0) {
        UnlockLooper();
        return false;
    }
    
    EmailItem* prevRow = fEmailList->ItemAt(prevIndex);
    if (prevRow == NULL) {
        UnlockLooper();
        return false;
    }
    
    BEntry entry(prevRow->GetPath());
    bool result = entry.GetRef(prev) == B_OK;
    UnlockLooper();
    return result;
}


bool EmailViewsWindow::HasNextEmail(const entry_ref* current)
{
    if (current == NULL)
        return false;

    int32 index = _FindEmailRowIndexByRef(current);
    if (index < 0)
        return false;

    return (index + 1) < fEmailList->CountItems();
}


bool EmailViewsWindow::HasPrevEmail(const entry_ref* current)
{
    if (current == NULL)
        return false;

    int32 index = _FindEmailRowIndexByRef(current);
    if (index < 0)
        return false;

    return index > 0;
}


void EmailViewsWindow::SelectEmailByRef(const entry_ref* ref)
{
    if (ref == NULL)
        return;
    
    // Must lock window when accessing views from another thread
    if (!LockLooper())
        return;
    
    int32 index = _FindEmailRowIndexByRef(ref);
    if (index < 0) {
        UnlockLooper();
        return;
    }
    
    fEmailList->DeselectAll();
    fEmailList->Select(index);
    fEmailList->ScrollToItem(index);
    UnlockLooper();
}


void EmailViewsWindow::_NavigateQueryList(int32 direction)
{
    int32 count = fQueryList->CountItems();
    if (count == 0)
        return;

    bool trashHasItems = (fCachedTrashCount > 0);

    // Determine starting point: current query list selection, or trash if
    // the trash view is active, or -1/count to seed first navigation
    int32 current = fQueryList->CurrentSelection();
    if (current < 0 && fShowTrashOnly) {
        // Trash is selected (it's not in the list view).
        // Up from trash goes to the last selectable query item.
        // Down from trash does nothing (trash is the bottom, no wrapping).
        if (direction < 0) {
            for (int32 i = count - 1; i >= 0; i--) {
                if (fQueryList->ItemAt(i)->IsEnabled()) {
                    fQueryList->Select(i);
                    fQueryList->ScrollToSelection();
                    PostMessage(MSG_QUERY_SELECTED);
                    return;
                }
            }
        }
        return;
    }

    if (current < 0) {
        // Nothing selected at all — seed so the first step lands on an item
        current = (direction > 0) ? -1 : count;
    }

    // Walk in the requested direction, skipping disabled items (separators)
    int32 next = current;
    for (;;) {
        next += direction;

        // Down past the last item → select trash if non-empty, else stop
        if (next >= count) {
            if (trashHasItems) {
                fQueryList->DeselectAll();
                PostMessage(MSG_TRASH_SELECTED);
            }
            return;
        }

        // Up past the first item → stop (no wrapping)
        if (next < 0)
            return;

        BListItem* item = fQueryList->ItemAt(next);
        if (item != NULL && item->IsEnabled()) {
            fQueryList->Select(next);
            fQueryList->ScrollToSelection();
            PostMessage(MSG_QUERY_SELECTED);
            return;
        }
        // Skip disabled items (separators) and keep going
    }
}


bool EmailViewsWindow::SelectBuiltInQueryByName(const char* name)
{
    if (name == NULL)
        return false;
    
    // Iterate through query list looking for matching built-in query
    for (int32 i = 0; i < fQueryList->CountItems(); i++) {
        QueryItem* item = dynamic_cast<QueryItem*>(fQueryList->ItemAt(i));
        if (item && item->IsQuery() && !item->IsCustomQuery()) {
            if (strcmp(item->GetBaseName(), name) == 0) {
                // Select the item in the list (visual feedback)
                fQueryList->Select(i);
                
                // Skip if same view is already selected
                if (item == fCurrentViewItem && !fShowTrashOnly)
                    return true;
                
                // Save column prefs for current view before switching
                SaveColumnPrefsForView(fCurrentViewItem);
                
                // Stop watching trash directory if we were viewing trash
                if (fShowTrashOnly) {
                    watch_node(&fTrashDirRef, B_STOP_WATCHING, this);
                }
                
                // Deselect trash item when folder is selected
                fTrashItem->SetSelected(false);
                fShowTrashOnly = false;
                fShowSpamOnly = false;
                fEmailList->SetShowingTrash(false);
                fEmailList->SetShowingSpam(false);
                
                // Clear search field when switching folders
                fSearchField->SetText("");
                fSearchField->SetHasResults(false);
                fIsSearchActive = false;
                
                // Clear the preview pane
                ClearPreviewPane();

                // Show empty list message for this view
                ShowEmptyListMessage(B_TRANSLATE("No emails found."));
                
                // Load column prefs for new view
                LoadColumnPrefsForView(item);
                fCurrentViewItem = item;
                
                // For built-in queries, GetPath() returns the query string
                fBaseQuery = item->GetPath();
                // Strip special prefix flags for search compatibility
                fAttachmentsOnly = (fBaseQuery.FindFirst("[ATTACHMENTS]") >= 0);
                fShowSpamOnly = (fBaseQuery.FindFirst("[SPAM]") >= 0);
                fBaseQuery.RemoveFirst("[ATTACHMENTS]");
                fBaseQuery.RemoveFirst("[SPAM]");
                fEmailList->SetShowingSpam(fShowSpamOnly);
                ExecuteQuery();
                
                return true;
            }
        }
    }
    
    return false;
}

bool EmailViewsWindow::SelectBuiltInQueryByIndex(int32 index)
{
    if (index < 0 || index >= fQueryList->CountItems())
        return false;
    
    QueryItem* item = dynamic_cast<QueryItem*>(fQueryList->ItemAt(index));
    if (item == NULL)
        return false;
    
    return SelectBuiltInQueryByName(item->GetBaseName());
}


bool EmailViewsWindow::_HandleSetSelection(BMessage* message)
{
    BMessage specifier;
    int32 form;
    const char* property;
    
    if (message->GetCurrentSpecifier(NULL, &specifier, &form, &property) != B_OK)
        return false;
    
    // Only handle "Selection" property
    if (strcmp(property, "Selection") != 0)
        return false;
    
    // Get the entry_ref from the message data
    entry_ref ref;
    if (message->FindRef("data", &ref) != B_OK) {
        BMessage reply(B_REPLY);
        reply.AddInt32("error", B_BAD_VALUE);
        message->SendReply(&reply);
        return true;
    }
    
    // Find the row with this ref
    int32 index = _FindEmailRowIndexByRef(&ref);
    if (index < 0) {
        BMessage reply(B_REPLY);
        reply.AddInt32("error", B_ENTRY_NOT_FOUND);
        message->SendReply(&reply);
        return true;
    }
    
    // Select the row
    fEmailList->DeselectAll();
    fEmailList->Select(index);
    fEmailList->ScrollToItem(index);
    
    // Send success reply
    BMessage reply(B_REPLY);
    reply.AddInt32("error", B_OK);
    message->SendReply(&reply);
    return true;
}


void EmailViewsWindow::_AddToDeskbar()
{
    BDeskbar deskbar;
    
    if (!deskbar.IsRunning())
        return;
    
    if (deskbar.HasItem(kDeskbarReplicantName))
        return;
    
    app_info appInfo;
    be_app->GetAppInfo(&appInfo);
    deskbar.AddItem(&appInfo.ref);
}

void EmailViewsWindow::_RemoveFromDeskbar()
{
    BDeskbar deskbar;
    int32 id;
    
    if (deskbar.GetItemInfo(kDeskbarReplicantName, &id) == B_OK) {
        deskbar.RemoveItem(id);
    }
}

// Helper to find our own binary image in memory.
// When running as a Deskbar replicant, we're loaded into the Deskbar's
// address space. We need to find our own add-on image to load resources
// (icons, localization catalogs) from it. This uses the standard BeOS/Haiku
// pattern of checking which image contains a known function address.
status_t
our_image(image_info& image)
{
    int32 cookie = 0;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        if ((char*)our_image >= (char*)image.text
            && (char*)our_image <= (char*)image.text + image.text_size)
            return B_OK;
    }
    return B_ERROR;
}

// DeskbarReplicant implementation

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "DeskbarReplicant"

// The replicant runs inside the Deskbar process, so B_TRANSLATE() cannot
// find EmailViews' catalog at runtime.  Instead we use an explicit BCatalog
// initialized with the app signature (see _Init).  The B_TRANSLATE_MARK()
// entries below are no-ops at runtime but let collectcatkeys pick up the
// strings automatically during "make catkeys".
static const char* kDeskbarReplicantStrings[] __attribute__((used)) = {
    B_TRANSLATE_MARK("No new emails"),
    B_TRANSLATE_MARK("1 new email"),
    B_TRANSLATE_MARK("%count% new emails"),
    B_TRANSLATE_MARK("Unread emails"),
    B_TRANSLATE_MARK("Unread emails (%count%)"),
    B_TRANSLATE_MARK("Create new email" B_UTF8_ELLIPSIS),
    B_TRANSLATE_MARK("Send/Receive email"),
    B_TRANSLATE_MARK("Email accounts" B_UTF8_ELLIPSIS),
    B_TRANSLATE_MARK("Remove from Deskbar"),
    B_TRANSLATE_MARK("About %app%" B_UTF8_ELLIPSIS),
    NULL
};

DeskbarReplicant::DeskbarReplicant(BRect frame, int32 resizingMode)
    : BView(frame, kDeskbarReplicantName, resizingMode,
            B_WILL_DRAW | B_TRANSPARENT_BACKGROUND | B_PULSE_NEEDED),
      fIconNoEmail(NULL),
      fIconNewEmail(NULL),
      fNewMailCount(-1)
{
    _Init();
}

DeskbarReplicant::DeskbarReplicant(BMessage* archive)
    : BView(archive),
      fIconNoEmail(NULL),
      fIconNewEmail(NULL),
      fNewMailCount(-1)
{
    _Init();
}

DeskbarReplicant::~DeskbarReplicant()
{
    delete fIconNoEmail;
    delete fIconNewEmail;
}

void DeskbarReplicant::_Init()
{
    // Find our binary image to load resources and catalog
    image_info info;
    if (our_image(info) != B_OK)
        return;
    
    // Load catalog from our binary's embedded resources.
    // B_TRANSLATE() won't work here because the replicant runs inside the
    // Deskbar process, and SetTo(signature) only searches filesystem catalog
    // directories where nothing is installed.  The entry_ref variant of
    // SetTo() reads the catalogs that "make bindcatalogs" embedded into the
    // EmailViews binary.
    BEntry catalogEntry(info.name);
    entry_ref catalogRef;
    if (catalogEntry.GetRef(&catalogRef) == B_OK)
        fCatalog.SetTo(catalogRef);
    
    BFile file(info.name, B_READ_ONLY);
    if (file.InitCheck() != B_OK)
        return;
    
    BResources resources(&file);
    if (resources.InitCheck() != B_OK)
        return;
    
    size_t size;
    BRect iconBounds = Bounds();
    
    // Load "no email" icon
    const void* data = resources.LoadResource(B_VECTOR_ICON_TYPE, "ReplicantIconNoEmail", &size);
    if (data != NULL) {
        fIconNoEmail = new BBitmap(iconBounds, B_RGBA32);
        if (BIconUtils::GetVectorIcon((const uint8*)data, size, fIconNoEmail) != B_OK) {
            delete fIconNoEmail;
            fIconNoEmail = NULL;
        }
    }
    
    // Load "new email" icon
    data = resources.LoadResource(B_VECTOR_ICON_TYPE, "ReplicantIconNewEmail", &size);
    if (data != NULL) {
        fIconNewEmail = new BBitmap(iconBounds, B_RGBA32);
        if (BIconUtils::GetVectorIcon((const uint8*)data, size, fIconNewEmail) != B_OK) {
            delete fIconNewEmail;
            fIconNewEmail = NULL;
        }
    }
    
    // Initial mail count
    _UpdateNewMailCount();
}

const char*
DeskbarReplicant::_GetString(const char* string, const char* context)
{
    // Get translated string from catalog, fallback to original if not found
    return fCatalog.GetString(string, context);
}

void DeskbarReplicant::_UpdateNewMailCount()
{
    // Count new emails using a query
    BVolumeRoster volumeRoster;
    BVolume volume;
    volumeRoster.GetBootVolume(&volume);
    
    // Get trash directory path
    BPath trashPath;
    BString trashLower;
    if (find_directory(B_TRASH_DIRECTORY, &trashPath) == B_OK) {
        trashLower = trashPath.Path();
        trashLower.ToLower();
    }
    
    BQuery query;
    query.SetVolume(&volume);
    query.SetPredicate("(BEOS:TYPE==\"text/x-email\")&&((MAIL:status==New)||(MAIL:status==Seen))");
    
    int32 count = 0;
    if (query.Fetch() == B_OK) {
        entry_ref ref;
        while (query.GetNextRef(&ref) == B_OK) {
            // Exclude trash
            BEntry entry(&ref);
            BPath path;
            entry.GetPath(&path);
            BString pathStr(path.Path());
            pathStr.ToLower();
            if (pathStr.FindFirst(trashLower) >= 0)
                continue;
            // Exclude spam
            BNode node(&ref);
            BString classification;
            if (node.InitCheck() == B_OK
                && node.ReadAttrString("MAIL:classification", &classification) == B_OK
                && classification.ICompare("Spam") == 0)
                continue;
            count++;
        }
    }
    
    if (count != fNewMailCount) {
        fNewMailCount = count;
        
        // Update tooltip
        BString tooltip;
        if (count == 0) {
            tooltip = _GetString("No new emails", "DeskbarReplicant");
        } else if (count == 1) {
            tooltip = _GetString("1 new email", "DeskbarReplicant");
        } else {
            BString fmt(_GetString("%count% new emails", "DeskbarReplicant"));
            BString countStr;
            countStr << count;
            fmt.ReplaceAll("%count%", countStr);
            tooltip = fmt;
        }
        SetToolTip(tooltip.String());
        
        Invalidate();
    }
}

_EXPORT DeskbarReplicant* DeskbarReplicant::Instantiate(BMessage* archive)
{
    if (!validate_instantiation(archive, "DeskbarReplicant"))
        return NULL;
    
    return new DeskbarReplicant(archive);
}

status_t DeskbarReplicant::Archive(BMessage* archive, bool deep) const
{
    status_t status = BView::Archive(archive, deep);
    if (status == B_OK)
        status = archive->AddString("add_on", kAppSignature);
    if (status == B_OK)
        status = archive->AddString("class", "DeskbarReplicant");
    
    return status;
}

void DeskbarReplicant::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    
    if (ViewUIColor() == B_NO_COLOR)
        SetLowColor(ViewColor());
    else
        SetLowUIColor(ViewUIColor());
    
    // Set initial tooltip (needs to be done after attached to window)
    BString tooltip;
    if (fNewMailCount <= 0) {
        tooltip = _GetString("No new emails", "DeskbarReplicant");
    } else if (fNewMailCount == 1) {
        tooltip = _GetString("1 new email", "DeskbarReplicant");
    } else {
        BString fmt(_GetString("%count% new emails", "DeskbarReplicant"));
        BString countStr;
        countStr << fNewMailCount;
        fmt.ReplaceAll("%count%", countStr);
        tooltip = fmt;
    }
    SetToolTip(tooltip.String());
}

void DeskbarReplicant::Draw(BRect updateRect)
{
    BBitmap* icon = (fNewMailCount > 0) ? fIconNewEmail : fIconNoEmail;
    
    if (icon != NULL) {
        SetDrawingMode(B_OP_ALPHA);
        DrawBitmap(icon);
        SetDrawingMode(B_OP_COPY);
    } else {
        // Fallback if icons not loaded
        rgb_color color = (fNewMailCount > 0) 
            ? make_color(0, 200, 0, 255)   // Green for new mail
            : make_color(128, 128, 128, 255);  // Gray for no mail
        SetHighColor(color);
        FillEllipse(Bounds());
    }
}

void DeskbarReplicant::MouseDown(BPoint point)
{
    BMessage* msg = Window()->CurrentMessage();
    int32 buttons = msg->FindInt32("buttons");
    
    if (buttons & B_SECONDARY_MOUSE_BUTTON) {
        // Show context menu
        BPoint screenPoint = ConvertToScreen(point);
        
        BPopUpMenu* menu = new BPopUpMenu("context", false, false);
        
        // Unread emails item with count
        BString unreadEmailsLabel;
        if (fNewMailCount > 0) {
            BString fmt(_GetString("Unread emails (%count%)", "DeskbarReplicant"));
            BString countStr;
            countStr << fNewMailCount;
            fmt.ReplaceAll("%count%", countStr);
            unreadEmailsLabel = fmt;
        } else {
            unreadEmailsLabel = _GetString("Unread emails", "DeskbarReplicant");
        }
        menu->AddItem(new BMenuItem(unreadEmailsLabel.String(), new BMessage('newv')));
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem(_GetString("Create new email" B_UTF8_ELLIPSIS, "DeskbarReplicant"), new BMessage('newm')));
        menu->AddItem(new BMenuItem(_GetString("Send/Receive email", "DeskbarReplicant"), new BMessage('chkm')));
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem(_GetString("Email accounts" B_UTF8_ELLIPSIS, "DeskbarReplicant"), new BMessage('emst')));
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem(_GetString("Remove from Deskbar", "DeskbarReplicant"), new BMessage('hide')));
        BString aboutLabel(_GetString("About %app%" B_UTF8_ELLIPSIS, "DeskbarReplicant"));
        aboutLabel.ReplaceAll("%app%", kAppName);
        menu->AddItem(new BMenuItem(aboutLabel.String(), new BMessage('abut')));
        
        menu->SetTargetForItems(this);
        menu->Go(screenPoint, true, true, true);
    } else {
        if (be_roster->IsRunning(kAppSignature)) {
            BMessenger messenger(kAppSignature);
            messenger.SendMessage(MSG_SHOW_WINDOW);
        } else {
            be_roster->Launch(kAppSignature);
        }
    }
}

void DeskbarReplicant::Pulse()
{
    // Periodically update mail count
    _UpdateNewMailCount();
}

void DeskbarReplicant::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case 'newv': {
            // Show EmailViews with "Unread emails" view selected (index 1)
            BMessage showMsg(MSG_SHOW_WINDOW);
            showMsg.AddInt32("view_index", 1);
            if (be_roster->IsRunning(kAppSignature)) {
                BMessenger messenger(kAppSignature);
                messenger.SendMessage(&showMsg);
            } else {
                be_roster->Launch(kAppSignature, &showMsg);
            }
            break;
        }
        
        case 'newm': {
            // Open EmailViews compose window for new message
            if (be_roster->IsRunning(kAppSignature)) {
                BMessenger messenger(kAppSignature);
                messenger.SendMessage(M_NEW);
            } else {
                // Launch app first, then send M_NEW
                be_roster->Launch(kAppSignature);
                // Give it a moment to start, then send message
                snooze(100000);  // 100ms
                BMessenger messenger(kAppSignature);
                messenger.SendMessage(M_NEW);
            }
            break;
        }
        
        case 'chkm':
            // Check for mail and send queued messages
            BMailDaemon().CheckAndSendQueuedMail();
            break;
        
        case 'emst':
            // Launch Haiku's E-mail preferences
            be_roster->Launch("application/x-vnd.Haiku-Mail");
            break;
        
        case 'abut':
            _ShowAbout();
            break;
        
        case 'hide': {
            // Save show_in_deskbar = false directly to settings file
            // This works even when the app isn't running
            BPath path;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
                path.Append("EmailViews");
                create_directory(path.Path(), 0755);
                path.Append("emailviews_settings");
                BMessage settings;
                
                // Read existing settings (if any)
                BFile file(path.Path(), B_READ_ONLY);
                if (file.InitCheck() == B_OK) {
                    settings.Unflatten(&file);
                }
                file.Unset();
                
                // Update the show_in_deskbar setting
                settings.RemoveName("show_in_deskbar");
                settings.AddBool("show_in_deskbar", false);
                
                // Write back
                BFile writeFile(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
                if (writeFile.InitCheck() == B_OK) {
                    settings.Flatten(&writeFile);
                }
            }
            
            // Notify app (if running) to update its internal state
            BMessenger messenger(kAppSignature);
            if (messenger.IsValid()) {
                messenger.SendMessage('dhid');  // Deskbar hidden
            }
            
            BDeskbar deskbar;
            deskbar.RemoveItem(kDeskbarReplicantName);
            break;
        }
        
        default:
            BView::MessageReceived(message);
            break;
    }
}

void DeskbarReplicant::_ShowAbout()
{
    // Load icon from our binary image at 64x64
    BBitmap* icon = NULL;
    image_info info;
    if (our_image(info) == B_OK) {
        BFile file(info.name, B_READ_ONLY);
        if (file.InitCheck() == B_OK) {
            BResources resources(&file);
            if (resources.InitCheck() == B_OK) {
                size_t size;
                const void* data = resources.LoadResource('VICN', "BEOS:ICON", &size);
                if (data && size > 0) {
                    icon = new BBitmap(BRect(0, 0, 63, 63), B_RGBA32);
                    if (BIconUtils::GetVectorIcon((const uint8*)data, size, icon) != B_OK) {
                        delete icon;
                        icon = NULL;
                    }
                }
            }
        }
    }
    
    AboutWindow* aboutWindow = new AboutWindow(icon);
    aboutWindow->Show();
}

// Export function for Deskbar to instantiate the replicant
extern "C" _EXPORT BView*
instantiate_deskbar_item(float maxWidth, float maxHeight)
{
    return new DeskbarReplicant(BRect(0, 0, maxHeight - 1, maxHeight - 1), B_FOLLOW_NONE);
}

// Required for BArchivable - called by Haiku's archiving system
// Provides definition for function declared in Archivable.h
BArchivable*
instantiate_object(BMessage* archive)
{
    const char* className = NULL;
    if (archive->FindString("class", &className) == B_OK && className != NULL) {
        if (strcmp(className, "DeskbarReplicant") == 0) {
            return DeskbarReplicant::Instantiate(archive);
        }
    }
    return NULL;
}

// EmailViewsApp implementation
EmailViewsApp::EmailViewsApp()
    : BApplication(kAppSignature)
{
    // Create and initialize global reader settings
    gReaderSettings = new TReaderSettings();
    gReaderSettings->Init();
    
    fWindow = new EmailViewsWindow();
    fWindow->Show();
}

EmailViewsApp::~EmailViewsApp()
{
    delete gReaderSettings;
    gReaderSettings = NULL;
}

void EmailViewsApp::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case MSG_SHOW_WINDOW: {
            // Restore and activate window - must lock when calling from app thread
            if (fWindow->Lock()) {
                fWindow->Minimize(false);
                fWindow->Activate();
                
                // Check for optional view selection by index (preferred,
                // language-independent) or by name (legacy)
                int32 viewIndex;
                if (message->FindInt32("view_index", &viewIndex) == B_OK) {
                    fWindow->SelectBuiltInQueryByIndex(viewIndex);
                } else {
                    const char* viewName = NULL;
                    if (message->FindString("view", &viewName) == B_OK && viewName != NULL) {
                        fWindow->SelectBuiltInQueryByName(viewName);
                    }
                }
                
                fWindow->Unlock();
            }
            break;
        }
        
        case M_PREFS:
            // Open Mail settings window
            if (gReaderSettings != NULL)
                gReaderSettings->ShowPrefsWindow();
            break;
        
        case PREFS_CHANGED:
            // Notify all reader windows to update their preferences
            if (gReaderSettings != NULL) {
                for (int32 i = 0; i < gReaderSettings->CountWindows(); i++) {
                    EmailReaderWindow* window = gReaderSettings->WindowAt(i);
                    if (window->Lock()) {
                        window->UpdatePreferences();
                        window->UpdateViews();
                        window->Unlock();
                    }
                }
            }
            // Also notify main window to update toolbar
            if (fWindow != NULL)
                fWindow->PostMessage(PREFS_CHANGED);
            break;
        
        case M_FONT:
            // Notify all reader windows about font change
            if (gReaderSettings != NULL)
                gReaderSettings->FontChange();
            // Also notify main window to update its preview pane
            if (fWindow != NULL)
                fWindow->PostMessage(M_FONT);
            break;
        
        case M_ACCOUNTS:
            // Open system Email account configuration
            be_roster->Launch("application/x-vnd.Haiku-Mail");
            break;
        
        case WINDOW_CLOSED: {
            // Handle window close notifications from prefs/signature windows
            int32 kind;
            if (message->FindInt32("kind", &kind) == B_OK && gReaderSettings != NULL) {
                switch (kind) {
                    case PREFS_WINDOW:
                        gReaderSettings->ClearPrefsWindow();
                        break;
                    case SIG_WINDOW:
                        gReaderSettings->ClearSignatureWindow();
                        break;
                }
            }
            break;
        }
        
        case M_NEW: {
            // Open a compose window - could be new, reply, or forward
            // This handles messages from reader windows (which have a source window)
            // Main window uses ComposeResponse() directly instead
            entry_ref ref;
            int32 type = M_NEW;
            EmailReaderWindow* sourceWindow = NULL;
            
            message->FindInt32("type", &type);
            message->FindPointer("window", (void**)&sourceWindow);
            
            // Calculate window frame with cascading
            BRect frame = gReaderSettings->NewWindowFrame();
            
            if (message->FindRef("ref", &ref) == B_OK && sourceWindow != NULL) {
                // Reply, Forward, or other operation from reader window
                EmailReaderWindow* composeWindow = new EmailReaderWindow(
                    frame,
                    "Composer",
                    NULL,     // ref - NULL for compose window
                    "",       // to
                    &gReaderSettings->ContentFont(),
                    type == M_RESEND,  // resending
                    fWindow   // emailViewsWindow
                );
                
                switch (type) {
                    case M_REPLY:
                    case M_REPLY_ALL:
                    case M_REPLY_TO_SENDER:
                        composeWindow->Reply(&ref, sourceWindow, type);
                        break;
                    
                    case M_FORWARD:
                        composeWindow->Forward(&ref, sourceWindow, true);
                        break;
                    
                    case M_FORWARD_WITHOUT_ATTACHMENTS:
                        composeWindow->Forward(&ref, sourceWindow, false);
                        break;
                    
                    case M_RESEND:
                    case M_COPY_TO_NEW:
                        composeWindow->CopyMessage(&ref, sourceWindow);
                        break;
                }
                
                composeWindow->MoveOnScreen();
                composeWindow->Show();
            } else {
                // Brand new message - check for pre-filled recipient
                const char* toAddress = "";
                message->FindString("to", &toAddress);
                
                EmailReaderWindow* composeWindow = new EmailReaderWindow(
                    frame,
                    "Composer",
                    NULL,       // ref - NULL for new message
                    toAddress,  // to - pre-filled recipient if provided
                    &gReaderSettings->ContentFont(),
                    false,      // resending
                    fWindow     // emailViewsWindow
                );
                composeWindow->AddAutoSignature(false, true);  // At end for new message, reset fChanged
                composeWindow->MoveOnScreen();
                composeWindow->Show();
            }
            break;
        }
        
        case 'dhid':
            // Deskbar replicant was hidden - forward to window
            fWindow->PostMessage(message);
            break;
        
        default:
            BApplication::MessageReceived(message);
            break;
    }
}

void EmailViewsApp::ReadyToRun()
{
    // Load spell check dictionaries
    _LoadDictionaries();
    
    // Check if we should show the Deskbar replicant
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
        path.Append("EmailViews/emailviews_settings");
        BFile file(path.Path(), B_READ_ONLY);
        if (file.InitCheck() == B_OK) {
            BMessage settings;
            if (settings.Unflatten(&file) == B_OK) {
                bool showInDeskbar = false;
                if (settings.FindBool("show_in_deskbar", &showInDeskbar) == B_OK && showInDeskbar) {
                    // Add replicant if not already present
                    BDeskbar deskbar;
                    if (deskbar.IsRunning() && !deskbar.HasItem(kDeskbarReplicantName)) {
                        app_info appInfo;
                        be_app->GetAppInfo(&appInfo);
                        deskbar.AddItem(&appInfo.ref);
                    }
                }
            }
        }
    }
}


void EmailViewsApp::_LoadDictionaries()
{
    static const char* kDictDirectory = "word_dictionary";
    static const char* kIndexDirectory = "word_index";
    static const char* kMetaphone = ".metaphone";
    static const char* kExact = ".exact";
    
    BPath indexDir;
    BPath dictionaryDir;
    BPath userDictionaryDir;
    BPath userIndexDir;
    BPath dataPath;
    BPath indexPath;
    BDirectory directory;
    BEntry entry;
    
    // Locate system dictionaries directory
    find_directory(B_SYSTEM_DATA_DIRECTORY, &indexDir, true);
    indexDir.Append("spell_check");
    dictionaryDir = indexDir;
    
    // Locate user dictionary directory
    find_directory(B_USER_CONFIG_DIRECTORY, &userIndexDir, true);
    userIndexDir.Append("data/spell_check");
    userDictionaryDir = userIndexDir;
    
    // Create user directory if needed
    directory.CreateDirectory(userIndexDir.Path(), NULL);
    
    // Setup directory paths
    indexDir.Append(kIndexDirectory);
    dictionaryDir.Append(kDictDirectory);
    userIndexDir.Append(kIndexDirectory);
    userDictionaryDir.Append(kDictDirectory);
    
    // Create directories if needed
    directory.CreateDirectory(indexDir.Path(), NULL);
    directory.CreateDirectory(dictionaryDir.Path(), NULL);
    directory.CreateDirectory(userIndexDir.Path(), NULL);
    directory.CreateDirectory(userDictionaryDir.Path(), NULL);
    
    dataPath = dictionaryDir;
    dataPath.Append("words");
    
    // Only load if a words dictionary exists
    if (!BEntry(dataPath.Path()).Exists())
        return;
    
    // Load system dictionaries
    directory.SetTo(dictionaryDir.Path());
    
    BString leafName;
    gUserDict = -1;
    
    while (gDictCount < MAX_DICTIONARIES
        && directory.GetNextEntry(&entry) != B_ENTRY_NOT_FOUND) {
        dataPath.SetTo(&entry);
        
        indexPath = indexDir;
        leafName.SetTo(dataPath.Leaf());
        leafName.Append(kMetaphone);
        indexPath.Append(leafName.String());
        gWords[gDictCount] = new Words(dataPath.Path(),
            indexPath.Path(), true);
        
        indexPath = indexDir;
        leafName.SetTo(dataPath.Leaf());
        leafName.Append(kExact);
        indexPath.Append(leafName.String());
        gExactWords[gDictCount] = new Words(dataPath.Path(),
            indexPath.Path(), false);
        gDictCount++;
    }
    
    // Create user dictionary if it does not exist
    dataPath = userDictionaryDir;
    dataPath.Append("user");
    if (!BEntry(dataPath.Path()).Exists()) {
        BFile user(dataPath.Path(), B_WRITE_ONLY | B_CREATE_FILE);
        BNodeInfo(&user).SetType("text/plain");
    }
    
    // Load user dictionary
    if (BEntry(userDictionaryDir.Path()).Exists()) {
        gUserDictFile = new BFile(dataPath.Path(),
            B_WRITE_ONLY | B_OPEN_AT_END);
        gUserDict = gDictCount;
        
        indexPath = userIndexDir;
        leafName.SetTo(dataPath.Leaf());
        leafName.Append(kMetaphone);
        indexPath.Append(leafName.String());
        gWords[gDictCount] = new Words(dataPath.Path(),
            indexPath.Path(), true);
        
        indexPath = userIndexDir;
        leafName.SetTo(dataPath.Leaf());
        leafName.Append(kExact);
        indexPath.Append(leafName.String());
        gExactWords[gDictCount] = new Words(dataPath.Path(),
            indexPath.Path(), false);
        gDictCount++;
    }
}


void EmailViewsApp::RefsReceived(BMessage* message)
{
    entry_ref ref;
    for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
        // Check if it's an email file
        BNode node(&ref);
        if (node.InitCheck() != B_OK)
            continue;
        
        BNodeInfo nodeInfo(&node);
        char mimeType[B_MIME_TYPE_LENGTH];
        if (nodeInfo.GetType(mimeType) == B_OK) {
            if (strcmp(mimeType, "text/x-email") == 0 
                || strcmp(mimeType, "text/x-vnd.Be-MailDraft") == 0) {
                // Open in reader window
                BPath path(&ref);
                fWindow->OpenEmailInViewer(path.Path());
            }
        }
    }
}

// Volume selection functions

void EmailViewsWindow::BuildVolumeMenu()
{
    if (fVolumeMenu == NULL)
        return;
    
    // Clear existing items
    while (fVolumeMenu->CountItems() > 0) {
        delete fVolumeMenu->RemoveItem((int32)0);
    }
    
    // Iterate through all mounted BFS volumes
    BVolumeRoster roster;
    BVolume volume;
    
    while (roster.GetNextVolume(&volume) == B_OK) {
        // Only show persistent, writable volumes that support queries (BFS).
        // This excludes packagefs (system, config) which are read-only,
        // tmpfs, devfs, and other virtual filesystems.
        if (!volume.IsPersistent() || !volume.KnowsQuery() || volume.IsReadOnly())
            continue;
        
        // Get volume name
        char name[B_FILE_NAME_LENGTH];
        if (volume.GetName(name) != B_OK)
            continue;
        
        // Create menu item with device ID in message
        BMessage* msg = new BMessage(MSG_VOLUME_SELECTED);
        msg->AddInt32("device", volume.Device());
        
        BMenuItem* item = new BMenuItem(name, msg);
        item->SetMarked(IsVolumeSelected(volume.Device()));
        fVolumeMenu->AddItem(item);
    }
}

void EmailViewsWindow::UpdateVolumeMenu()
{
    if (fVolumeMenu == NULL)
        return;
    
    // Update checkmarks based on current selection
    for (int32 i = 0; i < fVolumeMenu->CountItems(); i++) {
        BMenuItem* item = fVolumeMenu->ItemAt(i);
        if (item == NULL)
            continue;
        
        BMessage* msg = item->Message();
        if (msg == NULL || msg->what != MSG_VOLUME_SELECTED)
            continue;
        
        int32 device;
        if (msg->FindInt32("device", &device) == B_OK) {
            item->SetMarked(IsVolumeSelected(device));
        }
    }
}

void EmailViewsWindow::LoadVolumeSelection()
{
    // Clear existing selection
    fSelectedVolumes.MakeEmpty();
    
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
        return;
    
    path.Append("EmailViews/emailviews_settings");
    BFile file(path.Path(), B_READ_ONLY);
    if (file.InitCheck() != B_OK) {
        // No settings file - default to boot volume
        BVolumeRoster roster;
        BVolume* bootVolume = new BVolume();
        roster.GetBootVolume(bootVolume);
        if (bootVolume->InitCheck() == B_OK) {
            fSelectedVolumes.AddItem(bootVolume);
        } else {
            delete bootVolume;
        }
        return;
    }
    
    BMessage settings;
    if (settings.Unflatten(&file) != B_OK) {
        // Failed to read settings - default to boot volume
        BVolumeRoster roster;
        BVolume* bootVolume = new BVolume();
        roster.GetBootVolume(bootVolume);
        if (bootVolume->InitCheck() == B_OK) {
            fSelectedVolumes.AddItem(bootVolume);
        } else {
            delete bootVolume;
        }
        return;
    }
    
    // Read selected volume names from settings
    const char* volumeName;
    for (int32 i = 0; settings.FindString("selected_volume_name", i, &volumeName) == B_OK; i++) {
        // Find this volume by name
        BVolumeRoster roster;
        BVolume volume;
        while (roster.GetNextVolume(&volume) == B_OK) {
            char name[B_FILE_NAME_LENGTH];
            if (volume.GetName(name) == B_OK && strcmp(name, volumeName) == 0) {
                // Verify volume supports queries and is writable
                if (volume.IsPersistent() && volume.KnowsQuery() && !volume.IsReadOnly()) {
                    fSelectedVolumes.AddItem(new BVolume(volume));
                }
                break;
            }
        }
    }
    
    // If no valid volumes loaded, default to boot volume
    if (fSelectedVolumes.IsEmpty()) {
        BVolumeRoster roster;
        BVolume* bootVolume = new BVolume();
        roster.GetBootVolume(bootVolume);
        if (bootVolume->InitCheck() == B_OK) {
            fSelectedVolumes.AddItem(bootVolume);
        } else {
            delete bootVolume;
        }
    }
}

void EmailViewsWindow::SaveVolumeSelection()
{
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
        return;
    path.Append("EmailViews");
    create_directory(path.Path(), 0755);
    path.Append("emailviews_settings");
    
    // Load existing settings to preserve other data
    BMessage settings;
    BFile file(path.Path(), B_READ_ONLY);
    if (file.InitCheck() == B_OK) {
        settings.Unflatten(&file);
    }
    file.Unset();
    
    // Update volume selection (save by name, not device ID).
    // Device IDs change across reboots, but volume names are stable.
    settings.RemoveName("selected_volume");  // Remove old format if present
    settings.RemoveName("selected_volume_name");
    for (int32 i = 0; i < fSelectedVolumes.CountItems(); i++) {
        BVolume* volume = fSelectedVolumes.ItemAt(i);
        if (volume != NULL) {
            char name[B_FILE_NAME_LENGTH];
            if (volume->GetName(name) == B_OK) {
                settings.AddString("selected_volume_name", name);
            }
        }
    }
    
    // Save to settings file
    file.SetTo(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (file.InitCheck() == B_OK) {
        settings.Flatten(&file);
    }
}

bool EmailViewsWindow::IsVolumeSelected(dev_t device) const
{
    for (int32 i = 0; i < fSelectedVolumes.CountItems(); i++) {
        BVolume* volume = fSelectedVolumes.ItemAt(i);
        if (volume != NULL && volume->Device() == device)
            return true;
    }
    return false;
}

void EmailViewsWindow::SetVolumeSelected(dev_t device, bool selected)
{
    if (selected) {
        // Add if not already selected
        if (!IsVolumeSelected(device)) {
            BVolume* volume = new BVolume(device);
            if (volume->InitCheck() == B_OK) {
                fSelectedVolumes.AddItem(volume);
            } else {
                delete volume;
            }
        }
    } else {
        // Remove if currently selected (but ensure at least one volume remains)
        if (fSelectedVolumes.CountItems() <= 1)
            return;  // Don't allow deselecting the last volume
        
        for (int32 i = 0; i < fSelectedVolumes.CountItems(); i++) {
            BVolume* volume = fSelectedVolumes.ItemAt(i);
            if (volume != NULL && volume->Device() == device) {
                fSelectedVolumes.RemoveItemAt(i);  // Owning list deletes the item
                break;
            }
        }
    }
}

// --- Spam sender blocklist ---

void
EmailViewsWindow::LoadSpamBlocklist()
{
    fSpamBlocklist.clear();

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
            entry.ToLower();
            fSpamBlocklist.insert(entry);
        }
    }
    fclose(f);
}


void
EmailViewsWindow::SaveSpamBlocklist()
{
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
        return;
    path.Append("EmailViews");
    create_directory(path.Path(), 0755);
    path.Append("blocked_senders");

    FILE* f = fopen(path.Path(), "w");
    if (f == NULL)
        return;

    fprintf(f, "# EmailViews spam sender blocklist\n");
    fprintf(f, "# One address or @domain per line\n");
    for (const BString& entry : fSpamBlocklist) {
        fprintf(f, "%s\n", entry.String());
    }
    fclose(f);
}


void
EmailViewsWindow::AddToSpamBlocklist(const char* address)
{
    BString addr(address);
    addr.Trim();
    addr.ToLower();
    if (addr.Length() > 0) {
        fSpamBlocklist.insert(addr);
        SaveSpamBlocklist();
    }
}


void
EmailViewsWindow::RemoveFromSpamBlocklist(const char* address)
{
    BString addr(address);
    addr.Trim();
    addr.ToLower();
    fSpamBlocklist.erase(addr);
    SaveSpamBlocklist();
}


bool
EmailViewsWindow::IsSenderBlocked(const char* fromField) const
{
    BString addr = _ExtractEmailAddress(fromField);
    if (addr.Length() == 0)
        return false;

    // Check exact address match
    if (fSpamBlocklist.count(addr) > 0)
        return true;

    // Check domain match (@domain.com)
    int32 at = addr.FindFirst('@');
    if (at >= 0) {
        BString domain;
        addr.CopyInto(domain, at, addr.Length() - at);  // "@domain.com"
        if (fSpamBlocklist.count(domain) > 0)
            return true;
    }

    return false;
}


// Main function
int main()
{
    EmailViewsApp app;
    app.Run();
    return 0;
}
