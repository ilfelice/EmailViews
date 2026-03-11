/*
 * EmailListView.h - High-performance email list view for Haiku
 * Distributed under the terms of the MIT License.
 * 
 * Self-contained email list component with:
 * - Column headers with sorting, resizing, reordering
 * - Virtual scrolling (only draws visible rows)
 * - O(1) lookup by node_ref via HashMap
 * - Background query loading with two-phase display
 * - Live query support for real-time updates
 * - Status caption in corner
 */

#ifndef EMAIL_LIST_VIEW_H
#define EMAIL_LIST_VIEW_H

#include <GroupView.h>
#include <View.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <ObjectList.h>
#include <Font.h>
#include <String.h>
#include <StringView.h>
#include <Window.h>
#include <Query.h>
#include <Volume.h>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <memory>

#include "EmailRef.h"
#include "EmailColumnHeader.h"
#include "LoadingDots.h"

// Forward declarations
class EmailItem;
class EmailListView;

// Hash function for node_ref to use with HashMap
struct NodeRefHash {
    size_t operator()(const node_ref& ref) const {
        size_t hash = std::hash<dev_t>()(ref.device);
        hash ^= std::hash<ino_t>()(ref.node) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct NodeRefEqual {
    bool operator()(const node_ref& a, const node_ref& b) const {
        return a.device == b.device && a.node == b.node;
    }
};


/*
 * EmailItem - Visual representation of an email in the list
 */
class EmailItem {
public:
                        EmailItem(EmailRef* ref);
                        ~EmailItem();
    
    EmailRef*           Ref() const { return fRef; }
    
    bool                IsSelected() const { return fSelected; }
    void                SetSelected(bool selected) { fSelected = selected; }
    
    // String accessors
    const char*         GetPath() const;
    const char*         GetStatus() const;
    const char*         GetAccount() const;
    const char*         GetFrom() const;
    const char*         GetTo() const;
    const char*         GetSubject() const;
    
    // Time accessors
    time_t              GetWhen() const;
    const timespec&     GetCrtime() const;
    
    // Reference accessors
    const node_ref*     GetNodeRef() const;
    const entry_ref&    GetEntryRef() const;
    
    // Boolean state
    bool                IsRead() const;
    bool                HasAttachment() const;
    bool                IsStarred() const;
    
    // Setters
    void                SetRead(bool read);
    void                SetStatus(const char* status);
    void                SetStarred(bool starred);
    void                SetHasAttachment(bool hasAttachment);
    
    // Display strings
    const char*         Status();
    const char*         Subject();
    const char*         From();
    const char*         To();
    const char*         DateString();
    const char*         Account();
    
    void                InvalidateCache();
    
private:
    EmailRef*           fRef;
    bool                fSelected;
    mutable BString     fPath;
    mutable bool        fPathValid;
    mutable BString     fDateString;
    mutable bool        fDateStringValid;
};


/*
 * EmailListView - Self-contained email list component
 * 
 * This is a complete, self-contained view that includes:
 * - Column header with sorting, resizing, visibility, reordering
 * - Scrollable list content with virtual scrolling
 * - Vertical and horizontal scrollbars
 * - Status label showing email count
 * 
 * Just add this single view to your layout - no additional setup needed.
 */
class EmailListView : public BGroupView {
public:
                        EmailListView(const char* name, BWindow* target);
    virtual             ~EmailListView();
    
    // BView overrides
    virtual void        AttachedToWindow();
    virtual void        AllAttached();
    virtual void        DetachedFromWindow();
    virtual void        MessageReceived(BMessage* message);
    virtual void        MakeFocus(bool focus = true);
    
    // === Query Execution API ===
    
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    void                StartQuery(const char* predicate,
                                   BObjectList<BVolume, true>* volumes = NULL,
#else
    void                StartQuery(const char* predicate,
                                   BObjectList<BVolume>* volumes = NULL,
#endif
                                   bool showTrash = false,
                                   bool showSpam = false,
                                   bool attachmentsOnly = false);
    void                StopQuery();
    void                InvalidateQueryId() { fCurrentQueryId++; }
    bool                IsLoading() const { return fLoaderThread >= 0; }
    int32               LoadingProgress() const;
    
    // === Body / Full-Text Search API ===
    
    void                StartBodySearch(const char* searchText,
                                        bool caseSensitive,
                                        bool fullText,
                                        bool keepExisting = false,
                                        BList* externalScope = NULL);
    void                StopBodySearch();
    bool                IsBodySearchRunning() const
                            { return fBodySearchThread >= 0; }
    
    static const uint32 kMsgLoadingUpdate = 'ldud';
    
    // === Data Access ===
    
    int32               CountItems() const;
    EmailItem*          ItemAt(int32 index) const;
    
    void                AddEmail(EmailRef* ref);
    void                AddEmailSorted(EmailRef* ref);
    void                AddItem(EmailItem* item);
    void                RemoveEmail(const node_ref& ref);
    void                RemoveRow(EmailItem* item);
    void                UpdateRow(EmailItem* item);
    void                MakeEmpty();
    void                Clear() { MakeEmpty(); }
    
    // Lookup by node_ref (O(1))
    int32               IndexOf(const node_ref& ref) const;
    EmailItem*          ItemFor(const node_ref& ref) const;
    int32               IndexOf(EmailItem* item) const;
    
    // === Selection ===
    
    int32               CurrentSelectionIndex() const;
    void                Select(int32 index, bool extend = false, bool toggle = false);
    void                SelectRange(int32 from, int32 to);
    void                ExtendSelectionTo(int32 index);
    void                Deselect();
    void                DeselectAll();
    bool                IsSelected(int32 index) const;
    int32               CountSelected() const;
    int32               FirstSelected() const;
    int32               LastSelected() const;
    void                GetSelectedIndices(BList* indices) const;
    EmailItem*          SelectedItem() const;
    
    // Santa-style selection API
    EmailItem*          CurrentSelection(EmailItem* lastSelected = NULL) const;
    void                AddToSelection(EmailItem* item);
    void                SetFocusRow(EmailItem* item, bool select);
    EmailItem*          FocusRow() const;
    
    // Navigation (for Reader Next/Previous)
    bool                SelectNext();
    bool                SelectPrevious();
    
    // === Scrolling ===
    
    void                ScrollToSelection();
    void                ScrollToItem(int32 index);
    void                ScrollTo(EmailItem* item);
    void                ScrollTo(BPoint where);  // Scroll to absolute position
    void                EnsureVisible(int32 index);
    
    // === Sorting ===
    
    enum SortColumn {
        kSortByStatus = 0,
        kSortByStar,
        kSortByAttachment,
        kSortByFrom,
        kSortByTo,
        kSortBySubject,
        kSortByDate,
        kSortByAccount
    };
    
    enum SortOrder {
        kSortAscending = 1,
        kSortDescending = 2
    };
    
    void                SetSortColumn(SortColumn column, SortOrder order);
    SortColumn          GetSortColumn() const { return fSortColumn; }
    SortOrder           GetSortOrder() const { return fSortOrder; }
    void                SortItems();
    
    static int          CompareItems(const EmailItem* a, const EmailItem* b, 
                                     SortColumn column, SortOrder order);
    
    // === Bulk Operations ===
    
    void                BeginBulkLoad();
    void                EndBulkLoad();
    
    // Batch remove hints (currently no-ops, kept for API completeness)
    void                BeginBatchRemove() {}
    void                EndBatchRemove() {}
    
    // === Messages ===
    
    void                SetSelectionMessage(BMessage* message);
    void                SetInvocationMessage(BMessage* message);
    
    enum {
        kMsgAddEmails       = 'adem',
        kMsgQueryComplete   = 'qcmp',
        kMsgEmailAdded      = 'emad',
        kMsgEmailRemoved    = 'emrm',
        kMsgEmailChanged    = 'emch',
        kMsgMarkAsRead      = 'mkrd',
        kMsgMarkAsUnread    = 'mkun',
        kMsgMarkAsSent      = 'mkst',
        kMsgMoveToTrash     = 'mvtr',
        kMsgBodySearchBatch = 'bsbt',
        kMsgBodySearchDone  = 'bsdn',
        kMsgBodySearchProgress = 'bspg'
    };
    
    // === Appearance ===
    
    void                SetShowingTrash(bool showingTrash);
    void                SetShowingSpam(bool showingSpam);
    void                SetSpamBlocklist(const std::set<BString>& blocklist);
    
    void                InvalidateItem(int32 index);
    
    // Called by column header when columns change
    void                ColumnsResized();
    
    // Content view access (for external code that needs Bounds/Invalidate)
    void                Invalidate();
    void                Invalidate(BRect rect);
    BRect               ContentBounds() const;
    
    // === Column State Persistence ===
    
    status_t            SaveColumnState(BMessage* into) const;
    status_t            RestoreColumnState(const BMessage* from);

    void                SetStatusText(const char* text);
    void                StartLoadingDots();
    void                StopLoadingDots();
    
    // === Internal Component Access (for compatibility) ===
    
    BScrollView*        ScrollView() const { return fScrollView; }
    
private:
    // Internal content view class - draws the email rows
    class ContentView;
    friend class ContentView;
    
    // Drawing helpers (called by ContentView)
    void                _DrawContent(BView* view, BRect updateRect);
    void                _DrawRow(BView* view, EmailItem* item, BRect rowRect,
                                 bool isSelected, int32 rowIndex);
    void                _UpdateStripeColor();
    BRect               _RowRect(int32 index) const;
    int32               _IndexAt(BPoint point) const;
    
    // Input handling (called by ContentView)
    void                _HandleMouseDown(BView* view, BPoint where);
    void                _HandleKeyDown(const char* bytes, int32 numBytes);
    
    // Selection notification
    void                _NotifySelectionChanged();
    void                _NotifyInvocation();
    
    // Visible range
    int32               _FirstVisibleIndex() const;
    int32               _LastVisibleIndex() const;
    int32               _VisibleCount() const;
    
    // Scroll bar management
    void                _UpdateScrollBar();
    
    // Binary search for sorted insertion
    int32               _FindInsertionPoint(EmailRef* ref);
    int                 _CompareForInsertion(const EmailRef* a, const EmailRef* b) const;
    
    // Linear scan lookup (safe during Phase 2 loading when HashMap is stale)
    EmailItem*          _FindItemByNodeRef(const node_ref& ref, int32* outIndex) const;
    
    // Node watching
    void                _UpdateVisibleWatches();
    void                _StopAllWatches();
    
    // Context menu
    void                _ShowContextMenu(BPoint where);
    void                _MarkSelectedAs(const char* status);
    void                _MoveSelectedToTrash();
    // Query execution
    void                _ProcessLoaderBatch(BMessage* message);
    void                _StartLiveQueries();
    void                _StartInterimLiveQueries();
    void                _StopLiveQueries();
    void                _SendLoadingUpdate(bool loading, bool complete);
    void                _NotifyCountChanged();
    static int32        _LoaderThread(void* data);
    
    // Body search
    static int32        _BodySearchThread(void* data);
    
private:
    // Data — fItems is the source of truth for display order.
    // fNodeToIndex provides O(1) lookup by node_ref but may have stale indices
    // during Phase 2 loading (see IndexOf() for the safety mechanism).
    // fWatchedNodes tracks which nodes we have active B_WATCH_ATTR monitors on.
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<EmailItem, false>  fItems;  // Non-owning: MakeEmpty uses async disposal
#else
    BObjectList<EmailItem>  fItems;  // Non-owning: MakeEmpty uses async disposal
#endif
    std::unordered_map<node_ref, int32, NodeRefHash, NodeRefEqual> fNodeToIndex;
    std::unordered_set<node_ref, NodeRefHash, NodeRefEqual> fWatchedNodes;
    
    // Target window
    BWindow*            fTarget;
    
    // Geometry
    float               fRowHeight;
    
    // Selection
    std::set<int32>     fSelectedIndices;
    int32               fAnchorIndex;
    int32               fLastClickIndex;
    mutable int32       fSelectionIterIndex;
    
    // Sorting
    SortColumn          fSortColumn;
    SortOrder           fSortOrder;
    
    // State
    bool                fBulkLoading;
    bool                fShowingTrash;
    bool                fShowingSpam;
    std::set<BString>   fSpamBlocklist;
    
    // Internal components (owned by this view)
    ContentView*        fContentView;
    EmailColumnHeaderView* fColumnHeader;
    BScrollView*        fScrollView;
    BScrollBar*         fVScrollBar;
    BScrollBar*         fHScrollBar;
    BStringView*        fStatusLabel;
    LoadingDots*        fLoadingDots;
    
    // Messages
    BMessage*           fSelectionMessage;
    BMessage*           fInvocationMessage;
    
    // Drawing state
    rgb_color           fBackgroundColor;
    rgb_color           fStripeColor;
    rgb_color           fSelectedColor;
    rgb_color           fTextColor;
    rgb_color           fSelectedTextColor;
    
    // Query execution state
    thread_id           fLoaderThread;
    std::shared_ptr<volatile bool> fCurrentStopFlag;  // Shared with loader thread; set to true to cancel
    volatile int32      fCurrentQueryId;  // Incremented on each new query to detect stale results
    BString             fQueryPredicate;
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<BVolume, true> fQueryVolumes;
#else
    BObjectList<BVolume> fQueryVolumes;
#endif
    bool                fQueryShowTrash;
    bool                fQueryShowSpam;
    bool                fQueryAttachmentsOnly;
    time_t              fQueryCutoffTime;
    int32               fLoadedCount;
    int32               fTotalCount;
    
    // Live queries
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<BQuery, true> fLiveQueries;
#else
    BObjectList<BQuery> fLiveQueries;
#endif
    
    // Body search state
    thread_id           fBodySearchThread;
    std::shared_ptr<volatile bool> fBodySearchStopFlag;
    int32               fBodySearchTotal;   // Total emails to scan
    int32               fBodySearchScanned; // Emails scanned so far
    int32               fBodySearchFound;   // Matches found so far
};


/*
 * EmailListView::ContentView - Internal view that draws email rows
 * 
 * This is a private inner class that handles the actual row drawing
 * and mouse/keyboard interaction for the list content area.
 */
class EmailListView::ContentView : public BView {
public:
                        ContentView(EmailListView* parent);
    
    virtual void        Draw(BRect updateRect);
    virtual void        FrameResized(float width, float height);
    virtual void        ScrollTo(BPoint where);
    virtual void        MouseDown(BPoint where);
    virtual void        KeyDown(const char* bytes, int32 numBytes);
    virtual void        MakeFocus(bool focus = true);
    virtual void        AttachedToWindow();
    
private:
    EmailListView*      fParent;
};

#endif // EMAIL_LIST_VIEW_H
