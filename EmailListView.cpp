/*
 * EmailListView.cpp - High-performance email list view implementation
 * Distributed under the terms of the MIT License.
 * 
 * Architecture overview:
 * - EmailListView is a BGroupView containing a column header, a scrollable
 *   ContentView (draws rows), scrollbars, and a status label with loading dots.
 * - Only visible rows are drawn (virtual scrolling via _FirstVisibleIndex).
 * - A HashMap (node_ref → index) provides O(1) lookup. During loading,
 *   the HashMap may have stale indices for shifted items (see AddEmailSorted).
 * - Node monitors (B_WATCH_ATTR) track only the ~30-50 visible rows to stay
 *   within the system-wide 4096 monitor limit.
 *
 * Two-phase query loading:
 *   Phase 1: Recent emails (last 30 days) loaded with sorted insertion.
 *            Live queries start at end of Phase 1 so new mail appears immediately.
 *   Phase 2: Older emails loaded at low priority into the same list.
 *            HashMap rebuilt at Phase 2 completion.
 *   Single-phase: Queries involving MAIL:draft skip the time split because
 *            draft emails may lack MAIL:when.
 *
 * Threading model:
 *   Loader thread creates EmailRef objects (disk I/O) and posts batches to
 *   the window thread via BMessenger. The window thread inserts items into
 *   the list. A shared_ptr<volatile bool> stop flag allows safe cancellation.
 */

#include "EmailColumnHeader.h"
#include "EmailListView.h"
#include "EmailAccountMap.h"

#include <Box.h>
#include <Bitmap.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <DateFormat.h>
#include <DateTimeFormat.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <GroupLayout.h>
#include <GroupView.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <Node.h>
#include <NodeInfo.h>
#include <NodeMonitor.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Resources.h>
#include <Roster.h>
#include <Volume.h>
#include <VolumeRoster.h>
#include <Window.h>

#include <MessageRunner.h>

#include <mail_encoding.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <set>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "EmailListView"

#include "LoadingDots.h"


// A view that draws only a left border line. Used as the outermost
// container so the left border spans the full height of the email list.
class LeftBorderView : public BView {
public:
    LeftBorderView(const char* name)
        : BView(name, B_WILL_DRAW | B_DRAW_ON_CHILDREN | B_FRAME_EVENTS)
    {
        SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
        SetLayout(new BGroupLayout(B_VERTICAL, 0));
    }

    virtual void DrawAfterChildren(BRect updateRect)
    {
        BRect bounds = Bounds();
        SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
            B_DARKEN_2_TINT));
        StrokeLine(bounds.LeftTop(), bounds.LeftBottom());
    }
};


// A wrapper view for the status label and loading dots that draws
// top and bottom borders to match the scrollbar border lines.
class StatusAreaView : public BView {
public:
    StatusAreaView(const char* name)
        : BView(name, B_WILL_DRAW | B_DRAW_ON_CHILDREN | B_FRAME_EVENTS)
    {
        SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    }

    virtual void DrawAfterChildren(BRect updateRect)
    {
        BRect bounds = Bounds();
        SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
            B_DARKEN_2_TINT));
        StrokeLine(bounds.LeftTop(), bounds.RightTop());
        StrokeLine(bounds.LeftBottom(), bounds.RightBottom());
    }
};

// A spacer view for the corner below the vertical scrollbar.
// Draws bottom and right borders to complete the border frame.
class CornerSpacerView : public BView {
public:
    CornerSpacerView(const char* name)
        : BView(name, B_WILL_DRAW)
    {
        SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    }

    virtual void Draw(BRect updateRect)
    {
        BRect bounds = Bounds();
        SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
            B_DARKEN_2_TINT));
        // Bottom border
        StrokeLine(bounds.LeftBottom(), bounds.RightBottom());
        // Right border
        StrokeLine(bounds.RightTop(), bounds.RightBottom());
    }
};


// Internal message codes for loader thread → window thread communication.
// kMsgLoaderBatch carries a batch of EmailRef pointers.
// kMsgPhase1Done / kMsgPhase2Done signal phase completion (trigger next phase or finalize).
static const uint32 kMsgLoaderBatch = 'ldbh';
static const uint32 kMsgLoaderDone = 'lddn';
static const uint32 kMsgPhase1Done = 'ph1d';
static const uint32 kMsgPhase2Done = 'ph2d';
// Icon resource IDs (must match .rdef)
static const int32 kResStarred = 402;
static const int32 kResAttachment = 401;
static const int32 kResAttachmentWhite = 403;

// Cached icons (loaded once, shared by all instances)
static BBitmap* sStarIcon = NULL;
static BBitmap* sAttachmentIcon = NULL;
static BBitmap* sAttachmentWhiteIcon = NULL;
static bool sIconsLoaded = false;


// ============================================================================
// Body text extraction helpers (for body/full-text search)
// ============================================================================

// Strip HTML tags from a string in-place, decode common HTML entities,
// and collapse excessive whitespace. Only applied when the content is
// detected as HTML. Preserves all text between tags so code examples
// in entity-encoded form (e.g. &lt;div&gt;) come through correctly.
static void
StripHtmlTags(BString* text)
{
	if (text == NULL || text->Length() == 0)
		return;

	const char* src = text->String();
	int32 srcLen = text->Length();
	BString result;
	result.SetTo("", srcLen);  // Pre-allocate

	bool inTag = false;
	bool inScript = false;  // Inside <script> or <style> blocks

	for (int32 i = 0; i < srcLen; i++) {
		char c = src[i];

		if (inScript) {
			// Look for closing </script> or </style>
			if (c == '<' && i + 2 < srcLen && src[i + 1] == '/') {
				// Check for </script or </style (case-insensitive)
				BString closing;
				int32 end = i + 2;
				while (end < srcLen && src[end] != '>' && (end - i) < 12)
					end++;
				if (end < srcLen) {
					text->CopyInto(closing, i + 2, end - i - 2);
					closing.ToLower();
					if (closing == "script" || closing == "style") {
						inScript = false;
						i = end;  // Skip past the closing tag
					}
				}
			}
			continue;
		}

		if (c == '<') {
			// Check for <script or <style (case-insensitive)
			if (i + 7 < srcLen) {
				BString tagName;
				int32 end = i + 1;
				while (end < srcLen && src[end] != '>' && src[end] != ' '
					&& (end - i) < 10)
					end++;
				text->CopyInto(tagName, i + 1, end - i - 1);
				tagName.ToLower();
				if (tagName == "script" || tagName == "style") {
					inScript = true;
					// Skip to end of opening tag
					while (i < srcLen && src[i] != '>')
						i++;
					continue;
				}
			}
			inTag = true;
			// Insert a space for block-level tags to prevent word joining
			if (result.Length() > 0) {
				char last = result.ByteAt(result.Length() - 1);
				if (last != ' ' && last != '\n')
					result << ' ';
			}
			continue;
		}

		if (c == '>') {
			inTag = false;
			continue;
		}

		if (inTag)
			continue;

		// Decode HTML entities
		if (c == '&' && i + 1 < srcLen) {
			// Find the semicolon
			int32 semi = -1;
			for (int32 j = i + 1; j < srcLen && j < i + 10; j++) {
				if (src[j] == ';') {
					semi = j;
					break;
				}
			}
			if (semi > i + 1) {
				BString entity;
				text->CopyInto(entity, i + 1, semi - i - 1);

				bool decoded = false;
				if (entity == "amp") {
					result << '&'; decoded = true;
				} else if (entity == "lt") {
					result << '<'; decoded = true;
				} else if (entity == "gt") {
					result << '>'; decoded = true;
				} else if (entity == "quot") {
					result << '"'; decoded = true;
				} else if (entity == "apos") {
					result << '\''; decoded = true;
				} else if (entity == "nbsp") {
					result << ' '; decoded = true;
				} else if (entity.ByteAt(0) == '#') {
					// Numeric entity: &#nnn; or &#xHHH;
					int32 codePoint = 0;
					if (entity.ByteAt(1) == 'x' || entity.ByteAt(1) == 'X') {
						codePoint = (int32)strtol(
							entity.String() + 2, NULL, 16);
					} else {
						codePoint = atoi(entity.String() + 1);
					}
					if (codePoint > 0 && codePoint < 128) {
						result << (char)codePoint;
						decoded = true;
					} else if (codePoint >= 128) {
						// Non-ASCII: replace with space to avoid
						// broken UTF-8 sequences in search matching
						result << ' ';
						decoded = true;
					}
				}
				if (decoded) {
					i = semi;  // Skip past the entity
					continue;
				}
			}
		}

		result << c;
	}

	*text = result;
}


// Search a header block for a specific header field (case-insensitive).
// Returns the value (trimmed) or empty string if not found.
// headerStart/headerEnd define the region to search (exclusive of the
// blank-line separator).
static BString
_FindHeader(const char* headerStart, const char* headerEnd,
	const char* fieldName)
{
	int32 fieldLen = strlen(fieldName);
	for (const char* p = headerStart; p + fieldLen < headerEnd; p++) {
		// Only match at the start of a line (or start of the header block).
		// Without this check we can falsely match header names embedded
		// inside values — e.g. ARC-Message-Signature's "h=" field lists
		// header names like "content-type" as plain text.
		if (p != headerStart && p[-1] != '\n')
			continue;
		if ((*p == fieldName[0] || *p == (fieldName[0] ^ 0x20))
			&& strncasecmp(p, fieldName, fieldLen) == 0) {
			// Found — extract value (skip whitespace after the field name)
			const char* value = p + fieldLen;
			while (value < headerEnd && (*value == ' ' || *value == '\t'))
				value++;
			// Collect until end of line (handling header folding: if the
			// next line starts with whitespace, it's a continuation)
			char result[512];
			int32 j = 0;
			while (value < headerEnd && j < (int32)sizeof(result) - 1) {
				if (*value == '\r' || *value == '\n') {
					// Skip the line ending
					const char* next = value;
					if (*next == '\r') next++;
					if (next < headerEnd && *next == '\n') next++;
					// Check for folded header (next line starts with space/tab)
					if (next < headerEnd
						&& (*next == ' ' || *next == '\t')) {
						result[j++] = ' ';
						value = next;
						// Skip leading whitespace of continuation
						while (value < headerEnd
							&& (*value == ' ' || *value == '\t'))
							value++;
						continue;
					}
					break;  // Not folded — end of this header
				}
				result[j++] = *value++;
			}
			// Trim trailing whitespace
			while (j > 0 && (result[j - 1] == ' ' || result[j - 1] == '\t'))
				j--;
			result[j] = '\0';
			return BString(result);
		}
	}
	return BString();
}


// Extract the boundary string from a Content-Type header value.
// e.g. from "multipart/alternative; boundary=\"----=_Part_123\""
// returns "----=_Part_123".
static BString
_ExtractBoundary(const BString& contentType)
{
	// Find "boundary=" (case-insensitive)
	int32 pos = contentType.IFindFirst("boundary=");
	if (pos < 0)
		return BString();

	const char* start = contentType.String() + pos + 9;  // skip "boundary="

	BString boundary;
	if (*start == '"') {
		// Quoted boundary — collect until closing quote
		start++;
		while (*start != '\0' && *start != '"')
			boundary << *start++;
	} else {
		// Unquoted — collect until whitespace, semicolon, or end
		while (*start != '\0' && *start != ' ' && *start != '\t'
			&& *start != ';' && *start != '\r' && *start != '\n')
			boundary << *start++;
	}

	return boundary;
}


// Decode a MIME part body given its encoding and length.
// Sets outText and returns true on success.
static bool
_DecodePart(const char* bodyStart, int32 bodyLen,
	mail_encoding encoding, bool isHtml, BString* outText)
{
	if (bodyLen <= 0)
		return false;

	if (encoding == base64 || encoding == quoted_printable
		|| encoding == uuencode) {
		char* decoded = new(std::nothrow) char[bodyLen + 1];
		if (decoded == NULL)
			return false;

		ssize_t decodedLen = decode(encoding, decoded, bodyStart,
			bodyLen, 0);
		if (decodedLen > 0) {
			decoded[decodedLen] = '\0';
			*outText = decoded;
		}
		delete[] decoded;
	} else {
		// 7bit, 8bit, or no encoding — use raw bytes
		outText->SetTo(bodyStart, bodyLen);
	}

	if (outText->Length() == 0)
		return false;

	if (isHtml)
		StripHtmlTags(outText);

	return true;
}


// Find the first text part (text/plain preferred, text/html as fallback)
// inside a multipart body. boundary is the MIME boundary string.
// regionStart/regionEnd delimit the body area to search.
// Sets outText and returns true if a text part was found and decoded.
// If recurse is true, handles one level of nested multipart.
static bool
_FindTextPart(const char* regionStart, const char* regionEnd,
	const BString& boundary, bool recurse, BString* outText)
{
	// Build the boundary delimiter: "--" + boundary
	BString delim("--");
	delim << boundary;
	int32 delimLen = delim.Length();

	// Collect all part start positions by scanning for the delimiter.
	// Each part starts after the delimiter line; the closing delimiter
	// has "--" appended (e.g. "--boundary--").
	struct PartInfo {
		const char* headerStart;
		const char* headerEnd;   // blank line
		const char* bodyStart;
		const char* bodyEnd;
	};

	// We only need to track a modest number of parts (most emails have
	// 2-5 parts).  Use a fixed array to avoid heap allocation.
	static const int32 kMaxParts = 16;
	PartInfo parts[kMaxParts];
	int32 partCount = 0;

	const char* pos = regionStart;
	while (pos < regionEnd && partCount < kMaxParts) {
		// Find next boundary
		const char* found = NULL;
		for (const char* p = pos; p + delimLen <= regionEnd; p++) {
			if (*p == '-' && *(p + 1) == '-'
				&& strncmp(p, delim.String(), delimLen) == 0) {
				found = p;
				break;
			}
		}
		if (found == NULL)
			break;

		// Check if this is the closing delimiter (--boundary--)
		const char* afterDelim = found + delimLen;
		if (afterDelim + 1 < regionEnd
			&& afterDelim[0] == '-' && afterDelim[1] == '-')
			break;  // End of multipart

		// Skip past the delimiter line (to the start of part headers)
		const char* lineEnd = afterDelim;
		while (lineEnd < regionEnd && *lineEnd != '\n')
			lineEnd++;
		if (lineEnd < regionEnd)
			lineEnd++;  // skip the '\n'

		const char* partStart = lineEnd;

		// Find the next boundary to know where this part ends
		const char* nextBoundary = NULL;
		for (const char* p = partStart; p + delimLen <= regionEnd; p++) {
			if (*p == '-' && *(p + 1) == '-'
				&& strncmp(p, delim.String(), delimLen) == 0) {
				nextBoundary = p;
				break;
			}
		}
		if (nextBoundary == NULL)
			nextBoundary = regionEnd;

		// Within this part, find the blank line separating part headers
		// from part body
		const char* partHeaderEnd = NULL;
		int32 partSepLen = 0;
		// Try \r\n\r\n first
		for (const char* p = partStart; p + 3 < nextBoundary; p++) {
			if (p[0] == '\r' && p[1] == '\n'
				&& p[2] == '\r' && p[3] == '\n') {
				partHeaderEnd = p;
				partSepLen = 4;
				break;
			}
		}
		if (partHeaderEnd == NULL) {
			// Try \n\n
			for (const char* p = partStart; p + 1 < nextBoundary; p++) {
				if (p[0] == '\n' && p[1] == '\n') {
					partHeaderEnd = p;
					partSepLen = 2;
					break;
				}
			}
		}

		if (partHeaderEnd != NULL) {
			PartInfo& part = parts[partCount++];
			part.headerStart = partStart;
			part.headerEnd = partHeaderEnd;
			part.bodyStart = partHeaderEnd + partSepLen;
			// Trim trailing \r\n before the next boundary
			part.bodyEnd = nextBoundary;
			while (part.bodyEnd > part.bodyStart
				&& (part.bodyEnd[-1] == '\r' || part.bodyEnd[-1] == '\n'))
				part.bodyEnd--;
		}

		pos = nextBoundary;
	}

	// Now scan parts: prefer text/plain, fall back to text/html.
	// If a part is itself multipart and recurse is true, descend into it.
	int32 htmlPartIndex = -1;

	for (int32 i = 0; i < partCount; i++) {
		BString ct = _FindHeader(parts[i].headerStart,
			parts[i].headerEnd, "Content-Type:");
		ct.ToLower();

		if (ct.FindFirst("text/plain") >= 0) {
			// Found text/plain — decode and return immediately
			BString cte = _FindHeader(parts[i].headerStart,
				parts[i].headerEnd, "Content-Transfer-Encoding:");
			mail_encoding enc = cte.Length() > 0
				? encoding_for_cte(cte.String()) : no_encoding;
			int32 partBodyLen = parts[i].bodyEnd - parts[i].bodyStart;
			if (_DecodePart(parts[i].bodyStart, partBodyLen, enc, false,
				outText))
				return true;
		}

		if (ct.FindFirst("text/html") >= 0 && htmlPartIndex < 0)
			htmlPartIndex = i;

		// Handle one level of nested multipart
		if (recurse && ct.FindFirst("multipart/") >= 0) {
			BString innerBoundary = _ExtractBoundary(ct);
			if (innerBoundary.Length() > 0) {
				// The inner multipart's body starts at parts[i].bodyStart
				// and ends at parts[i].bodyEnd (approximately — the body
				// extends to the outer boundary, so use nextBoundary region).
				// Actually, the inner body includes everything from the
				// part body start up to where the outer part ends.
				if (_FindTextPart(parts[i].bodyStart, parts[i].bodyEnd,
					innerBoundary, false, outText))
					return true;
			}
		}
	}

	// No text/plain found — try text/html fallback
	if (htmlPartIndex >= 0) {
		BString cte = _FindHeader(parts[htmlPartIndex].headerStart,
			parts[htmlPartIndex].headerEnd,
			"Content-Transfer-Encoding:");
		mail_encoding enc = cte.Length() > 0
			? encoding_for_cte(cte.String()) : no_encoding;
		int32 partBodyLen = parts[htmlPartIndex].bodyEnd
			- parts[htmlPartIndex].bodyStart;
		if (_DecodePart(parts[htmlPartIndex].bodyStart, partBodyLen, enc,
			true, outText))
			return true;
	}

	return false;
}


// Extract the plain text body from an email file. Returns true if text
// was successfully extracted. Uses lightweight mail_encoding decode()
// functions instead of BEmailMessage to avoid expensive MIME object
// construction. Handles:
//   - Single-part emails (7bit, 8bit, quoted-printable, base64)
//   - Multipart emails (finds the text/plain or text/html part)
//   - One level of nested multipart (e.g. multipart/mixed containing
//     multipart/alternative — the most common structure for emails
//     with attachments)
static bool
ExtractBodyText(const entry_ref& ref, BString* outText)
{
	if (outText == NULL)
		return false;

	outText->SetTo("");

	// Read the raw file
	BFile file(&ref, B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return false;

	off_t fileSize;
	file.GetSize(&fileSize);
	if (fileSize <= 0 || fileSize > 1024 * 1024)
		return false;  // Skip files > 1 MB

	char* buffer = new(std::nothrow) char[fileSize + 1];
	if (buffer == NULL)
		return false;

	ssize_t bytesRead = file.Read(buffer, fileSize);
	if (bytesRead <= 0) {
		delete[] buffer;
		return false;
	}
	buffer[bytesRead] = '\0';

	const char* bufferEnd = buffer + bytesRead;

	// Find the blank line separating headers from body
	const char* headerEnd = strstr(buffer, "\r\n\r\n");
	int32 separatorLen = 4;
	if (headerEnd == NULL) {
		headerEnd = strstr(buffer, "\n\n");
		separatorLen = 2;
	}

	if (headerEnd == NULL) {
		delete[] buffer;
		return false;
	}

	const char* bodyStart = headerEnd + separatorLen;
	if (bodyStart >= bufferEnd) {
		delete[] buffer;
		return false;
	}

	// Check if this is a multipart message
	BString contentType = _FindHeader(buffer, headerEnd, "Content-Type:");
	BString ctLower(contentType);
	ctLower.ToLower();

	bool ok = false;
	bool isMultipart = (ctLower.FindFirst("multipart/") >= 0);

	if (isMultipart) {
		// Multipart — extract boundary and find the text part
		BString boundary = _ExtractBoundary(contentType);
		if (boundary.Length() > 0)
			ok = _FindTextPart(bodyStart, bufferEnd, boundary, true, outText);
	}

	if (!ok && !isMultipart) {
		// Single-part email only — decode the whole body using the
		// top-level Content-Transfer-Encoding.  Do NOT fall through here
		// for multipart messages: the raw body contains undecoded base64
		// blobs, boundary markers, and part headers that would cause
		// false-positive search matches.
		BString cte = _FindHeader(buffer, headerEnd,
			"Content-Transfer-Encoding:");
		mail_encoding encoding = cte.Length() > 0
			? encoding_for_cte(cte.String()) : no_encoding;
		int32 bodyLen = bufferEnd - bodyStart;

		bool isHtml = (ctLower.FindFirst("text/html") >= 0);
		ok = _DecodePart(bodyStart, bodyLen, encoding, isHtml, outText);

		// If Content-Type wasn't explicit, check content for HTML
		if (ok && !isHtml && outText->Length() > 0) {
			BString lower(*outText);
			lower.ToLower();
			if (lower.FindFirst("<!doctype html") >= 0
				|| lower.FindFirst("<html") >= 0
				|| (lower.FindFirst("<head") >= 0
					&& lower.FindFirst("<body") >= 0)) {
				StripHtmlTags(outText);
			}
		}
	}

	delete[] buffer;
	return ok;
}


// Background item disposal to avoid UI stalls when clearing large lists.
// Deleting 20,000+ EmailItem/EmailRef objects plus clearing a large HashMap
// can take >1s on the UI thread. This moves both costs to a background thread.
struct _DisposerData {
    EmailItem** items;
    int32       count;
    std::unordered_map<node_ref, int32, NodeRefHash, NodeRefEqual>* hashMap;
};

static int32
_ItemDisposerThread(void* data)
{
    _DisposerData* d = (_DisposerData*)data;
    for (int32 i = 0; i < d->count; i++)
        delete d->items[i];
    delete[] d->items;
    delete d->hashMap;
    delete d;
    return 0;
}


static BBitmap*
_LoadIconFromResource(int32 resourceId, float size)
{
    BResources* resources = BApplication::AppResources();
    if (resources == NULL)
        return NULL;

    size_t dataSize;
    const void* data = resources->LoadResource('VICN', resourceId, &dataSize);
    if (data == NULL)
        return NULL;

    BBitmap* bitmap = new BBitmap(BRect(0, 0, size - 1, size - 1), B_RGBA32);
    if (bitmap->InitCheck() != B_OK) {
        delete bitmap;
        return NULL;
    }

    if (BIconUtils::GetVectorIcon((const uint8*)data, dataSize, bitmap) != B_OK) {
        delete bitmap;
        return NULL;
    }

    return bitmap;
}


static void
_LoadIcons(float size)
{
    if (sIconsLoaded)
        return;
    sIconsLoaded = true;

    sStarIcon = _LoadIconFromResource(kResStarred, size);
    sAttachmentIcon = _LoadIconFromResource(kResAttachment, size);
    sAttachmentWhiteIcon = _LoadIconFromResource(kResAttachmentWhite, size);

    if (sStarIcon == NULL)
        fprintf(stderr, "Warning: Star icon (resource %d) not found\n", kResStarred);
    if (sAttachmentIcon == NULL)
        fprintf(stderr, "Warning: Attachment icon (resource %d) not found\n", kResAttachment);
}

// Data passed to loader thread. Owns its own copies of volumes and shares
// the stop flag with EmailListView via shared_ptr (so the flag stays alive
// even if a new query replaces fCurrentStopFlag before this thread exits).
struct LoaderData {
    EmailListView*      view;
    BMessenger          messenger;
    BString             predicate;
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<BVolume, false> volumes;  // Does NOT own items - we delete manually
#else
    BObjectList<BVolume> volumes;  // Does NOT own items - we delete manually
#endif
    bool                showTrash;
    bool                showSpam;
    bool                attachmentsOnly;
    std::set<BString>   spamBlocklist;  // copy of sender blocklist for background filtering
    std::shared_ptr<volatile bool> stopFlag;  // Shared ownership with EmailListView
    volatile int32*     currentQueryId; // Points to EmailListView::fCurrentQueryId for staleness check
    time_t              cutoffTime;     // 30 days ago timestamp
    int32               phase;          // 1 = recent, 2 = older
    int32               queryId;        // Unique ID to identify this query session
    
    ~LoaderData() {
        // Manually delete volumes since we set owning to false
        for (int32 i = 0; i < volumes.CountItems(); i++) {
            delete volumes.ItemAt(i);
        }
        // stopFlag shared_ptr releases automatically
    }
};


// =============================================================================
// EmailRef out-of-line method
// Defined here (not in EmailRef.h) to break the circular dependency:
// EmailRef.h → EmailAccountMap.h → (large include chain). Keeping EmailRef.h
// lightweight is important because it's included everywhere.
// =============================================================================

void
EmailRef::_ResolveAccountName(int32 accountId)
{
    account = EmailAccountMap::Instance().GetAccountName(accountId);
}


// =============================================================================
// EmailItem implementation
// =============================================================================

EmailItem::EmailItem(EmailRef* ref)
    :
    fRef(ref),
    fSelected(false),
    fPathValid(false),
    fDateStringValid(false)
{
}


EmailItem::~EmailItem()
{
    delete fRef;
}


// === EmailViews API accessors ===

const char*
EmailItem::GetPath() const
{
    if (fRef == NULL)
        return "";
    
    if (!fPathValid) {
        BEntry entry(&fRef->entryRef);
        if (entry.InitCheck() == B_OK) {
            BPath path;
            if (entry.GetPath(&path) == B_OK) {
                fPath = path.Path();
            }
        }
        fPathValid = true;
    }
    return fPath.String();
}


const char*
EmailItem::GetStatus() const
{
    if (fRef == NULL)
        return "";
    return fRef->status.String();
}


const char*
EmailItem::GetAccount() const
{
    if (fRef == NULL)
        return "";
    return fRef->account.String();
}


const char*
EmailItem::GetFrom() const
{
    if (fRef == NULL)
        return "";
    return fRef->from.String();
}


const char*
EmailItem::GetTo() const
{
    if (fRef == NULL)
        return "";
    return fRef->to.String();
}


const char*
EmailItem::GetSubject() const
{
    if (fRef == NULL)
        return "";
    return fRef->subject.String();
}


time_t
EmailItem::GetWhen() const
{
    if (fRef == NULL)
        return 0;
    return fRef->when;
}


const timespec&
EmailItem::GetCrtime() const
{
    static timespec empty = {0, 0};
    if (fRef == NULL)
        return empty;
    return fRef->crtime;
}


const node_ref*
EmailItem::GetNodeRef() const
{
    if (fRef == NULL)
        return NULL;
    return &fRef->nodeRef;
}


const entry_ref&
EmailItem::GetEntryRef() const
{
    static entry_ref empty;
    if (fRef == NULL)
        return empty;
    return fRef->entryRef;
}


bool
EmailItem::IsRead() const
{
    if (fRef == NULL)
        return true;
    return fRef->isRead;
}


bool
EmailItem::HasAttachment() const
{
    if (fRef == NULL)
        return false;
    return fRef->hasAttachment;
}


bool
EmailItem::IsStarred() const
{
    if (fRef == NULL)
        return false;
    return fRef->isStarred;
}


void
EmailItem::SetRead(bool read)
{
    if (fRef != NULL) {
        fRef->isRead = read;
        // Update status string to match
        if (read && fRef->status.ICompare("New") == 0) {
            fRef->status = "Read";
        } else if (!read) {
            fRef->status = "New";
        }
    }
}


void
EmailItem::SetStatus(const char* status)
{
    if (fRef != NULL && status != NULL) {
        fRef->status = status;
        fRef->isRead = (fRef->status.ICompare("New") != 0);
    }
}


void
EmailItem::SetStarred(bool starred)
{
    if (fRef != NULL) {
        fRef->isStarred = starred;
    }
}


void
EmailItem::SetHasAttachment(bool hasAttachment)
{
    if (fRef != NULL) {
        fRef->hasAttachment = hasAttachment;
    }
}


// === Display string methods (for drawing) ===

const char*
EmailItem::Status()
{
    if (fRef == NULL)
        return "";
    return fRef->status.String();
}


const char*
EmailItem::Subject()
{
    if (fRef == NULL)
        return "(No Data)";
    if (fRef->subject.Length() == 0)
        return "(No Subject)";
    return fRef->subject.String();
}


const char*
EmailItem::From()
{
    if (fRef == NULL)
        return "(No Data)";
    if (fRef->from.Length() == 0)
        return "(Unknown Sender)";
    return fRef->from.String();
}


const char*
EmailItem::To()
{
    if (fRef == NULL)
        return "";
    return fRef->to.String();
}


const char*
EmailItem::DateString()
{
    if (fRef == NULL)
        return "";
    
    // Format date string on first access (cached in item)
    if (!fDateStringValid) {
        if (fRef->when > 0) {
            BDateTimeFormat dateTimeFormat;
            dateTimeFormat.Format(fDateString, fRef->when, 
                                  B_SHORT_DATE_FORMAT, B_SHORT_TIME_FORMAT);
        } else {
            fDateString = "";
        }
        fDateStringValid = true;
    }
    return fDateString.String();
}


const char*
EmailItem::Account()
{
    if (fRef == NULL)
        return "";
    return fRef->account.String();
}


void
EmailItem::InvalidateCache()
{
    fPathValid = false;
    fDateStringValid = false;
}


// =============================================================================
// EmailListView::ContentView implementation (inner class for drawing rows)
// =============================================================================

EmailListView::ContentView::ContentView(EmailListView* parent)
    :
    BView("emailListContent", B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE),
    fParent(parent)
{
    SetViewUIColor(B_LIST_BACKGROUND_COLOR);
    SetLowUIColor(B_LIST_BACKGROUND_COLOR);
}


void
EmailListView::ContentView::Draw(BRect updateRect)
{
    fParent->_DrawContent(this, updateRect);
}


void
EmailListView::ContentView::FrameResized(float width, float height)
{
    BView::FrameResized(width, height);
    fParent->_UpdateScrollBar();
}


void
EmailListView::ContentView::ScrollTo(BPoint where)
{
    BView::ScrollTo(where);
    // Re-evaluate which rows need node monitors after scroll
    fParent->_UpdateVisibleWatches();
    // Keep column header horizontally aligned with list content
    if (fParent->fColumnHeader != NULL) {
        fParent->fColumnHeader->ScrollToH(where.x);
    }
}


void
EmailListView::ContentView::MouseDown(BPoint where)
{
    // Make us focused on click
    if (!IsFocus())
        MakeFocus(true);
    
    fParent->_HandleMouseDown(this, where);
}


void
EmailListView::ContentView::KeyDown(const char* bytes, int32 numBytes)
{
    fParent->_HandleKeyDown(bytes, numBytes);
}


void
EmailListView::ContentView::MakeFocus(bool focus)
{
    BView::MakeFocus(focus);
    // Redraw to show/hide focus indicator if needed
    Invalidate();
}


void
EmailListView::ContentView::AttachedToWindow()
{
    BView::AttachedToWindow();
    SetFlags(Flags() | B_NAVIGABLE);
}


// =============================================================================
// EmailListView implementation (container view)
// =============================================================================

EmailListView::EmailListView(const char* name, BWindow* target)
    :
    BGroupView(name, B_VERTICAL, 0),
    fItems(20),
    fTarget(target),
    fRowHeight(20.0f),
    fAnchorIndex(-1),
    fLastClickIndex(-1),
    fSelectionIterIndex(0),
    fSortColumn(kSortByDate),
    fSortOrder(kSortDescending),
    fBulkLoading(false),
    fShowingTrash(false),
    fShowingSpam(false),
    fContentView(NULL),
    fColumnHeader(NULL),
    fScrollView(NULL),
    fVScrollBar(NULL),
    fHScrollBar(NULL),
    fStatusLabel(NULL),
    fLoadingDots(NULL),
    fSelectionMessage(NULL),
    fInvocationMessage(NULL),
    fLoaderThread(-1),
    fCurrentQueryId(0),
    fQueryShowTrash(false),
    fQueryShowSpam(false),
    fQueryAttachmentsOnly(false),
    fQueryCutoffTime(0),
    fLoadedCount(0),
    fTotalCount(0),
    fBodySearchThread(-1),
    fBodySearchTotal(0),
    fBodySearchScanned(0),
    fBodySearchFound(0)
{
    // Create internal components
    
    // 1. Column header
    fColumnHeader = new EmailColumnHeaderView("columnHeader");
    
    // 2. Content view (the scrollable list)
    fContentView = new ContentView(this);
    
    // 3. Scroll view WITHOUT scrollbars and NO border (scrollbars are separate,
    //    individual components draw their own border lines)
    //    BScrollView takes ownership of fContentView and adds it as child
    fScrollView = new BScrollView("scrollView", fContentView,
        B_WILL_DRAW | B_FRAME_EVENTS, false, false, B_NO_BORDER);
    fScrollView->SetExplicitMinSize(BSize(0, 0));
    fScrollView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    
    // 4. Vertical scrollbar (separate, spans full height)
    //    Pass NULL as target initially, then set it
    fVScrollBar = new BScrollBar("vscroll", NULL, 0, 0, B_VERTICAL);
    fVScrollBar->SetTarget(fContentView);

    // 5. Horizontal scrollbar (separate)
    fHScrollBar = new BScrollBar("hscroll", NULL, 0, 0, B_HORIZONTAL);
    fHScrollBar->SetTarget(fContentView);

    // 6. Status label and loading dots — live left of the horizontal scrollbar,
    //    sized from font metrics so they scale with the system font.
    float hScrollHeight = fHScrollBar->PreferredSize().height;
    fStatusLabel = new BStringView("statusLabel", "");
    fStatusLabel->SetAlignment(B_ALIGN_LEFT);
    BFont statusFont;
    fStatusLabel->GetFont(&statusFont);

    // Scale font down if it doesn't fit in the scrollbar height.
    // Leave 2px padding (1px top + 1px bottom) for visual breathing room.
    float availableHeight = hScrollHeight - 2.0f;
    font_height fh;
    statusFont.GetHeight(&fh);
    float textHeight = fh.ascent + fh.descent;
    if (textHeight > availableHeight && availableHeight > 0) {
        float scale = availableHeight / textHeight;
        statusFont.SetSize(floorf(statusFont.Size() * scale));
        fStatusLabel->SetFont(&statusFont);
    }

    // Measure "999,999 emails" as a representative worst-case width.
    // Add generous padding (20%) to accommodate translated strings that may
    // be longer than the English original.
    float statusLabelWidth = statusFont.StringWidth("999,999 emails") * 1.2f + 4;
    fStatusLabel->SetExplicitMinSize(BSize(statusLabelWidth, hScrollHeight));
    fStatusLabel->SetExplicitMaxSize(BSize(statusLabelWidth, hScrollHeight));

    float dotAreaHeight = ceilf((statusFont.Size() * 1.0f));
    float dotsWidth = dotAreaHeight * 2.5f;
    fLoadingDots = new LoadingDots("loadingDots");
    fLoadingDots->SetExplicitMinSize(BSize(dotsWidth, hScrollHeight));
    fLoadingDots->SetExplicitMaxSize(BSize(dotsWidth, hScrollHeight));
    fLoadingDots->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_VERTICAL_CENTER));

    // 7. Corner spacer
    BView* cornerSpacer = new CornerSpacerView("cornerSpacer");
    cornerSpacer->SetExplicitMinSize(BSize(0, hScrollHeight));
    cornerSpacer->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, hScrollHeight));

    // Link column header to this list
    fColumnHeader->SetListView(this);

    // Build layout with individual border drawing on each component.
    // - Column header draws its own top and bottom borders
    // - LeftBorderView draws left border spanning full height
    // - StatusAreaView draws bottom border under status/loading area
    // - CornerSpacerView draws bottom and right borders
    // - Scrollbars draw their own complete borders
    // - Right border provided by window frame (view pushed to edge)
    // ┌─────────────────────────────────────┐
    // │[Column Header           ][V-Scrollbar]│
    // │[Content View            ][           ]│
    // │[Status][Dots][H-Scrollbar][Corner    ]│
    // └─────────────────────────────────────┘
    SetExplicitMinSize(BSize(0, 0));
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

    // Create a view that draws only the left border
    LeftBorderView* borderView = new LeftBorderView("border");

    // StatusAreaView wraps status label + loading dots and draws bottom border
    StatusAreaView* statusArea = new StatusAreaView("statusArea");
    BLayoutBuilder::Group<>(statusArea, B_HORIZONTAL, 0)
        .SetInsets(be_control_look->DefaultLabelSpacing(), 0,
                   be_control_look->DefaultLabelSpacing(), 0)
        .Add(fStatusLabel, 0)
        .Add(fLoadingDots, 0);

    // Create the content layout
    BGroupLayout* innerLayout = BLayoutBuilder::Group<>(B_HORIZONTAL, 0)
        // Left side: header + content + bottom bar
        .AddGroup(B_VERTICAL, 0, 1.0f)
            .Add(fColumnHeader)
            .Add(fScrollView, 1.0f)
            .AddGroup(B_HORIZONTAL, 0)
                .Add(statusArea, 0)
                .Add(fHScrollBar, 1.0f)
            .End()
        .End()
        // Right side: vertical scrollbar spanning full height + corner
        .AddGroup(B_VERTICAL, 0)
            .Add(fVScrollBar, 1.0f)
            .Add(cornerSpacer)
        .End()
        .SetInsets(1, 0, 0, 0);
    
    // Add the layout's view to the border view
    borderView->AddChild(innerLayout->View());
    
    // Add the border view to this GroupView
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(borderView, 1.0f)
        .SetInsets(0);
}


EmailListView::~EmailListView()
{
    if (fCurrentStopFlag)
        *fCurrentStopFlag = true;
    if (fLoaderThread >= 0) {
        status_t result;
        wait_for_thread(fLoaderThread, &result);
        fLoaderThread = -1;
    }
    if (fBodySearchStopFlag)
        *fBodySearchStopFlag = true;
    if (fBodySearchThread >= 0) {
        status_t result;
        wait_for_thread(fBodySearchThread, &result);
        fBodySearchThread = -1;
    }
    _StopLiveQueries();
    
    // fCurrentStopFlag shared_ptr releases automatically (thread has exited)
    
    // Explicitly delete all items (fItems is non-owning)
    for (int32 i = 0; i < fItems.CountItems(); i++)
        delete fItems.ItemAt(i);
    
    delete fSelectionMessage;
    delete fInvocationMessage;
}


void
EmailListView::AttachedToWindow()
{
    BGroupView::AttachedToWindow();
}


void
EmailListView::AllAttached()
{
    BGroupView::AllAttached();
    
    // Calculate row height based on content view's font
    if (fContentView != NULL) {
        font_height fontHeight;
        fContentView->GetFontHeight(&fontHeight);
        fRowHeight = ceilf((fontHeight.ascent + fontHeight.descent 
                            + fontHeight.leading) * 1.4f);
        if (fRowHeight < 20.0f)
            fRowHeight = 20.0f;
    }
    
    // Set up colors from system theme
    fBackgroundColor = ui_color(B_LIST_BACKGROUND_COLOR);
    fSelectedColor = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
    fTextColor = ui_color(B_LIST_ITEM_TEXT_COLOR);
    fSelectedTextColor = ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);
    _UpdateStripeColor();
    
    // Load row icons at row height size
    _LoadIcons(fRowHeight - 4);

    // Set header icons for icon columns — loaded at header height so they
    // fit within the header without overflowing at any font size.
    if (fColumnHeader != NULL) {
        float headerIconSize = fColumnHeader->GetHeaderHeight() - 4;
        BBitmap* starHeader = _LoadIconFromResource(kResStarred, headerIconSize);
        BBitmap* attachHeader = _LoadIconFromResource(kResAttachment, headerIconSize);
        if (starHeader != NULL)
            fColumnHeader->SetColumnHeaderIcon(kColumnStar, starHeader);
        if (attachHeader != NULL)
            fColumnHeader->SetColumnHeaderIcon(kColumnAttachment, attachHeader);
    }
    
    _UpdateScrollBar();
}


void
EmailListView::DetachedFromWindow()
{
    if (fCurrentStopFlag)
        *fCurrentStopFlag = true;
    if (fLoaderThread >= 0) {
        status_t result;
        wait_for_thread(fLoaderThread, &result);
        fLoaderThread = -1;
    }
    _StopLiveQueries();
    _StopAllWatches();
    BGroupView::DetachedFromWindow();
}


void
EmailListView::MakeFocus(bool focus)
{
    // Forward to content view
    if (fContentView != NULL)
        fContentView->MakeFocus(focus);
}


void
EmailListView::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kMsgLoaderBatch:
        {
            // Check if this batch is from the current query
            int32 queryId = 0;
            message->FindInt32("queryId", &queryId);
            if (queryId != fCurrentQueryId) {
                // Stale batch from old query - delete the EmailRefs and ignore
                void* ptr;
                int32 index = 0;
                while (message->FindPointer("emailref", index++, &ptr) == B_OK) {
                    delete (EmailRef*)ptr;
                }
                break;
            }
            
            // Batch from background loader thread
            _ProcessLoaderBatch(message);
            break;
        }
        
        case kMsgPhase1Done:
        {
            // Check if this is from the current query
            int32 queryId = 0;
            message->FindInt32("queryId", &queryId);
            if (queryId != fCurrentQueryId) {
                break;
            }
            
            // Phase 1 complete - recent emails loaded with sorted insertion
            fLoaderThread = -1;
            
            // Start live queries now so new emails appear
            _StartLiveQueries();
            
            if (fQueryCutoffTime == 0) {
                // Single-phase loading (no MAIL:when split) — we're done
                SortItems();
                
                fNodeToIndex.clear();
                for (int32 i = 0; i < fItems.CountItems(); i++) {
                    EmailItem* item = fItems.ItemAt(i);
                    if (item != NULL && item->Ref() != NULL)
                        fNodeToIndex[item->Ref()->nodeRef] = i;
                }
                
                _UpdateScrollBar();
                if (fContentView != NULL) fContentView->Invalidate();
                _SendLoadingUpdate(false, true);
                break;
            }
            
            // Notify target - phase 1 complete, user can interact
            _SendLoadingUpdate(true, false);
            
            // Launch phase 2 - older emails
            LoaderData* data = new LoaderData();
            data->view = this;
            data->messenger = BMessenger(this);
            data->predicate = fQueryPredicate;
            data->showTrash = fQueryShowTrash;
            data->showSpam = fQueryShowSpam;
            data->attachmentsOnly = fQueryAttachmentsOnly;
            data->spamBlocklist = fSpamBlocklist;
            data->stopFlag = fCurrentStopFlag;
            data->currentQueryId = &fCurrentQueryId;
            data->cutoffTime = fQueryCutoffTime;
            data->phase = 2;
            data->queryId = fCurrentQueryId;
            
            for (int32 i = 0; i < fQueryVolumes.CountItems(); i++) {
                BVolume* vol = fQueryVolumes.ItemAt(i);
                if (vol != NULL)
                    data->volumes.AddItem(new BVolume(*vol));
            }
            
            fLoaderThread = spawn_thread(_LoaderThread, "email_loader_p2",
                                          B_LOW_PRIORITY, data);
            if (fLoaderThread >= 0) {
                resume_thread(fLoaderThread);
            } else {
                delete data;
                _SendLoadingUpdate(false, true);
            }
            break;
        }
        
        case kMsgPhase2Done:
        {
            // Check if this is from the current query
            int32 queryId = 0;
            message->FindInt32("queryId", &queryId);
            if (queryId != fCurrentQueryId) {
                break;
            }
            
            // Phase 2 complete - all emails loaded
            fLoaderThread = -1;
            
            // Final sort to fix any ordering edge cases
            SortItems();
            
            // Rebuild HashMap after sort
            fNodeToIndex.clear();
            for (int32 i = 0; i < fItems.CountItems(); i++) {
                EmailItem* item = fItems.ItemAt(i);
                if (item != NULL && item->Ref() != NULL) {
                    fNodeToIndex[item->Ref()->nodeRef] = i;
                }
            }
            
            // Final UI update
            _UpdateScrollBar();
            if (fContentView != NULL) fContentView->Invalidate();
            
            // Notify target - all complete
            _SendLoadingUpdate(false, true);
            break;
        }
        
        case kMsgLoaderDone:
        {
            // Legacy - kept for compatibility
            fLoaderThread = -1;
            EndBulkLoad();
            SortItems();
            _UpdateScrollBar();
            if (fContentView != NULL) fContentView->Invalidate();
            _StartLiveQueries();
            _SendLoadingUpdate(false, true);
            break;
        }
        
        case kMsgAddEmails:
        {
            // Batch of emails from background thread (legacy)
            entry_ref ref;
            int32 index = 0;
            while (message->FindRef("ref", index, &ref) == B_OK) {
                EmailRef* emailRef = new EmailRef(ref);
                AddEmailSorted(emailRef);
                index++;
            }
            break;
        }
        
        case kMsgQueryComplete:
        {
            // Query finished - end bulk loading mode (legacy)
            EndBulkLoad();
            break;
        }
        
        case kMsgEmailAdded:
        {
            // Live query: new email arrived
            entry_ref ref;
            if (message->FindRef("ref", &ref) == B_OK) {
                EmailRef* emailRef = new EmailRef(ref);
                AddEmailSorted(emailRef);
            }
            break;
        }
        
        case kMsgEmailRemoved:
        {
            // Live query: email removed
            node_ref nodeRef;
            dev_t device;
            ino_t node;
            
            if (message->FindInt32("device", &device) == B_OK &&
                message->FindInt64("node", &node) == B_OK) {
                nodeRef.device = device;
                nodeRef.node = node;
                RemoveEmail(nodeRef);
            }
            break;
        }
        
        case kMsgEmailChanged:
        {
            // Live query: email attribute changed
            node_ref nodeRef;
            dev_t device;
            ino_t node;
            
            if (message->FindInt32("device", &device) == B_OK &&
                message->FindInt64("node", &node) == B_OK) {
                nodeRef.device = device;
                nodeRef.node = node;
                
                // Find the item and reload from disk
                EmailItem* item = ItemFor(nodeRef);
                if (item != NULL) {
                    if (item->Ref() != NULL)
                        item->Ref()->ReloadAttributes();
                    item->InvalidateCache();
                    
                    // Redraw if visible
                    int32 index = IndexOf(nodeRef);
                    if (index >= _FirstVisibleIndex() && index <= _LastVisibleIndex()) {
                        if (fContentView != NULL) fContentView->Invalidate(_RowRect(index));
                    }
                }
            }
            break;
        }
        
        case B_NODE_MONITOR:
        {
            // Node monitor notification for watched visible emails
            int32 opcode;
            if (message->FindInt32("opcode", &opcode) != B_OK)
                break;
            
            if (opcode == B_ATTR_CHANGED) {
                node_ref nodeRef;
                dev_t device;
                ino_t node;
                
                if (message->FindInt32("device", &device) == B_OK &&
                    message->FindInt64("node", &node) == B_OK) {
                    nodeRef.device = device;
                    nodeRef.node = node;
                    
                    // Find the item by linear scan to avoid stale HashMap
                    // indices during Phase 2 loading
                    int32 index;
                    EmailItem* item = _FindItemByNodeRef(nodeRef, &index);
                    if (item != NULL) {
                        if (item->Ref() != NULL)
                            item->Ref()->ReloadAttributes();
                        item->InvalidateCache();
                        
                        // Redraw if visible
                        if (index >= _FirstVisibleIndex() && index <= _LastVisibleIndex()) {
                            if (fContentView != NULL) fContentView->Invalidate(_RowRect(index));
                        }
                    }
                }
            }
            break;
        }
        
        case B_COLORS_UPDATED:
        {
            // Theme changed - refresh cached colors
            fBackgroundColor = ui_color(B_LIST_BACKGROUND_COLOR);
            fSelectedColor = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
            fTextColor = ui_color(B_LIST_ITEM_TEXT_COLOR);
            fSelectedTextColor = ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);
            _UpdateStripeColor();
            if (fContentView != NULL) fContentView->Invalidate();
            break;
        }
        
        case kMsgMarkAsRead:
            _MarkSelectedAs("Read");
            break;
        
        case kMsgMarkAsUnread:
            _MarkSelectedAs("New");
            break;
        
        case kMsgMarkAsSent:
            _MarkSelectedAs("Sent");
            break;
        
        case kMsgMoveToTrash:
            _MoveSelectedToTrash();
            break;
        
        case EmailColumnHeaderView::kMsgColumnClicked:
        {
            // Column header clicked - handle sorting
            int32 column = 0;
            int32 order = 0;
            if (message->FindInt32("column", &column) == B_OK &&
                message->FindInt32("order", &order) == B_OK) {
                // Map EmailColumn to EmailListView sort column
                SortColumn sortCol;
                switch (column) {
                    case kColumnStatus:
                        sortCol = kSortByStatus;
                        break;
                    case kColumnStar:
                        sortCol = kSortByStar;
                        break;
                    case kColumnAttachment:
                        sortCol = kSortByAttachment;
                        break;
                    case kColumnFrom:
                        sortCol = kSortByFrom;
                        break;
                    case kColumnTo:
                        sortCol = kSortByTo;
                        break;
                    case kColumnSubject:
                        sortCol = kSortBySubject;
                        break;
                    case kColumnDate:
                        sortCol = kSortByDate;
                        break;
                    case kColumnAccount:
                        sortCol = kSortByAccount;
                        break;
                    default:
                        sortCol = kSortByDate;
                        break;
                }
                
                SortOrder sortOrder = (order == kSortAscending) 
                    ? kSortAscending 
                    : kSortDescending;
                
                SetSortColumn(sortCol, sortOrder);
            }
            break;
        }
        
        case B_QUERY_UPDATE:
        {
            // Live query notification - handle entry created/removed/changed
            int32 opcode;
            if (message->FindInt32("opcode", &opcode) != B_OK)
                break;
            
            switch (opcode) {
                case B_ENTRY_CREATED:
                {
                    entry_ref ref;
                    const char* name;
                    ino_t directory;
                    dev_t device;
                    
                    if (message->FindInt32("device", &device) != B_OK ||
                        message->FindInt64("directory", &directory) != B_OK ||
                        message->FindString("name", &name) != B_OK)
                        break;
                    
                    ref.device = device;
                    ref.directory = directory;
                    ref.set_name(name);
                    
                    // Check if already in list. Live query initial results overlap
                    // with loader results because both run the same predicate.
                    // The HashMap dedup prevents duplicates.
                    node_ref nodeRef;
                    nodeRef.device = device;
                    ino_t node;
                    if (message->FindInt64("node", &node) == B_OK) {
                        nodeRef.node = node;
                        if (fNodeToIndex.find(nodeRef) != fNodeToIndex.end())
                            break;  // Already have this email
                    }
                    
                    // Filter by trash status. Path comparison is case-insensitive
                    // because BFS is case-insensitive by default, and some volumes
                    // may have mixed-case trash directory names.
                    BPath path(&ref);
                    if (path.InitCheck() == B_OK) {
                        BVolume volume(device);
                        BPath trashPath;
                        if (find_directory(B_TRASH_DIRECTORY, &trashPath, false, &volume) == B_OK) {
                            BString pathStr(path.Path());
                            BString trashStr(trashPath.Path());
                            pathStr.ToLower();
                            trashStr.ToLower();
                            bool inTrash = (trashStr.Length() > 0 &&
                                           pathStr.FindFirst(trashStr) >= 0);
                            if (fQueryShowTrash && !inTrash)
                                break;
                            if (!fQueryShowTrash && inTrash)
                                break;
                        }
                    }
                    
                    // Filter by spam classification
                    {
                        BNode attrNode(&ref);
                        BString classification;
                        bool isSpam = (attrNode.InitCheck() == B_OK
                            && attrNode.ReadAttrString("MAIL:classification",
                                &classification) == B_OK
                            && classification.ICompare("Spam") == 0);

                        // Check sender against blocklist for newly arrived emails
                        if (!isSpam && !fSpamBlocklist.empty()
                            && attrNode.InitCheck() == B_OK) {
                            char fromBuf[256] = "";
                            attrNode.ReadAttr("MAIL:from", B_STRING_TYPE, 0,
                                fromBuf, sizeof(fromBuf) - 1);
                            if (fromBuf[0] != '\0') {
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

                                bool blocked = (fSpamBlocklist.count(addr) > 0);
                                if (!blocked) {
                                    int32 at = addr.FindFirst('@');
                                    if (at >= 0) {
                                        BString domain;
                                        addr.CopyInto(domain, at, addr.Length() - at);
                                        blocked = (fSpamBlocklist.count(domain) > 0);
                                    }
                                }
                                if (blocked) {
                                    BString spam("Spam");
                                    attrNode.WriteAttrString("MAIL:classification", &spam);
                                    isSpam = true;
                                }
                            }
                        }

                        if (fQueryShowSpam && !isSpam)
                            break;
                        if (!fQueryShowSpam && !fQueryShowTrash && isSpam)
                            break;
                    }
                    
                    // Filter by attachments if needed
                    if (fQueryAttachmentsOnly) {
                        EmailRef* tempRef = new EmailRef(ref);
                        if (!tempRef->hasAttachment) {
                            delete tempRef;
                            break;
                        }
                        AddEmailSorted(tempRef);
                    } else {
                        EmailRef* emailRef = new EmailRef(ref);
                        AddEmailSorted(emailRef);
                    }
                    
                    // Redraw for live query additions (single items)
                    if (fContentView != NULL)
                        fContentView->Invalidate();
                    
                    // Notify parent to update email count (throttled)
                    _NotifyCountChanged();
                    break;
                }
                
                case B_ENTRY_REMOVED:
                {
                    node_ref nodeRef;
                    dev_t device;
                    ino_t node;
                    
                    if (message->FindInt32("device", &device) != B_OK ||
                        message->FindInt64("node", &node) != B_OK)
                        break;
                    
                    nodeRef.device = device;
                    nodeRef.node = node;
                    
                    RemoveEmail(nodeRef);
                    
                    // Notify parent to update email count (throttled)
                    _NotifyCountChanged();
                    break;
                }
                
                case B_ATTR_CHANGED:
                {
                    node_ref nodeRef;
                    dev_t device;
                    ino_t node;
                    
                    if (message->FindInt32("device", &device) != B_OK ||
                        message->FindInt64("node", &node) != B_OK)
                        break;
                    
                    nodeRef.device = device;
                    nodeRef.node = node;
                    
                    // Find the item by linear scan to avoid stale HashMap
                    // indices during Phase 2 loading
                    int32 index;
                    EmailItem* item = _FindItemByNodeRef(nodeRef, &index);
                    if (item != NULL) {
                        // Re-read changed attributes from disk
                        if (item->Ref() != NULL)
                            item->Ref()->ReloadAttributes();
                        item->InvalidateCache();
                        
                        if (index >= _FirstVisibleIndex()
                            && index <= _LastVisibleIndex()) {
                            if (fContentView != NULL)
                                fContentView->Invalidate(_RowRect(index));
                        }
                    }
                    break;
                }
            }
            break;
        }
        
        case kMsgBodySearchBatch:
        {
            // Batch of matching emails from body search thread
            int32 queryId = 0;
            message->FindInt32("queryId", &queryId);
            if (queryId != fCurrentQueryId) {
                // Stale batch — delete the EmailRefs
                void* ptr;
                int32 index = 0;
                while (message->FindPointer("emailref", index++, &ptr) == B_OK)
                    delete (EmailRef*)ptr;
                break;
            }
            
            void* ptr;
            int32 index = 0;
            int32 added = 0;
            while (message->FindPointer("emailref", index++, &ptr) == B_OK) {
                EmailRef* emailRef = (EmailRef*)ptr;
                if (emailRef != NULL) {
                    AddEmailSorted(emailRef);
                    added++;
                }
            }
            
            if (added > 0) {
                // Redraw the content view and update scrollbar
                _UpdateScrollBar();
                if (fContentView != NULL)
                    fContentView->Invalidate();
                if (Window() != NULL)
                    Window()->UpdateIfNeeded();
            }
            
            // Update status with scanned count
            fBodySearchFound = fItems.CountItems();
            int32 scanned = 0;
            message->FindInt32("scanned", &scanned);
            fBodySearchScanned = scanned;
            BString status;
            status.SetToFormat("%ld of %ld",
                (long)fBodySearchScanned, (long)fBodySearchTotal);
            SetStatusText(status.String());
            
            // Show content as matches arrive
            _SendLoadingUpdate(true, false);
            break;
        }
        
        case kMsgBodySearchProgress:
        {
            // Progress update from body search thread (scanned count)
            int32 queryId = 0;
            message->FindInt32("queryId", &queryId);
            if (queryId != fCurrentQueryId)
                break;
            
            int32 scanned = 0;
            message->FindInt32("scanned", &scanned);
            fBodySearchScanned = scanned;
            
            // Update status: "N of M" where N = scanned, M = total
            BString status;
            status.SetToFormat("%ld of %ld",
                (long)fBodySearchScanned, (long)fBodySearchTotal);
            SetStatusText(status.String());
            break;
        }
        
        case kMsgBodySearchDone:
        {
            int32 queryId = 0;
            message->FindInt32("queryId", &queryId);
            if (queryId != fCurrentQueryId)
                break;
            
            fBodySearchThread = -1;
            fBodySearchFound = fItems.CountItems();
            
            // Final sort and HashMap rebuild
            SortItems();
            fNodeToIndex.clear();
            for (int32 i = 0; i < fItems.CountItems(); i++) {
                EmailItem* item = fItems.ItemAt(i);
                if (item != NULL && item->Ref() != NULL)
                    fNodeToIndex[item->Ref()->nodeRef] = i;
            }
            
            _UpdateScrollBar();
            if (fContentView != NULL) fContentView->Invalidate();
            _SendLoadingUpdate(false, true);
            break;
        }
        
        default:
            BGroupView::MessageReceived(message);
            break;
    }
}


void
EmailListView::_DrawContent(BView* view, BRect updateRect)
{
    // Draw only rows that intersect updateRect
    // Note: updateRect is in view coordinates (which scroll)
    
    int32 count = fItems.CountItems();
    if (count == 0) {
        // Empty state - just fill with background
        view->SetHighColor(fBackgroundColor);
        view->FillRect(updateRect);
        return;
    }
    
    // Calculate visible range based on updateRect
    int32 firstVisible = std::max((int32)0, (int32)(updateRect.top / fRowHeight));
    int32 lastVisible = std::min(count - 1, (int32)(updateRect.bottom / fRowHeight));
    
    // Fill background above first row if needed
    float firstRowTop = firstVisible * fRowHeight;
    if (updateRect.top < firstRowTop) {
        BRect topRect = updateRect;
        topRect.bottom = firstRowTop - 1;
        if (topRect.IsValid()) {
            view->SetHighColor(fBackgroundColor);
            view->FillRect(topRect);
        }
    }
    
    // Draw visible rows
    for (int32 i = firstVisible; i <= lastVisible; i++) {
        EmailItem* item = fItems.ItemAt(i);
        if (item == NULL)
            continue;
        
        BRect rowRect = _RowRect(i);
        
        // Only draw if row intersects update rect
        if (rowRect.Intersects(updateRect)) {
            bool isSelected = IsSelected(i);
            _DrawRow(view, item, rowRect, isSelected, i);
        }
    }
    
    // Fill background below last row if needed
    float lastRowBottom = (lastVisible + 1) * fRowHeight;
    if (lastRowBottom < updateRect.bottom) {
        BRect bottomRect = updateRect;
        bottomRect.top = lastRowBottom;
        view->SetHighColor(fBackgroundColor);
        view->FillRect(bottomRect);
    }
    
    // Update node watches for visible emails
    _UpdateVisibleWatches();
}


void
EmailListView::_DrawRow(BView* view, EmailItem* item, BRect rowRect,
    bool isSelected, int32 rowIndex)
{
    // Safety check
    if (item == NULL)
        return;
    
    // Draw background
    if (isSelected) {
        view->SetHighColor(fSelectedColor);
    } else if (rowIndex % 2 != 0) {
        view->SetHighColor(fStripeColor);
    } else {
        view->SetHighColor(fBackgroundColor);
    }
    view->FillRect(rowRect);
    
    // Draw text
    if (isSelected) {
        view->SetHighColor(fSelectedTextColor);
    } else {
        view->SetHighColor(fTextColor);
    }
    
    // Get status to check if email is new/unread
    BString statusStr(item->Status());
    bool isNew = (statusStr == "New");
    
    // Set bold font for new/unread emails
    if (isNew) {
        BFont font;
        view->GetFont(&font);
        font.SetFace(B_BOLD_FACE);
        view->SetFont(&font);
    }
    
    // Calculate text position (vertically centered)
    font_height fontHeight;
    view->GetFontHeight(&fontHeight);
    float textY = rowRect.top + (fRowHeight - fontHeight.ascent - fontHeight.descent) / 2.0f
                  + fontHeight.ascent;
    
    float padding = 5.0f;
    float xPos = rowRect.left;
    
    // Get strings safely - copy to local BStrings
    BString fromStr(item->From());
    BString toStr(item->To());
    BString subjectStr(item->Subject());
    BString dateStr(item->DateString());
    BString accountStr(item->Account());
    
    // Draw columns in the order specified by the header
    if (fColumnHeader != NULL) {
        int32 columnCount = fColumnHeader->CountColumns();
        for (int32 i = 0; i < columnCount; i++) {
            EmailColumn colId = fColumnHeader->ColumnAt(i);
            float colWidth = fColumnHeader->ColumnWidthAt(i);
            
            // Skip hidden columns (ColumnWidthAt returns 0 for hidden)
            if (colWidth <= 0)
                continue;
            
            switch (colId) {
                case kColumnStatus:
                    // Status is centered
                    if (statusStr.Length() > 0) {
                        float strWidth = view->StringWidth(statusStr.String());
                        float strX = xPos + (colWidth - strWidth) / 2.0f;
                        view->MovePenTo(strX, textY);
                        view->DrawString(statusStr.String());
                    }
                    break;
                
                case kColumnStar:
                {
                    // Draw star icon if starred
                    if (item->IsStarred() && sStarIcon != NULL) {
                        float iconSize = sStarIcon->Bounds().Width() + 1;
                        float iconX = xPos + (colWidth - iconSize) / 2.0f;
                        float iconY = rowRect.top + (fRowHeight - iconSize) / 2.0f;
                        view->SetDrawingMode(B_OP_ALPHA);
                        view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
                        view->DrawBitmap(sStarIcon, BPoint(iconX, iconY));
                        view->SetDrawingMode(B_OP_COPY);
                    }
                    break;
                }
                
                case kColumnAttachment:
                {
                    // Draw paperclip icon if has attachment
                    if (item->HasAttachment()) {
                        // Use white icon on dark backgrounds
                        bool isDark = fBackgroundColor.red < 128;
                        BBitmap* icon = (isDark && sAttachmentWhiteIcon != NULL)
                            ? sAttachmentWhiteIcon : sAttachmentIcon;
                        if (icon != NULL) {
                            float iconSize = icon->Bounds().Width() + 1;
                            float iconX = xPos + (colWidth - iconSize) / 2.0f;
                            float iconY = rowRect.top + (fRowHeight - iconSize) / 2.0f;
                            view->SetDrawingMode(B_OP_ALPHA);
                            view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
                            view->DrawBitmap(icon, BPoint(iconX, iconY));
                            view->SetDrawingMode(B_OP_COPY);
                        }
                    }
                    break;
                }
                    
                case kColumnFrom:
                    {
                        BString str(fromStr);
                        float avail = colWidth - padding * 2;
                        view->TruncateString(&str, B_TRUNCATE_END, avail);
                        view->MovePenTo(xPos + padding, textY);
                        view->DrawString(str.String());
                    }
                    break;
                    
                case kColumnTo:
                    {
                        BString str(toStr);
                        float avail = colWidth - padding * 2;
                        view->TruncateString(&str, B_TRUNCATE_END, avail);
                        view->MovePenTo(xPos + padding, textY);
                        view->DrawString(str.String());
                    }
                    break;
                    
                case kColumnSubject:
                    {
                        BString str(subjectStr);
                        float avail = colWidth - padding * 2;
                        view->TruncateString(&str, B_TRUNCATE_END, avail);
                        view->MovePenTo(xPos + padding, textY);
                        view->DrawString(str.String());
                    }
                    break;
                    
                case kColumnDate:
                    {
                        BString str(dateStr);
                        float avail = colWidth - padding * 2;
                        view->TruncateString(&str, B_TRUNCATE_END, avail);
                        view->MovePenTo(xPos + padding, textY);
                        view->DrawString(str.String());
                    }
                    break;
                    
                case kColumnAccount:
                    {
                        BString str(accountStr);
                        float avail = colWidth - padding * 2;
                        view->TruncateString(&str, B_TRUNCATE_END, avail);
                        view->MovePenTo(xPos + padding, textY);
                        view->DrawString(str.String());
                    }
                    break;
                    
                default:
                    break;
            }
            
            xPos += colWidth;
        }
    } else {
        // Fallback to default order and proportions
        float width = rowRect.Width();
        float statusWidth = width * 0.05f;
        float fromWidth = width * 0.15f;
        float toWidth = width * 0.15f;
        float subjectWidth = width * 0.35f;
        float dateWidth = width * 0.15f;
        float accountWidth = width * 0.15f;
        
        // Draw Status (centered in column)
        if (statusStr.Length() > 0) {
            float statusStrWidth = view->StringWidth(statusStr.String());
            float statusX = xPos + (statusWidth - statusStrWidth) / 2.0f;
            view->MovePenTo(statusX, textY);
            view->DrawString(statusStr.String());
        }
        xPos += statusWidth;
        
        // Draw From
        view->TruncateString(&fromStr, B_TRUNCATE_END, fromWidth - padding * 2);
        view->MovePenTo(xPos + padding, textY);
        view->DrawString(fromStr.String());
        xPos += fromWidth;
        
        // Draw To
        view->TruncateString(&toStr, B_TRUNCATE_END, toWidth - padding * 2);
        view->MovePenTo(xPos + padding, textY);
        view->DrawString(toStr.String());
        xPos += toWidth;
        
        // Draw Subject
        view->TruncateString(&subjectStr, B_TRUNCATE_END, subjectWidth - padding * 2);
        view->MovePenTo(xPos + padding, textY);
        view->DrawString(subjectStr.String());
        xPos += subjectWidth;
        
        // Draw Date
        view->TruncateString(&dateStr, B_TRUNCATE_END, dateWidth - padding * 2);
        view->MovePenTo(xPos + padding, textY);
        view->DrawString(dateStr.String());
        xPos += dateWidth;
        
        // Draw Account
        view->TruncateString(&accountStr, B_TRUNCATE_END, accountWidth - padding * 2);
        view->MovePenTo(xPos + padding, textY);
        view->DrawString(accountStr.String());
    }
    
    // Restore regular font if we changed it
    if (isNew) {
        BFont font;
        view->GetFont(&font);
        font.SetFace(B_REGULAR_FACE);
        view->SetFont(&font);
    }
    
    // Draw separator line at bottom
    // Use a tint that provides contrast on both light and dark themes
    rgb_color lineColor;
    if (fBackgroundColor.Brightness() > 127) {
        // Light background - darken
        lineColor = tint_color(fBackgroundColor, B_DARKEN_1_TINT);
    } else {
        // Dark background - lighten
        lineColor = tint_color(fBackgroundColor, B_LIGHTEN_1_TINT);
    }
    view->SetHighColor(lineColor);
    view->StrokeLine(BPoint(rowRect.left, rowRect.bottom),
               BPoint(rowRect.right, rowRect.bottom));
}


BRect
EmailListView::_RowRect(int32 index) const
{
    // Return the rect in the view's scrolling coordinate system
    // Row 0 is at y=0, Row 1 is at y=fRowHeight, etc.
    BRect rect;
    if (fContentView != NULL)
        rect = fContentView->Bounds();
    else
        rect = BRect(0, 0, 100, 100);
    rect.top = index * fRowHeight;
    rect.bottom = rect.top + fRowHeight - 1;
    return rect;
}


int32
EmailListView::_IndexAt(BPoint point) const
{
    // point is in view coordinates (which scroll with the view)
    if (point.y < 0)
        return -1;
    
    int32 index = (int32)(point.y / fRowHeight);
    
    if (index >= fItems.CountItems())
        return -1;
    
    return index;
}


int32
EmailListView::_FirstVisibleIndex() const
{
    if (fContentView == NULL)
        return 0;
    // Bounds().top is the scroll position in the view's coordinate system
    return (int32)(fContentView->Bounds().top / fRowHeight);
}


int32
EmailListView::_LastVisibleIndex() const
{
    if (fContentView == NULL)
        return -1;
    BRect bounds = fContentView->Bounds();
    int32 lastVisible = (int32)(bounds.bottom / fRowHeight);
    return std::min(lastVisible, fItems.CountItems() - 1);
}


int32
EmailListView::_VisibleCount() const
{
    return _LastVisibleIndex() - _FirstVisibleIndex() + 1;
}


void
EmailListView::ScrollTo(BPoint where)
{
    // Scroll the content view to the specified position
    if (fContentView != NULL) {
        fContentView->ScrollTo(where);
    }
}


void
EmailListView::_UpdateStripeColor()
{
    // Compute stripe color: a subtle tint of the background.
    // Dark themes (low brightness) get a slightly lighter stripe;
    // light themes get a slightly darker stripe.
    int32 brightness = (fBackgroundColor.red + fBackgroundColor.green
                        + fBackgroundColor.blue) / 3;
    int32 shift = (brightness > 127) ? -12 : 12;
    
    fStripeColor.red   = (uint8)std::max(0, std::min(255, (int)fBackgroundColor.red + (int)shift));
    fStripeColor.green = (uint8)std::max(0, std::min(255, (int)fBackgroundColor.green + (int)shift));
    fStripeColor.blue  = (uint8)std::max(0, std::min(255, (int)fBackgroundColor.blue + (int)shift));
    fStripeColor.alpha = 255;
}


void
EmailListView::_UpdateScrollBar()
{
    if (fContentView == NULL)
        return;
    
    // Vertical scrollbar
    if (fVScrollBar != NULL) {
        float viewHeight = fContentView->Bounds().Height();
        float dataHeight = fItems.CountItems() * fRowHeight;
        
        if (dataHeight <= viewHeight) {
            // Everything fits, no scrolling needed
            fVScrollBar->SetRange(0, 0);
            fVScrollBar->SetValue(0);
        } else {
            float maxScroll = dataHeight - viewHeight;
            fVScrollBar->SetRange(0, maxScroll);
            fVScrollBar->SetProportion(viewHeight / dataHeight);
            fVScrollBar->SetSteps(fRowHeight, viewHeight - fRowHeight);
        }
    }
    
    // Horizontal scrollbar
    if (fHScrollBar != NULL && fColumnHeader != NULL) {
        float viewWidth = fContentView->Bounds().Width();
        float dataWidth = fColumnHeader->TotalWidth();
        
        if (dataWidth <= viewWidth) {
            // Everything fits, no scrolling needed
            fHScrollBar->SetRange(0, 0);
            fHScrollBar->SetValue(0);
        } else {
            float maxScroll = dataWidth - viewWidth;
            fHScrollBar->SetRange(0, maxScroll);
            fHScrollBar->SetProportion(viewWidth / dataWidth);
            fHScrollBar->SetSteps(20, viewWidth - 20);
        }
    }
}


void
EmailListView::_HandleMouseDown(BView* view, BPoint where)
{
    // Check which mouse button was pressed
    BMessage* message = Window()->CurrentMessage();
    int32 buttons = 0;
    message->FindInt32("buttons", &buttons);
    int32 clicks = 1;
    message->FindInt32("clicks", &clicks);
    
    int32 index = _IndexAt(where);
    
    if (buttons & B_SECONDARY_MOUSE_BUTTON) {
        // Right-click - show context menu
        if (index >= 0) {
            // If clicking on unselected row, select it first
            if (!IsSelected(index)) {
                Select(index);
            }
        }
        
        if (CountSelected() > 0) {
            _ShowContextMenu(where);
        }
    } else if (index >= 0) {
        // Left-click - handle selection
        // On Haiku: B_COMMAND_KEY = Alt, B_OPTION_KEY = Win/Super
        int32 mods = ::modifiers();
        bool extend = (mods & B_SHIFT_KEY) != 0;
        bool toggle = (mods & B_COMMAND_KEY) != 0;  // Alt for non-contiguous
        
        Select(index, extend, toggle);
        
        // Double-click - invoke
        if (clicks == 2) {
            _NotifyInvocation();
        }
    }
}


void
EmailListView::_HandleKeyDown(const char* bytes, int32 numBytes)
{
    if (numBytes == 1) {
        int32 mods = ::modifiers();
        // DEBUG: log all keystrokes to stderr
        fprintf(stderr, "[EmailListView] KeyDown: bytes[0]=0x%02x ('%c') numBytes=%d mods=0x%08x B_COMMAND_KEY=0x%08x\n",
            (unsigned char)bytes[0], bytes[0], (int)numBytes, (int)mods, (int)B_COMMAND_KEY);
        bool extend = (mods & B_SHIFT_KEY) != 0;
        int32 current = fLastClickIndex >= 0 ? fLastClickIndex : FirstSelected();
        
        switch (bytes[0]) {
            case B_UP_ARROW:
                if (current > 0) {
                    int32 newIndex = current - 1;
                    if (extend) {
                        // Extend selection to new position (preserves existing)
                        ExtendSelectionTo(newIndex);
                    } else {
                        Select(newIndex);
                    }
                    EnsureVisible(newIndex);
                }
                break;
            
            case B_DOWN_ARROW:
                if (current < fItems.CountItems() - 1) {
                    int32 newIndex = current + 1;
                    if (extend) {
                        // Extend selection to new position (preserves existing)
                        ExtendSelectionTo(newIndex);
                    } else {
                        Select(newIndex);
                    }
                    EnsureVisible(newIndex);
                }
                break;
            
            case B_PAGE_UP:
            {
                int32 pageSize = _VisibleCount();
                int32 newIndex = std::max((int32)0, current - pageSize);
                if (extend) {
                    ExtendSelectionTo(newIndex);
                } else {
                    Select(newIndex);
                }
                EnsureVisible(newIndex);
                break;
            }
            
            case B_PAGE_DOWN:
            {
                int32 pageSize = _VisibleCount();
                int32 maxIndex = fItems.CountItems() - 1;
                int32 newIndex = std::min(maxIndex, current + pageSize);
                if (extend) {
                    ExtendSelectionTo(newIndex);
                } else {
                    Select(newIndex);
                }
                EnsureVisible(newIndex);
                break;
            }
            
            case B_HOME:
                if (extend) {
                    ExtendSelectionTo(0);
                } else {
                    Select(0);
                }
                EnsureVisible(0);
                break;
            
            case B_END:
            {
                int32 lastIndex = fItems.CountItems() - 1;
                if (lastIndex >= 0) {
                    if (extend) {
                        ExtendSelectionTo(lastIndex);
                    } else {
                        Select(lastIndex);
                    }
                    EnsureVisible(lastIndex);
                }
                break;
            }
            
            case B_DELETE:
                if (CountSelected() > 0) {
                    _MoveSelectedToTrash();
                }
                break;
            
            case 'z':
            case 'Z':
                // Alt+Z — forward undo delete to the parent window
                // Using raw constant 'undl' = MSG_UNDO_DELETE (defined in EmailViews.h)
                // to avoid circular include dependency.
                if (mods & B_COMMAND_KEY) {
                    BMessenger(Window()).SendMessage(new BMessage('undl'));
                }
                break;
            
            case B_RETURN:
                if (CountSelected() > 0) {
                    _NotifyInvocation();
                }
                break;
            
            default:
                break;
        }
    }
}


// Note: Old MakeFocus moved to constructor section
// =============================================================================
// Data management
// =============================================================================

void
EmailListView::AddEmail(EmailRef* ref)
{
    // Simple append (unsorted)
    EmailItem* item = new EmailItem(ref);
    fItems.AddItem(item);
    
    _UpdateScrollBar();
    
    // Only invalidate if item is visible
    int32 index = fItems.CountItems() - 1;
    if (index >= _FirstVisibleIndex() && index <= _LastVisibleIndex()) {
        if (fContentView != NULL) fContentView->Invalidate(_RowRect(index));
    }
}


void
EmailListView::AddEmailSorted(EmailRef* ref)
{
    // Binary search for insertion point (sorted by current sort column)
    int32 insertIndex = _FindInsertionPoint(ref);
    
    EmailItem* item = new EmailItem(ref);
    fItems.AddItem(item, insertIndex);
    
    // Add the new entry to HashMap for dedup and node monitor lookups.
    // During loading (fLoaderThread >= 0), we do NOT update indices for
    // shifted items — that would be O(n) per insertion, making loading O(n²).
    // The HashMap is fully rebuilt at loading completion.
    // For live query additions (single items after loading), we update shifted
    // entries to keep the HashMap accurate.
    fNodeToIndex[ref->nodeRef] = insertIndex;
    
    if (fLoaderThread < 0) {
        // Post-loading: update shifted items to keep HashMap accurate
        for (int32 i = insertIndex + 1; i < fItems.CountItems(); i++) {
            EmailItem* shifted = fItems.ItemAt(i);
            if (shifted != NULL && shifted->Ref() != NULL)
                fNodeToIndex[shifted->Ref()->nodeRef] = i;
        }
    }
    
    // Adjust selection indices if inserting before selected items
    std::set<int32> adjustedSelection;
    for (int32 idx : fSelectedIndices) {
        if (idx >= insertIndex)
            adjustedSelection.insert(idx + 1);
        else
            adjustedSelection.insert(idx);
    }
    fSelectedIndices = adjustedSelection;
    
    // Adjust anchor and last click indices
    if (fAnchorIndex >= insertIndex)
        fAnchorIndex++;
    if (fLastClickIndex >= insertIndex)
        fLastClickIndex++;
    
    _UpdateScrollBar();
    
    // Drawing is NOT done here — the caller (_ProcessLoaderBatch or
    // live query handler) invalidates once after processing a batch,
    // avoiding per-item flicker during loading.
}


int32
EmailListView::_FindInsertionPoint(EmailRef* ref)
{
    // Binary search for correct sorted position using current sort settings.
    // This ensures items appear in the user's chosen order during loading.
    
    int32 low = 0;
    int32 high = fItems.CountItems();
    
    while (low < high) {
        int32 mid = (low + high) / 2;
        EmailItem* midItem = fItems.ItemAt(mid);
        
        if (midItem == NULL || midItem->Ref() == NULL) {
            high = mid;
            continue;
        }
        
        int result = _CompareForInsertion(ref, midItem->Ref());
        
        if (result < 0) {
            high = mid;  // New item comes before mid
        } else {
            low = mid + 1;  // New item comes after mid
        }
    }
    
    return low;
}


int
EmailListView::_CompareForInsertion(const EmailRef* a, const EmailRef* b) const
{
    int result = 0;
    
    switch (fSortColumn) {
        case kSortByStatus:
            result = a->status.Compare(b->status);
            break;
        case kSortByStar:
            result = (int)b->isStarred - (int)a->isStarred;
            break;
        case kSortByAttachment:
            result = (int)b->hasAttachment - (int)a->hasAttachment;
            break;
        case kSortByFrom:
            result = a->from.ICompare(b->from);
            break;
        case kSortByTo:
            result = a->to.ICompare(b->to);
            break;
        case kSortBySubject:
            result = a->subject.ICompare(b->subject);
            break;
        case kSortByAccount:
            result = a->account.ICompare(b->account);
            break;
        case kSortByDate:
        default:
            if (a->when < b->when)
                result = -1;
            else if (a->when > b->when)
                result = 1;
            else {
                // Tiebreaker: inode number (sequential on BFS, higher = newer file).
                // MAIL:when is second-precision, so two emails sent/received within
                // the same second (e.g. sending to yourself) have identical timestamps.
                // Inode numbers are assigned sequentially, making them a reliable
                // proxy for creation order within the same volume.
                if (a->nodeRef.node < b->nodeRef.node)
                    result = -1;
                else if (a->nodeRef.node > b->nodeRef.node)
                    result = 1;
            }
            break;
    }
    
    // Reverse for descending order
    if (fSortOrder == kSortDescending)
        result = -result;
    
    return result;
}


void
EmailListView::RemoveEmail(const node_ref& nodeRef)
{
    // O(1) lookup via HashMap.
    // Safe to call for items already removed (e.g. delete handler removes
    // the row immediately, then B_ENTRY_REMOVED from the live query tries
    // again — the HashMap lookup simply returns "not found").
    auto it = fNodeToIndex.find(nodeRef);
    if (it == fNodeToIndex.end())
        return;
    
    int32 index = it->second;
    
    // During loading the HashMap may have stale indices for shifted items.
    // Verify the index actually points to the right item; if not, do a
    // linear scan to find the correct position.
    if (index < 0 || index >= fItems.CountItems()) {
        _FindItemByNodeRef(nodeRef, &index);
        if (index < 0)
            return;
    } else {
        EmailItem* item = fItems.ItemAt(index);
        if (item == NULL || item->Ref() == NULL
            || item->Ref()->nodeRef.device != nodeRef.device
            || item->Ref()->nodeRef.node != nodeRef.node) {
            _FindItemByNodeRef(nodeRef, &index);
            if (index < 0)
                return;
        }
    }
    
    // Stop watching this node if we were watching it
    auto watchIt = fWatchedNodes.find(nodeRef);
    if (watchIt != fWatchedNodes.end()) {
        watch_node(&nodeRef, B_STOP_WATCHING, this);
        fWatchedNodes.erase(watchIt);
    }
    
    // Remove from HashMap
    fNodeToIndex.erase(it);
    
    // Update indices for items after the removed one
    for (int32 i = index + 1; i < fItems.CountItems(); i++) {
        EmailItem* item = fItems.ItemAt(i);
        if (item != NULL && item->Ref() != NULL) {
            fNodeToIndex[item->Ref()->nodeRef] = i - 1;
        }
    }
    
    // Adjust selection - remove if selected, shift others
    bool wasSelected = fSelectedIndices.erase(index) > 0;
    
    std::set<int32> adjustedSelection;
    for (int32 idx : fSelectedIndices) {
        if (idx > index)
            adjustedSelection.insert(idx - 1);
        else
            adjustedSelection.insert(idx);
    }
    fSelectedIndices = adjustedSelection;
    
    // Adjust anchor and last click indices
    if (fAnchorIndex == index)
        fAnchorIndex = -1;
    else if (fAnchorIndex > index)
        fAnchorIndex--;
    
    if (fLastClickIndex == index)
        fLastClickIndex = -1;
    else if (fLastClickIndex > index)
        fLastClickIndex--;
    
    // Remove and delete item
    delete fItems.RemoveItemAt(index);
    
    _UpdateScrollBar();
    if (fContentView != NULL) fContentView->Invalidate();
    
    // Notify parent of selection change if removed item was selected
    if (wasSelected) {
        // If only one item was selected (now zero selected), auto-select
        // the item at the same position (or the last item if at the end)
        if (fSelectedIndices.empty() && fItems.CountItems() > 0) {
            int32 newIndex = index;
            if (newIndex >= fItems.CountItems())
                newIndex = fItems.CountItems() - 1;
            Select(newIndex);
            EnsureVisible(newIndex);
        }
        _NotifySelectionChanged();
    }
}


int32
EmailListView::CountItems() const
{
    return fItems.CountItems();
}


EmailItem*
EmailListView::ItemAt(int32 index) const
{
    return fItems.ItemAt(index);
}


int32
EmailListView::IndexOf(const node_ref& ref) const
{
    if (fLoaderThread >= 0) {
        // During loading the HashMap has stale indices for shifted items.
        // Use linear scan for correctness (only called for user-initiated
        // lookups like Next/Previous navigation, not hot paths).
        int32 outIndex;
        if (_FindItemByNodeRef(ref, &outIndex) != NULL)
            return outIndex;
        return -1;
    }
    auto it = fNodeToIndex.find(ref);
    if (it != fNodeToIndex.end())
        return it->second;
    return -1;
}


EmailItem*
EmailListView::ItemFor(const node_ref& ref) const
{
    int32 index = IndexOf(ref);
    if (index >= 0)
        return fItems.ItemAt(index);
    return NULL;
}


EmailItem*
EmailListView::_FindItemByNodeRef(const node_ref& ref, int32* outIndex) const
{
    // Linear scan to find item by node_ref, returning both the item and its
    // true index. This bypasses fNodeToIndex which may have stale indices
    // during Phase 2 loading (AddEmailSorted intentionally skips updating
    // shifted items for O(n) loading performance).
    for (int32 i = 0; i < fItems.CountItems(); i++) {
        EmailItem* item = fItems.ItemAt(i);
        if (item != NULL && item->Ref() != NULL) {
            const node_ref& itemRef = item->Ref()->nodeRef;
            if (itemRef.device == ref.device && itemRef.node == ref.node) {
                if (outIndex != NULL)
                    *outIndex = i;
                return item;
            }
        }
    }
    if (outIndex != NULL)
        *outIndex = -1;
    return NULL;
}


void
EmailListView::BeginBulkLoad()
{
    fBulkLoading = true;
}


void
EmailListView::EndBulkLoad()
{
    fBulkLoading = false;
    
    // Rebuild the HashMap in one pass - O(n)
    fNodeToIndex.clear();
    fNodeToIndex.reserve(fItems.CountItems());
    
    for (int32 i = 0; i < fItems.CountItems(); i++) {
        EmailItem* item = fItems.ItemAt(i);
        if (item != NULL && item->Ref() != NULL) {
            fNodeToIndex[item->Ref()->nodeRef] = i;
        }
    }
}


// =============================================================================
// Sorting
// =============================================================================

// Comparison context for BObjectList::SortItems
struct SortContext {
    EmailListView::SortColumn column;
    EmailListView::SortOrder order;
};

// Comparison function for BObjectList::SortItems
static int _CompareItemsForSort(const EmailItem* a, const EmailItem* b, void* context)
{
    SortContext* ctx = static_cast<SortContext*>(context);
    int result = 0;
    
    // Need to cast away const for accessor methods (they're not const-correct)
    EmailItem* itemA = const_cast<EmailItem*>(a);
    EmailItem* itemB = const_cast<EmailItem*>(b);
    
    switch (ctx->column) {
        case EmailListView::kSortByStatus:
            result = strcmp(itemA->Status(), itemB->Status());
            break;
        case EmailListView::kSortByStar:
            result = (int)itemB->IsStarred() - (int)itemA->IsStarred();
            break;
        case EmailListView::kSortByAttachment:
            result = (int)itemB->HasAttachment() - (int)itemA->HasAttachment();
            break;
        case EmailListView::kSortByFrom:
            result = strcasecmp(itemA->From(), itemB->From());
            break;
        case EmailListView::kSortByTo:
            result = strcasecmp(itemA->To(), itemB->To());
            break;
        case EmailListView::kSortBySubject:
            result = strcasecmp(itemA->Subject(), itemB->Subject());
            break;
        case EmailListView::kSortByAccount:
            result = strcasecmp(itemA->Account(), itemB->Account());
            break;
        case EmailListView::kSortByDate:
        default:
            // Sort by MAIL:when timestamp
            if (itemA->Ref() != NULL && itemB->Ref() != NULL) {
                if (itemA->Ref()->when < itemB->Ref()->when)
                    result = -1;
                else if (itemA->Ref()->when > itemB->Ref()->when)
                    result = 1;
                else
                    result = 0;
            }
            break;
    }
    
    // Reverse for descending order
    if (ctx->order == EmailListView::kSortDescending)
        result = -result;
    
    return result;
}


void
EmailListView::SetSortColumn(SortColumn column, SortOrder order)
{
    if (column == fSortColumn && order == fSortOrder)
        return;  // No change
    
    fSortColumn = column;
    fSortOrder = order;
    
    // Stop all watches before sorting
    _StopAllWatches();
    
    int32 count = fItems.CountItems();
    
    // Use BObjectList's SortItems method
    SortContext context = { fSortColumn, fSortOrder };
    fItems.SortItems(_CompareItemsForSort, &context);
    
    // Rebuild HashMap
    fNodeToIndex.clear();
    for (int32 i = 0; i < fItems.CountItems(); i++) {
        EmailItem* item = fItems.ItemAt(i);
        if (item != NULL && item->Ref() != NULL) {
            fNodeToIndex[item->Ref()->nodeRef] = i;
        }
    }
    
    // Clear selection and scroll to top to show the new ordering
    DeselectAll();
    fAnchorIndex = -1;
    fLastClickIndex = -1;
    
    ScrollTo(BPoint(0, 0));
    if (fContentView != NULL) fContentView->Invalidate();
}


// =============================================================================
// Selection
// =============================================================================

void
EmailListView::_NotifySelectionChanged()
{
    if (fSelectionMessage == NULL || fTarget == NULL)
        return;
    
    BMessage msg(*fSelectionMessage);
    msg.AddInt32("count", CountSelected());
    if (!fSelectedIndices.empty()) {
        int32 first = FirstSelected();
        msg.AddInt32("index", first);
        EmailItem* item = fItems.ItemAt(first);
        if (item != NULL && item->Ref() != NULL) {
            msg.AddRef("ref", &item->Ref()->entryRef);
        }
    }
    
    fTarget->PostMessage(&msg);
}


void
EmailListView::_NotifyInvocation()
{
    if (fInvocationMessage == NULL || fTarget == NULL)
        return;
    
    BMessage msg(*fInvocationMessage);
    for (int32 index : fSelectedIndices) {
        EmailItem* item = fItems.ItemAt(index);
        if (item != NULL && item->Ref() != NULL) {
            msg.AddRef("ref", &item->Ref()->entryRef);
        }
    }
    
    if (msg.HasRef("ref"))
        fTarget->PostMessage(&msg);
}


void
EmailListView::Select(int32 index, bool extend, bool toggle)
{
    if (index < 0 || index >= fItems.CountItems())
        return;
    
    if (toggle) {
        // Alt+click: toggle individual item
        if (fSelectedIndices.count(index) > 0) {
            // Deselect
            fSelectedIndices.erase(index);
            EmailItem* item = fItems.ItemAt(index);
            if (item != NULL) {
                item->SetSelected(false);
                if (fContentView != NULL) fContentView->Invalidate(_RowRect(index));
            }
        } else {
            // Select
            fSelectedIndices.insert(index);
            EmailItem* item = fItems.ItemAt(index);
            if (item != NULL) {
                item->SetSelected(true);
                if (fContentView != NULL) fContentView->Invalidate(_RowRect(index));
            }
        }
        fLastClickIndex = index;
        // Don't change anchor for toggle
    } else if (extend && fAnchorIndex >= 0) {
        // Shift+click: extend selection from anchor
        SelectRange(fAnchorIndex, index);
        fLastClickIndex = index;
    } else {
        // Normal click: single selection, clear others
        DeselectAll();
        fSelectedIndices.insert(index);
        EmailItem* item = fItems.ItemAt(index);
        if (item != NULL) {
            item->SetSelected(true);
            if (fContentView != NULL) fContentView->Invalidate(_RowRect(index));
        }
        fAnchorIndex = index;
        fLastClickIndex = index;
    }
    
    // Notify target window of selection change
    _NotifySelectionChanged();
}


void
EmailListView::SelectRange(int32 from, int32 to)
{
    if (from > to)
        std::swap(from, to);
    
    // Clamp to valid range
    from = std::max((int32)0, from);
    to = std::min(to, fItems.CountItems() - 1);
    
    // Clear existing selection
    DeselectAll();
    
    // Select range
    for (int32 i = from; i <= to; i++) {
        fSelectedIndices.insert(i);
        EmailItem* item = fItems.ItemAt(i);
        if (item != NULL) {
            item->SetSelected(true);
        }
    }
    
    // Invalidate affected area
    if (fContentView != NULL) fContentView->Invalidate(BRect(_RowRect(from).left, _RowRect(from).top,
                     _RowRect(to).right, _RowRect(to).bottom));
}


void
EmailListView::ExtendSelectionTo(int32 index)
{
    // Extend selection from current position to index.
    // If target is already selected, we're reversing - deselect rows between current and target.
    // If target is not selected, we're extending - select rows between current and target.
    if (index < 0 || index >= fItems.CountItems())
        return;
    
    int32 current = fLastClickIndex >= 0 ? fLastClickIndex : FirstSelected();
    if (current < 0) {
        // Nothing selected, just select the target
        Select(index);
        return;
    }
    
    if (index == current)
        return;
    
    bool targetSelected = fSelectedIndices.count(index) > 0;
    
    if (targetSelected) {
        // Reversing direction - deselect from current toward index (but not index itself)
        int32 step = (index < current) ? -1 : 1;
        for (int32 i = current; i != index; i += step) {
            if (fSelectedIndices.count(i) > 0) {
                fSelectedIndices.erase(i);
                EmailItem* item = fItems.ItemAt(i);
                if (item != NULL) {
                    item->SetSelected(false);
                    if (fContentView != NULL) fContentView->Invalidate(_RowRect(i));
                }
            }
        }
    } else {
        // Extending - select from current toward index (including index)
        int32 start = std::min(current, index);
        int32 end = std::max(current, index);
        for (int32 i = start; i <= end; i++) {
            if (fSelectedIndices.count(i) == 0) {
                fSelectedIndices.insert(i);
                EmailItem* item = fItems.ItemAt(i);
                if (item != NULL) {
                    item->SetSelected(true);
                    if (fContentView != NULL) fContentView->Invalidate(_RowRect(i));
                }
            }
        }
    }
    
    fLastClickIndex = index;
    
    // Notify target window of selection change
    _NotifySelectionChanged();
}


void
EmailListView::Deselect()
{
    DeselectAll();
}


void
EmailListView::DeselectAll()
{
    for (int32 index : fSelectedIndices) {
        EmailItem* item = fItems.ItemAt(index);
        if (item != NULL) {
            item->SetSelected(false);
            if (fContentView != NULL) fContentView->Invalidate(_RowRect(index));
        }
    }
    fSelectedIndices.clear();
}


bool
EmailListView::IsSelected(int32 index) const
{
    return fSelectedIndices.count(index) > 0;
}


int32
EmailListView::CountSelected() const
{
    return (int32)fSelectedIndices.size();
}


int32
EmailListView::FirstSelected() const
{
    if (fSelectedIndices.empty())
        return -1;
    return *fSelectedIndices.begin();
}


int32
EmailListView::LastSelected() const
{
    if (fSelectedIndices.empty())
        return -1;
    return *fSelectedIndices.rbegin();
}


void
EmailListView::GetSelectedIndices(BList* indices) const
{
    if (indices == NULL)
        return;
    for (int32 index : fSelectedIndices) {
        indices->AddItem((void*)(intptr_t)index);
    }
}


EmailItem*
EmailListView::SelectedItem() const
{
    int32 first = FirstSelected();
    if (first >= 0 && first < fItems.CountItems()) {
        return fItems.ItemAt(first);
    }
    return NULL;
}


void
EmailListView::ScrollToSelection()
{
    int32 first = FirstSelected();
    if (first >= 0) {
        EnsureVisible(first);
    }
}


void
EmailListView::EnsureVisible(int32 index)
{
    if (index < 0 || index >= fItems.CountItems())
        return;
    
    if (fContentView == NULL)
        return;
    
    float rowTop = index * fRowHeight;
    float rowBottom = rowTop + fRowHeight;
    BRect bounds = fContentView->Bounds();
    
    if (rowTop < bounds.top) {
        // Row is above visible area - scroll up
        ScrollTo(BPoint(0, rowTop));
    } else if (rowBottom > bounds.bottom) {
        // Row is below visible area - scroll down
        ScrollTo(BPoint(0, rowBottom - bounds.Height()));
    }
}


void
EmailListView::_UpdateVisibleWatches()
{
    if (Window() == NULL)
        return;
    
    // Only monitor visible rows for attribute changes (B_WATCH_ATTR).
    // The system-wide node monitor limit is 4096; with lists of 20,000+
    // emails we'd exhaust it instantly if we watched everything. Watching
    // only visible rows (~30-50) keeps us well under the limit while still
    // showing real-time status changes (read/unread, star, etc.).
    int32 firstVisible = _FirstVisibleIndex();
    int32 lastVisible = _LastVisibleIndex();
    int32 count = fItems.CountItems();
    
    // Clamp to valid range
    firstVisible = std::max((int32)0, firstVisible);
    lastVisible = std::min(count - 1, lastVisible);
    
    // Build set of node_refs that should be watched
    std::unordered_set<node_ref, NodeRefHash, NodeRefEqual> shouldWatch;
    
    for (int32 i = firstVisible; i <= lastVisible; i++) {
        EmailItem* item = fItems.ItemAt(i);
        if (item != NULL && item->Ref() != NULL) {
            shouldWatch.insert(item->Ref()->nodeRef);
        }
    }
    
    // Stop watching nodes that are no longer visible
    for (auto it = fWatchedNodes.begin(); it != fWatchedNodes.end(); ) {
        if (shouldWatch.find(*it) == shouldWatch.end()) {
            // No longer visible - stop watching
            watch_node(&(*it), B_STOP_WATCHING, this);
            
            // Invalidate cache so it reloads when visible again
            EmailItem* item = ItemFor(*it);
            if (item != NULL) {
                item->InvalidateCache();
            }
            
            it = fWatchedNodes.erase(it);
        } else {
            ++it;
        }
    }
    
    // Start watching newly visible nodes
    for (const auto& nodeRef : shouldWatch) {
        if (fWatchedNodes.find(nodeRef) == fWatchedNodes.end()) {
            // Not yet watched - start watching for attribute changes
            status_t status = watch_node(&nodeRef, B_WATCH_ATTR, this);
            if (status == B_OK) {
                fWatchedNodes.insert(nodeRef);
                
                // Invalidate cache since attributes may have changed while not watched
                EmailItem* item = ItemFor(nodeRef);
                if (item != NULL) {
                    item->InvalidateCache();
                }
            }
        }
    }
}


void
EmailListView::_StopAllWatches()
{
    for (const auto& nodeRef : fWatchedNodes) {
        watch_node(&nodeRef, B_STOP_WATCHING, this);
    }
    fWatchedNodes.clear();
}


void
EmailListView::_ShowContextMenu(BPoint where)
{
    BPopUpMenu* menu = new BPopUpMenu("EmailContextMenu", false, false);
    
    if (fShowingTrash) {
        // Trash view: only Restore and Delete permanently
        // These forward to the parent window (EmailViewsWindow)
        // MSG_RESTORE_EMAIL = 'rste', MSG_DELETE_EMAIL = 'edel' (defined in EmailViews.h)
        menu->AddItem(new BMenuItem(B_TRANSLATE("Restore"), 
            new BMessage('rste')));
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem(B_TRANSLATE("Delete permanently"), 
            new BMessage('edel')));
        
        // Convert to screen coordinates
        if (fContentView != NULL)
            fContentView->ConvertToScreen(&where);
        else
            ConvertToScreen(&where);
        
        // Target the parent window for all items
        menu->SetTargetForItems(fTarget);
        menu->Go(where, true, true, true);
        return;
    }
    
    if (fShowingSpam) {
        // Spam view: Not spam, mark read/unread, and Move to Trash
        menu->AddItem(new BMenuItem(B_TRANSLATE("Not spam"),
            new BMessage('uspm')));  // MSG_UNMARK_SPAM
        menu->AddSeparatorItem();

        // Check selection status for mark read/unread
        bool hasNew = false;
        bool hasRead = false;
        for (int32 index : fSelectedIndices) {
            EmailItem* item = fItems.ItemAt(index);
            if (item == NULL)
                continue;
            const char* status = item->Status();
            if (status != NULL) {
                if (strcmp(status, "New") == 0)
                    hasNew = true;
                else if (strcmp(status, "Read") == 0)
                    hasRead = true;
            }
        }

        BMenuItem* markReadItem = new BMenuItem(B_TRANSLATE("Mark as read"),
            new BMessage('mred'));  // MSG_MARK_READ
        markReadItem->SetEnabled(hasNew);
        menu->AddItem(markReadItem);

        BMenuItem* markUnreadItem = new BMenuItem(B_TRANSLATE("Mark as unread"),
            new BMessage('murd'));  // MSG_MARK_UNREAD
        markUnreadItem->SetEnabled(hasRead);
        menu->AddItem(markUnreadItem);

        menu->AddItem(new BMenuItem(B_TRANSLATE("Move to Trash"),
            new BMessage('edel')));  // MSG_DELETE_EMAIL
        
        // Convert to screen coordinates
        if (fContentView != NULL)
            fContentView->ConvertToScreen(&where);
        else
            ConvertToScreen(&where);
        
        menu->SetTargetForItems(fTarget);
        menu->Go(where, true, true, true);
        return;
    }
    
    // Normal view: full context menu.
    // Message constants use raw 'xxxx' values instead of MSG_* symbols to
    // avoid including EmailViews.h, which would create a circular dependency.
    // The values must match those defined in EmailViews.h.
    
    // --- Reply / Forward section ---
    menu->AddItem(new BMenuItem(B_TRANSLATE("Reply"),
        new BMessage('rply')));  // MSG_REPLY
    menu->AddItem(new BMenuItem(B_TRANSLATE("Reply all"),
        new BMessage('rpla')));  // MSG_REPLY_ALL
    menu->AddItem(new BMenuItem(B_TRANSLATE("Forward"),
        new BMessage('frwd')));  // MSG_FORWARD
    menu->AddItem(new BMenuItem(B_TRANSLATE("Send as attachment"),
        new BMessage('saat')));  // MSG_SEND_AS_ATTACHMENT
    
    menu->AddSeparatorItem();
    
    // --- Star / Mark / Trash section ---
    
    // Star toggle (conditional based on selection)
    bool hasStarred = false;
    bool hasUnstarred = false;
    for (int32 index : fSelectedIndices) {
        EmailItem* selItem = fItems.ItemAt(index);
        if (selItem == NULL)
            continue;
        if (selItem->IsStarred())
            hasStarred = true;
        else
            hasUnstarred = true;
    }
    
    if (hasUnstarred) {
        BMessage* starMsg = new BMessage('star');  // MSG_STAR_EMAIL
        starMsg->AddBool("starred", true);
        menu->AddItem(new BMenuItem(B_TRANSLATE("Star"), starMsg));
    }
    if (hasStarred) {
        BMessage* unstarMsg = new BMessage('star');  // MSG_STAR_EMAIL
        unstarMsg->AddBool("starred", false);
        menu->AddItem(new BMenuItem(B_TRANSLATE("Unstar"), unstarMsg));
    }
    
    // Mark status items - enabled based on selection
    bool hasNew = false;
    bool hasRead = false;
    for (int32 index : fSelectedIndices) {
        EmailItem* item = fItems.ItemAt(index);
        if (item == NULL)
            continue;
        const char* status = item->Status();
        if (status != NULL) {
            if (strcmp(status, "New") == 0)
                hasNew = true;
            else if (strcmp(status, "Read") == 0)
                hasRead = true;
        }
    }
    
    BMenuItem* markReadItem = new BMenuItem(B_TRANSLATE("Mark as read"),
        new BMessage('mred'));  // MSG_MARK_READ
    markReadItem->SetEnabled(hasNew);
    menu->AddItem(markReadItem);
    
    BMenuItem* markUnreadItem = new BMenuItem(B_TRANSLATE("Mark as unread"),
        new BMessage('murd'));  // MSG_MARK_UNREAD
    markUnreadItem->SetEnabled(hasRead);
    menu->AddItem(markUnreadItem);
    
    menu->AddItem(new BMenuItem(B_TRANSLATE("Move to Trash"),
        new BMessage('edel')));  // MSG_DELETE_EMAIL
    menu->AddItem(new BMenuItem(B_TRANSLATE("Mark as spam"),
        new BMessage('mspm')));  // MSG_MARK_SPAM
    
    menu->AddSeparatorItem();
    
    // --- Filter section ---
    menu->AddItem(new BMenuItem(B_TRANSLATE("Filter by 'Sender'"),
        new BMessage('fxsn')));  // MSG_FILTER_EXACT_SENDER
    menu->AddItem(new BMenuItem(B_TRANSLATE("Filter by 'Recipient'"),
        new BMessage('fxrp')));  // MSG_FILTER_EXACT_RECIPIENT
    menu->AddItem(new BMenuItem(B_TRANSLATE("Filter by 'Subject'"),
        new BMessage('fxsb')));  // MSG_FILTER_EXACT_SUBJECT
    menu->AddItem(new BMenuItem(B_TRANSLATE("Filter by 'Account'"),
        new BMessage('fxac')));  // MSG_FILTER_EXACT_ACCOUNT
    
    menu->AddSeparatorItem();
    
    // --- People / Email section ---
    menu->AddItem(new BMenuItem(B_TRANSLATE("New email to sender"),
        new BMessage('emsd')));  // MSG_EMAIL_SENDER
    
    // "Create Person file for" submenu - extract sender addresses from selection
    BMenu* personSubMenu = new BMenu(B_TRANSLATE("Create Person file for"));
    std::set<BString> addedAddresses;  // avoid duplicates
    
    for (int32 index : fSelectedIndices) {
        EmailItem* selItem = fItems.ItemAt(index);
        if (selItem == NULL)
            continue;
        
        BFile file(selItem->GetPath(), B_READ_ONLY);
        if (file.InitCheck() != B_OK)
            continue;
        
        char fromBuffer[256] = "";
        ssize_t size = file.ReadAttr("MAIL:from", B_STRING_TYPE, 0,
            fromBuffer, sizeof(fromBuffer) - 1);
        if (size <= 0)
            continue;
        fromBuffer[size] = '\0';
        
        // Extract email address and display name
        BString fullFrom(fromBuffer);
        BString emailAddr(fromBuffer);
        BString displayName;
        
        int32 angleStart = emailAddr.FindFirst("<");
        int32 angleEnd = emailAddr.FindFirst(">");
        if (angleStart >= 0 && angleEnd > angleStart) {
            // Has "Name <address>" format
            displayName.SetTo(fromBuffer, angleStart);
            displayName.Trim();
            // Remove surrounding quotes if present
            if (displayName.Length() >= 2
                && displayName[0] == '"'
                && displayName[displayName.Length() - 1] == '"') {
                displayName.Remove(0, 1);
                displayName.Truncate(displayName.Length() - 1);
            }
            emailAddr.Remove(0, angleStart + 1);
            emailAddr.Truncate(angleEnd - angleStart - 1);
        }
        
        // Skip if we already added this address
        if (addedAddresses.count(emailAddr) > 0)
            continue;
        addedAddresses.insert(emailAddr);
        
        // Build label: "Name <address>" or just "address"
        BString label;
        if (displayName.Length() > 0)
            label.SetToFormat("%s <%s>", displayName.String(), emailAddr.String());
        else
            label = emailAddr;
        
        BMessage* personMsg = new BMessage('crps');  // MSG_CREATE_PERSON
        personMsg->AddString("address", emailAddr.String());
        personMsg->AddString("name", displayName.String());
        personSubMenu->AddItem(new BMenuItem(label.String(), personMsg));
    }
    
    menu->AddItem(personSubMenu);
    
    menu->AddSeparatorItem();
    
    // --- Query section ---
    menu->AddItem(new BMenuItem(B_TRANSLATE("Add 'From' query"),
        new BMessage('fbsn')));  // MSG_FILTER_BY_SENDER
    menu->AddItem(new BMenuItem(B_TRANSLATE("Add 'To' query"),
        new BMessage('fbrp')));  // MSG_FILTER_BY_RECIPIENT
    menu->AddItem(new BMenuItem(B_TRANSLATE("Add 'Account' query"),
        new BMessage('fbac')));  // MSG_FILTER_BY_ACCOUNT
    
    // Convert to screen coordinates (where is in ContentView coords)
    if (fContentView != NULL)
        fContentView->ConvertToScreen(&where);
    else
        ConvertToScreen(&where);
    
    // Target the parent window for all items
    menu->SetTargetForItems(fTarget);
    // Also target submenu items
    personSubMenu->SetTargetForItems(fTarget);
    menu->Go(where, true, true, true);
}


void
EmailListView::_MarkSelectedAs(const char* status)
{
    if (status == NULL)
        return;
    
    // Determine which current status is required for this operation
    const char* requiredStatus = NULL;
    if (strcmp(status, "Read") == 0)
        requiredStatus = "New";      // Mark as read only applies to New emails
    else if (strcmp(status, "New") == 0)
        requiredStatus = "Read";     // Mark as unread only applies to Read emails
    // Mark as sent has no restriction (requiredStatus stays NULL)
    
    int32 count = 0;
    
    // Iterate through all selected items
    for (int32 index : fSelectedIndices) {
        EmailItem* item = fItems.ItemAt(index);
        if (item == NULL || item->Ref() == NULL)
            continue;
        
        // Check if this email has the required status
        if (requiredStatus != NULL) {
            const char* currentStatus = item->Status();
            if (currentStatus == NULL || strcmp(currentStatus, requiredStatus) != 0)
                continue;  // Skip this email
        }
        
        // Open the node and write the status attribute
        BNode node(&item->Ref()->entryRef);
        if (node.InitCheck() != B_OK)
            continue;
        
        ssize_t written = node.WriteAttr("MAIL:status", B_STRING_TYPE, 0,
                                          status, strlen(status) + 1);
        if (written > 0) {
            count++;
            
            // Invalidate the item's cache so it reloads the status
            item->InvalidateCache();
            
            // Redraw the row
            if (fContentView != NULL) fContentView->Invalidate(_RowRect(index));
        }
    }
}


void
EmailListView::_MoveSelectedToTrash()
{
    // Delegate to parent window which handles all delete logic
    // (move to trash, permanent delete in trash view, count updates, etc.)
    if (fTarget)
        BMessenger(fTarget).SendMessage('edel');  // MSG_DELETE_EMAIL
}


// =============================================================================
// EmailViews Compatibility API
// =============================================================================

void
EmailListView::AddItem(EmailItem* item)
{
    if (item == NULL)
        return;
    
    int32 index = fItems.CountItems();
    fItems.AddItem(item);
    
    // Always update HashMap
    if (item->Ref() != NULL) {
        fNodeToIndex[item->Ref()->nodeRef] = index;
    }
    
    _UpdateScrollBar();
    if (fContentView != NULL)
        fContentView->Invalidate();
}


void
EmailListView::RemoveRow(EmailItem* item)
{
    if (item == NULL)
        return;
    
    // Use HashMap for O(1) lookup, with fallback to linear scan
    int32 index = -1;
    if (item->Ref() != NULL) {
        auto it = fNodeToIndex.find(item->Ref()->nodeRef);
        if (it != fNodeToIndex.end()) {
            index = it->second;
            // Verify the index is correct (can be stale during bulk loading)
            if (index < 0 || index >= fItems.CountItems()
                || fItems.ItemAt(index) != item) {
                index = fItems.IndexOf(item);
            }
        }
    }
    if (index < 0)
        index = fItems.IndexOf(item);
    if (index < 0)
        return;
    
    // Remove from HashMap
    if (item->Ref() != NULL) {
        fNodeToIndex.erase(item->Ref()->nodeRef);
    }
    
    // Adjust selection indices
    std::set<int32> newSelection;
    for (int32 sel : fSelectedIndices) {
        if (sel < index)
            newSelection.insert(sel);
        else if (sel > index)
            newSelection.insert(sel - 1);
        // else: sel == index, skip (removing this item)
    }
    fSelectedIndices = newSelection;
    
    // Remove and delete item
    delete fItems.RemoveItemAt(index);
    
    // Rebuild HashMap indices for items after removed one.
    // Must always do this, even during bulk loading, to keep
    // the HashMap consistent for subsequent batch insertions.
    for (int32 i = index; i < fItems.CountItems(); i++) {
        EmailItem* it = fItems.ItemAt(i);
        if (it != NULL && it->Ref() != NULL) {
            fNodeToIndex[it->Ref()->nodeRef] = i;
        }
    }
    
    _UpdateScrollBar();
    if (fContentView != NULL) fContentView->Invalidate();
}


void
EmailListView::UpdateRow(EmailItem* item)
{
    if (item == NULL)
        return;
    
    int32 index = fItems.IndexOf(item);
    if (index >= 0) {
        InvalidateItem(index);
    }
}


void
EmailListView::InvalidateItem(int32 index)
{
    if (index < 0 || index >= fItems.CountItems())
        return;
    
    BRect rowRect = _RowRect(index);
    if (fContentView != NULL) fContentView->Invalidate(rowRect);
}


void
EmailListView::Invalidate()
{
    if (fContentView != NULL)
        fContentView->Invalidate();
}


void
EmailListView::Invalidate(BRect rect)
{
    if (fContentView != NULL)
        fContentView->Invalidate(rect);
}


BRect
EmailListView::ContentBounds() const
{
    if (fContentView != NULL)
        return fContentView->Bounds();
    return BRect();
}


void
EmailListView::ColumnsResized()
{
    _UpdateScrollBar();
    if (fContentView != NULL)
        fContentView->Invalidate();
}


void
EmailListView::SetShowingTrash(bool showingTrash)
{
    fShowingTrash = showingTrash;
}


void
EmailListView::SetShowingSpam(bool showingSpam)
{
    fShowingSpam = showingSpam;
}


void
EmailListView::SetSpamBlocklist(const std::set<BString>& blocklist)
{
    fSpamBlocklist = blocklist;
}


EmailItem*
EmailListView::CurrentSelection(EmailItem* lastSelected) const
{
    if (lastSelected == NULL) {
        // Start iteration from beginning
        fSelectionIterIndex = 0;
    }
    
    int32 count = fItems.CountItems();
    while (fSelectionIterIndex < count) {
        EmailItem* item = fItems.ItemAt(fSelectionIterIndex);
        fSelectionIterIndex++;
        
        if (item != NULL && item->IsSelected()) {
            if (lastSelected == NULL || item != lastSelected) {
                return item;
            }
        }
    }
    
    return NULL;
}


void
EmailListView::AddToSelection(EmailItem* item)
{
    if (item == NULL)
        return;
    
    int32 index = fItems.IndexOf(item);
    if (index >= 0) {
        Select(index, true);  // extend = true
    }
}


void
EmailListView::SetFocusRow(EmailItem* item, bool select)
{
    if (item == NULL)
        return;
    
    int32 index = fItems.IndexOf(item);
    if (index >= 0) {
        if (select) {
            Select(index, true);  // extend selection
        }
        ScrollToItem(index);
    }
}


EmailItem*
EmailListView::FocusRow() const
{
    int32 index = FirstSelected();
    if (index >= 0)
        return ItemAt(index);
    return NULL;
}


bool
EmailListView::SelectNext()
{
    int32 count = fItems.CountItems();
    if (count == 0)
        return false;

    int32 current = LastSelected();
    int32 next = (current >= 0) ? current + 1 : 0;

    if (next >= count)
        return false;  // Already at end

    Select(next);
    ScrollToSelection();
    return true;
}


bool
EmailListView::SelectPrevious()
{
    int32 count = fItems.CountItems();
    if (count == 0)
        return false;

    int32 current = FirstSelected();
    int32 prev = (current >= 0) ? current - 1 : count - 1;

    if (prev < 0)
        return false;  // Already at beginning

    Select(prev);
    ScrollToSelection();
    return true;
}


int32
EmailListView::IndexOf(EmailItem* item) const
{
    return fItems.IndexOf(item);
}


void
EmailListView::ScrollTo(EmailItem* item)
{
    if (item == NULL)
        return;
    
    int32 index = fItems.IndexOf(item);
    if (index >= 0) {
        ScrollToItem(index);
    }
}


void
EmailListView::ScrollToItem(int32 index)
{
    EnsureVisible(index);
}


void
EmailListView::SetSelectionMessage(BMessage* message)
{
    delete fSelectionMessage;
    fSelectionMessage = message;
}


void
EmailListView::SetInvocationMessage(BMessage* message)
{
    delete fInvocationMessage;
    fInvocationMessage = message;
}


void
EmailListView::MakeEmpty()
{
    _StopAllWatches();
    
    int32 count = fItems.CountItems();
    if (count > 1000) {
        // Large list: collect pointers and swap HashMap off-thread.
        // The list clear is instant (non-owning), and both item deletion
        // and HashMap deallocation happen on a background thread.
        _DisposerData* data = new _DisposerData();
        data->count = count;
        data->items = new EmailItem*[count];
        for (int32 i = 0; i < count; i++)
            data->items[i] = fItems.ItemAt(i);
        fItems.MakeEmpty();
        
        // Swap HashMap into disposer — avoids ~1s clear() on UI thread
        data->hashMap = new std::unordered_map<node_ref, int32,
            NodeRefHash, NodeRefEqual>(std::move(fNodeToIndex));
        // fNodeToIndex is now empty (moved-from state)
        
        thread_id thread = spawn_thread(_ItemDisposerThread,
            "item_disposer", B_LOW_PRIORITY, data);
        if (thread >= 0) {
            resume_thread(thread);
        } else {
            for (int32 i = 0; i < count; i++)
                delete data->items[i];
            delete[] data->items;
            delete data->hashMap;
            delete data;
        }
    } else {
        for (int32 i = 0; i < count; i++)
            delete fItems.ItemAt(i);
        fItems.MakeEmpty();
        fNodeToIndex.clear();
    }
    
    fSelectedIndices.clear();
    fAnchorIndex = -1;
    fLastClickIndex = -1;
    
    _UpdateScrollBar();
    if (fContentView != NULL)
        fContentView->Invalidate();
}


int32
EmailListView::CurrentSelectionIndex() const
{
    return FirstSelected();
}


// Sort comparison function for BObjectList::SortItems
// state points to a SortState struct with column and order
struct SortState {
    EmailListView::SortColumn column;
    EmailListView::SortOrder order;
};

static int
CompareEmailItems(const EmailItem* a, const EmailItem* b, void* state)
{
    SortState* sortState = (SortState*)state;
    return EmailListView::CompareItems(a, b, sortState->column, sortState->order);
}


void
EmailListView::SortItems()
{
    if (fItems.CountItems() <= 1)
        return;
    
    // Remember selection by node_ref so we can restore it after the sort
    // (indices change but node_refs are stable identities)
    std::vector<node_ref> selectedRefs;
    for (int32 sel : fSelectedIndices) {
        EmailItem* item = fItems.ItemAt(sel);
        if (item != NULL && item->Ref() != NULL) {
            selectedRefs.push_back(item->Ref()->nodeRef);
        }
    }
    
    // Sort using our comparison function
    SortState state = { fSortColumn, fSortOrder };
    fItems.SortItems(CompareEmailItems, &state);
    
    // Rebuild HashMap and restore selection
    fNodeToIndex.clear();
    fSelectedIndices.clear();
    
    for (int32 i = 0; i < fItems.CountItems(); i++) {
        EmailItem* item = fItems.ItemAt(i);
        if (item != NULL && item->Ref() != NULL) {
            fNodeToIndex[item->Ref()->nodeRef] = i;
            
            // Check if this was selected
            for (const auto& selRef : selectedRefs) {
                if (selRef == item->Ref()->nodeRef) {
                    fSelectedIndices.insert(i);
                    item->SetSelected(true);
                    break;
                }
            }
        }
    }
    
    if (fContentView != NULL) fContentView->Invalidate();
}


int
EmailListView::CompareItems(const EmailItem* a, const EmailItem* b,
                              SortColumn column, SortOrder order)
{
    if (a == NULL || b == NULL)
        return 0;
    
    EmailRef* refA = a->Ref();
    EmailRef* refB = b->Ref();
    if (refA == NULL || refB == NULL)
        return 0;
    
    int result = 0;
    
    switch (column) {
        case kSortByStatus:
            result = strcasecmp(refA->status.String(), refB->status.String());
            break;
        case kSortByStar:
            result = (int)refB->isStarred - (int)refA->isStarred;
            break;
        case kSortByAttachment:
            result = (int)refB->hasAttachment - (int)refA->hasAttachment;
            break;
        case kSortByFrom:
            result = strcasecmp(refA->from.String(), refB->from.String());
            break;
        case kSortByTo:
            result = strcasecmp(refA->to.String(), refB->to.String());
            break;
        case kSortBySubject:
            result = strcasecmp(refA->subject.String(), refB->subject.String());
            break;
        case kSortByDate:
            if (refA->when < refB->when)
                result = -1;
            else if (refA->when > refB->when)
                result = 1;
            else {
                // Tiebreaker: inode number (sequential on BFS, higher = newer file).
                // MAIL:when is second-precision, so two emails sent/received within
                // the same second (e.g. sending to yourself) have identical timestamps.
                // Inode numbers are assigned sequentially, making them a reliable
                // proxy for creation order within the same volume.
                if (refA->nodeRef.node < refB->nodeRef.node)
                    result = -1;
                else if (refA->nodeRef.node > refB->nodeRef.node)
                    result = 1;
            }
            break;
        case kSortByAccount:
            result = strcasecmp(refA->account.String(), refB->account.String());
            break;
    }
    
    // Reverse for descending
    if (order == kSortDescending)
        result = -result;
    
    return result;
}


// =============================================================================
// Query Execution Implementation
// =============================================================================


void
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
EmailListView::StartQuery(const char* predicate, BObjectList<BVolume, true>* volumes,
                          bool showTrash, bool showSpam, bool attachmentsOnly)
#else
EmailListView::StartQuery(const char* predicate, BObjectList<BVolume>* volumes,
                          bool showTrash, bool showSpam, bool attachmentsOnly)
#endif
{
    // Stop any existing query
    StopQuery();
    _StopLiveQueries();
    
    // Clear current list
    MakeEmpty();
    
    // Increment query ID to invalidate any pending messages from old queries
    fCurrentQueryId++;
    
    // Store query parameters
    fQueryPredicate = predicate;
    fQueryShowTrash = showTrash;
    fQueryShowSpam = showSpam;
    fQueryAttachmentsOnly = attachmentsOnly;
    fLoadedCount = 0;
    fTotalCount = 0;
    
    // Signal any previous loader to stop (via its own flag)
    if (fCurrentStopFlag)
        *fCurrentStopFlag = true;
    
    // Allocate a new stop flag for this query's threads.
    // Old flag shared_ptr is released here; if a previous loader thread
    // still holds a copy, the flag stays alive until that thread exits.
    fCurrentStopFlag = std::make_shared<volatile bool>(false);
    
    // Copy volumes
    fQueryVolumes.MakeEmpty();
    if (volumes != NULL) {
        for (int32 i = 0; i < volumes->CountItems(); i++) {
            BVolume* vol = volumes->ItemAt(i);
            if (vol != NULL)
                fQueryVolumes.AddItem(new BVolume(*vol));
        }
    } else {
        // Default: use all queryable volumes
        BVolumeRoster roster;
        BVolume vol;
        while (roster.GetNextVolume(&vol) == B_OK) {
            if (vol.KnowsQuery())
                fQueryVolumes.AddItem(new BVolume(vol));
        }
    }
    
    // Use two-phase loading (recent first) only for queries where MAIL:when
    // is reliably present. Draft emails lack MAIL:when entirely, so the
    // time-split predicate (MAIL:when >= cutoff) would miss them. In that
    // case cutoffTime stays 0 and a single-phase load is used instead.
    BString predicateStr(predicate);
    time_t cutoffTime = 0;
    if (predicateStr.FindFirst("MAIL:draft") < 0)
        cutoffTime = time(NULL) - (30 * 24 * 60 * 60);
    
    fQueryCutoffTime = cutoffTime;
    
    LoaderData* data = new LoaderData();
    data->view = this;
    data->messenger = BMessenger(this);
    data->predicate = predicate;
    data->showTrash = showTrash;
    data->showSpam = showSpam;
    data->attachmentsOnly = attachmentsOnly;
    data->spamBlocklist = fSpamBlocklist;
    data->stopFlag = fCurrentStopFlag;
    data->currentQueryId = &fCurrentQueryId;
    data->cutoffTime = cutoffTime;
    data->phase = 1;  // Phase 1: recent emails first
    data->queryId = fCurrentQueryId;
    
    // Copy volumes to loader data
    for (int32 i = 0; i < fQueryVolumes.CountItems(); i++) {
        BVolume* vol = fQueryVolumes.ItemAt(i);
        if (vol != NULL)
            data->volumes.AddItem(new BVolume(*vol));
    }
    
    // Send loading started notification
    _SendLoadingUpdate(true, false);
    
    // Start loader thread
    fLoaderThread = spawn_thread(_LoaderThread, "email_loader",
                                  B_NORMAL_PRIORITY, data);
    if (fLoaderThread >= 0) {
        resume_thread(fLoaderThread);
    } else {
        delete data;
        _SendLoadingUpdate(false, true);
    }
}


void
EmailListView::StopQuery()
{
    if (fLoaderThread >= 0) {
        if (fCurrentStopFlag)
            *fCurrentStopFlag = true;
        fLoaderThread = -1;
    }
}


int32
EmailListView::LoadingProgress() const
{
    if (fLoaderThread < 0)
        return -1;
    if (fTotalCount <= 0)
        return 0;
    return (fLoadedCount * 100) / fTotalCount;
}


int32
EmailListView::_LoaderThread(void* data)
{
    LoaderData* loaderData = (LoaderData*)data;
    
    BMessenger messenger = loaderData->messenger;
    BString basePredicate = loaderData->predicate;
    bool showTrash = loaderData->showTrash;
    bool showSpam = loaderData->showSpam;
    bool attachmentsOnly = loaderData->attachmentsOnly;
    volatile bool* stopFlag = loaderData->stopFlag.get();
    volatile int32* currentQueryId = loaderData->currentQueryId;
    time_t cutoffTime = loaderData->cutoffTime;
    int32 phase = loaderData->phase;
    int32 queryId = loaderData->queryId;
    
    // Helper: check if this thread is stale (stop flag set OR queryId changed)
    #define IS_STALE() (*stopFlag || *currentQueryId != queryId)
    
    // Build phase-specific predicate
    BString predicate;
    if (cutoffTime > 0 && phase == 1) {
        // Phase 1: Recent emails (last 30 days)
        predicate.SetToFormat("((%s)&&(MAIL:when>=%ld))",
                              basePredicate.String(), cutoffTime);
    } else if (cutoffTime > 0 && phase == 2) {
        // Phase 2: Older emails
        predicate.SetToFormat("((%s)&&(MAIL:when<%ld))",
                              basePredicate.String(), cutoffTime);
    } else {
        predicate = basePredicate;
    }
    
    // Batch of fully-loaded EmailRefs to send
    const int32 kBatchSize = 50;
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
    BObjectList<EmailRef, false> batch(kBatchSize);
#else
    BObjectList<EmailRef> batch(kBatchSize);
#endif
    int32 totalLoaded = 0;
    
    // Query each volume
    for (int32 v = 0; v < loaderData->volumes.CountItems(); v++) {
        if (IS_STALE())
            break;
            
        BVolume* volume = loaderData->volumes.ItemAt(v);
        if (volume == NULL || !volume->KnowsQuery())
            continue;
        
        char volName[B_FILE_NAME_LENGTH];
        volume->GetName(volName);
        
        // Get trash path for this volume
        BPath trashPath;
        find_directory(B_TRASH_DIRECTORY, &trashPath, false, volume);
        BString trashLower = trashPath.Path();
        trashLower.ToLower();
        
        // Create and run query
        BQuery query;
        query.SetVolume(volume);
        query.SetPredicate(predicate.String());
        
        if (query.Fetch() != B_OK) {
            fprintf(stderr, "Query fetch failed on volume %s\n", volName);
            continue;
        }
        
        entry_ref ref;
        while (query.GetNextRef(&ref) == B_OK) {
            if (IS_STALE())
                break;
            
            // Check trash status
            BPath path(&ref);
            BString pathLower = path.Path();
            pathLower.ToLower();
            bool inTrash = (trashLower.Length() > 0 && 
                           pathLower.FindFirst(trashLower) >= 0);
            
            // Filter by trash mode
            if (showTrash && !inTrash)
                continue;
            if (!showTrash && inTrash)
                continue;
            
            // Check spam classification attribute
            BNode node(&ref);
            BString classification;
            bool isSpam = (node.InitCheck() == B_OK
                && node.ReadAttrString("MAIL:classification", &classification) == B_OK
                && classification.ICompare("Spam") == 0);

            // Auto-tag emails from blocked senders
            if (!isSpam && !loaderData->spamBlocklist.empty()
                && node.InitCheck() == B_OK) {
                char fromBuf[256] = "";
                node.ReadAttr("MAIL:from", B_STRING_TYPE, 0, fromBuf,
                    sizeof(fromBuf) - 1);
                if (fromBuf[0] != '\0') {
                    BString fromStr(fromBuf);
                    BString addr;
                    int32 open = fromStr.FindFirst('<');
                    int32 close = fromStr.FindFirst('>', open);
                    if (open >= 0 && close > open) {
                        fromStr.CopyInto(addr, open + 1, close - open - 1);
                    } else {
                        addr = fromStr;
                    }
                    addr.Trim();
                    addr.ToLower();

                    bool blocked = (loaderData->spamBlocklist.count(addr) > 0);
                    if (!blocked) {
                        int32 at = addr.FindFirst('@');
                        if (at >= 0) {
                            BString domain;
                            addr.CopyInto(domain, at, addr.Length() - at);
                            blocked = (loaderData->spamBlocklist.count(domain) > 0);
                        }
                    }
                    if (blocked) {
                        BString spam("Spam");
                        node.WriteAttrString("MAIL:classification", &spam);
                        isSpam = true;
                    }
                }
            }

            // Filter by spam mode:
            // - Spam view: show only spam
            // - Trash view: show everything in trash (including spam)
            // - All other views: exclude spam
            if (showSpam && !isSpam)
                continue;
            if (!showSpam && !showTrash && isSpam)
                continue;
            
            // Create EmailRef - disk I/O happens here in background thread
            EmailRef* emailRef = new EmailRef(ref);
            
            // Filter by attachment if needed
            if (attachmentsOnly && !emailRef->hasAttachment) {
                delete emailRef;
                continue;
            }
            
            // Add to batch
            batch.AddItem(emailRef);
            totalLoaded++;
            
            // Send batch when full
            if (batch.CountItems() >= kBatchSize) {
                if (IS_STALE()) {
                    // Stale - delete accumulated EmailRefs and exit
                    for (int32 i = 0; i < batch.CountItems(); i++) {
                        delete batch.ItemAt(i);
                    }
                    batch.MakeEmpty();
                    break;
                }
                BMessage msg(kMsgLoaderBatch);
                msg.AddInt32("phase", phase);
                msg.AddInt32("queryId", queryId);
                for (int32 i = 0; i < batch.CountItems(); i++) {
                    msg.AddPointer("emailref", batch.ItemAt(i));
                }
                messenger.SendMessage(&msg);
                batch.MakeEmpty();
                
                // Yield briefly so the window thread can process the batch
                // and paint. Without this, a fast disk can flood the message
                // queue and the UI appears frozen during loading.
                snooze(5000);
            }
        }
        
        // Flush batch after each volume so results appear without waiting
        // for subsequent (potentially slow) volumes to be scanned
        if (batch.CountItems() > 0 && !IS_STALE()) {
            BMessage msg(kMsgLoaderBatch);
            msg.AddInt32("phase", phase);
            msg.AddInt32("queryId", queryId);
            for (int32 i = 0; i < batch.CountItems(); i++) {
                msg.AddPointer("emailref", batch.ItemAt(i));
            }
            messenger.SendMessage(&msg);
            batch.MakeEmpty();
        }
    }
    
    // Send remaining batch only if not stale
    if (batch.CountItems() > 0) {
        if (IS_STALE()) {
            // Stale - clean up unsent EmailRefs
            for (int32 i = 0; i < batch.CountItems(); i++) {
                delete batch.ItemAt(i);
            }
            batch.MakeEmpty();
        } else {
            BMessage msg(kMsgLoaderBatch);
            msg.AddInt32("phase", phase);
            msg.AddInt32("queryId", queryId);
            for (int32 i = 0; i < batch.CountItems(); i++) {
                msg.AddPointer("emailref", batch.ItemAt(i));
            }
            messenger.SendMessage(&msg);
            batch.MakeEmpty();
        }
    }
    
    // Send phase completion message only if not stale.
    // During shutdown, the looper may be locked or destroyed,
    // so we must not call SendMessage.
    if (!IS_STALE()) {
        BMessage done(phase == 1 ? kMsgPhase1Done : kMsgPhase2Done);
        done.AddInt32("count", totalLoaded);
        done.AddInt32("queryId", queryId);
        messenger.SendMessage(&done);
    }
    
    #undef IS_STALE
    
    delete loaderData;
    return 0;
}


void
EmailListView::_ProcessLoaderBatch(BMessage* message)
{
    // Receive pre-loaded EmailRef pointers from background thread
    
    int32 phase = 1;
    message->FindInt32("phase", &phase);
    
    void* ptr;
    int32 index = 0;
    int32 added = 0;
    
    // All items use sorted insertion with CopyBits optimization.
    // Items below visible area skip drawing entirely, so this is
    // efficient even for tens of thousands of items.
    while (message->FindPointer("emailref", index++, &ptr) == B_OK) {
        EmailRef* emailRef = (EmailRef*)ptr;
        if (emailRef != NULL) {
            AddEmailSorted(emailRef);
            fLoadedCount++;
            added++;
        }
    }
    
    if (added > 0) {
        // Single invalidate for the entire batch — avoids per-item flicker
        if (fContentView != NULL)
            fContentView->Invalidate();
        
        // Force draw immediately so items appear progressively
        if (Window() != NULL)
            Window()->UpdateIfNeeded();
        
        // Throttle loading update messages
        static bigtime_t lastUpdateTime = 0;
        bigtime_t now = system_time();
        
        if ((now - lastUpdateTime) > 500000) {
            _SendLoadingUpdate(true, false);
            lastUpdateTime = now;
        }
    }
}


// =============================================================================
// Body / Full-Text Search
// =============================================================================

// Data passed to the body search background thread.
struct BodySearchData {
    BMessenger          messenger;
    std::shared_ptr<volatile bool> stopFlag;
    volatile int32*     currentQueryId;
    int32               queryId;
    BString             searchText;
    bool                caseSensitive;
    bool                fullText;      // true = check headers first
    BList               refs;          // List of entry_ref* (owned, thread frees them)
    std::unordered_set<node_ref, NodeRefHash, NodeRefEqual> skipNodes;  // Already-matched nodes to skip
};


void
EmailListView::StartBodySearch(const char* searchText, bool caseSensitive,
    bool fullText, bool keepExisting, BList* externalScope)
{
    if (searchText == NULL || searchText[0] == '\0')
        return;
    
    // Stop any running body search
    StopBodySearch();
    
    // Build the search scope. If an external scope is provided (e.g. the
    // full BFS result set saved before re-adding preserved matches), use
    // that. Otherwise snapshot the current list.
    BList refs;
    if (externalScope != NULL) {
        for (int32 i = 0; i < externalScope->CountItems(); i++) {
            entry_ref* ref = (entry_ref*)externalScope->ItemAt(i);
            if (ref != NULL)
                refs.AddItem(new entry_ref(*ref));
        }
    } else {
        for (int32 i = 0; i < fItems.CountItems(); i++) {
            EmailItem* item = fItems.ItemAt(i);
            if (item != NULL && item->Ref() != NULL)
                refs.AddItem(new entry_ref(item->Ref()->entryRef));
        }
    }
    
    int32 total = refs.CountItems();
    if (total == 0)
        return;
    
    // Bump query ID so any pending loader batches are discarded
    fCurrentQueryId++;
    
    // Stop live queries — they'd add items during the search
    _StopLiveQueries();
    
    // Build a set of node_refs for items to skip (only when keeping
    // existing matches from a previous search). When keepExisting is
    // false, clear the list and scan everything.
    std::unordered_set<node_ref, NodeRefHash, NodeRefEqual> skipNodes;
    if (keepExisting) {
        for (int32 i = 0; i < fItems.CountItems(); i++) {
            EmailItem* item = fItems.ItemAt(i);
            if (item != NULL && item->Ref() != NULL)
                skipNodes.insert(item->Ref()->nodeRef);
        }
    } else {
        MakeEmpty();
    }
    
    // Reset counters (scanned starts at the number we're skipping)
    fBodySearchTotal = total;
    fBodySearchScanned = (int32)skipNodes.size();
    fBodySearchFound = fItems.CountItems();
    
    // Show initial status
    BString status;
    status.SetToFormat("0 of %ld", (long)total);
    SetStatusText(status.String());
    StartLoadingDots();
    
    // Prepare thread data
    fBodySearchStopFlag = std::make_shared<volatile bool>(false);
    
    BodySearchData* data = new BodySearchData();
    data->messenger = BMessenger(this);
    data->stopFlag = fBodySearchStopFlag;
    data->currentQueryId = &fCurrentQueryId;
    data->queryId = fCurrentQueryId;
    data->searchText = searchText;
    data->caseSensitive = caseSensitive;
    data->fullText = fullText;
    data->skipNodes = std::move(skipNodes);
    
    // Transfer refs to thread data
    for (int32 i = 0; i < refs.CountItems(); i++)
        data->refs.AddItem(refs.ItemAt(i));
    
    fBodySearchThread = spawn_thread(_BodySearchThread, "body_search",
        B_NORMAL_PRIORITY, data);
    if (fBodySearchThread >= 0) {
        resume_thread(fBodySearchThread);
    } else {
        // Failed to spawn thread — clean up
        for (int32 i = 0; i < data->refs.CountItems(); i++)
            delete (entry_ref*)data->refs.ItemAt(i);
        delete data;
        StopLoadingDots();
        _SendLoadingUpdate(false, true);
    }
}


void
EmailListView::StopBodySearch()
{
    if (fBodySearchStopFlag)
        *fBodySearchStopFlag = true;
    if (fBodySearchThread >= 0) {
        status_t result;
        wait_for_thread(fBodySearchThread, &result);
        fBodySearchThread = -1;
    }
}


/*static*/ int32
EmailListView::_BodySearchThread(void* data)
{
    BodySearchData* searchData = (BodySearchData*)data;
    
    BMessenger messenger = searchData->messenger;
    volatile bool* stopFlag = searchData->stopFlag.get();
    volatile int32* currentQueryId = searchData->currentQueryId;
    int32 queryId = searchData->queryId;
    BString searchText = searchData->searchText;
    bool caseSensitive = searchData->caseSensitive;
    bool fullText = searchData->fullText;
    
    #define BODY_IS_STALE() (*stopFlag || *currentQueryId != queryId)
    
    int32 total = searchData->refs.CountItems();
    int32 scanned = 0;
    
    // No batching — each match is sent immediately so it appears in the
    // list as soon as it's found. AddEmailSorted uses CopyBits to shift
    // existing rows and only draws the new row, so this is flicker-free.
    
    for (int32 i = 0; i < total; i++) {
        if (BODY_IS_STALE())
            break;
        
        entry_ref* ref = (entry_ref*)searchData->refs.ItemAt(i);
        if (ref == NULL)
            continue;
        
        bool matched = false;
        
        // Full-text mode: check header attributes first (fast path)
        if (fullText) {
            EmailRef headerRef(*ref);
            
            // Skip emails already in the result list (carried over from
            // a previous search with a narrower scope). Check here because
            // EmailRef's constructor already resolved the node_ref.
            if (searchData->skipNodes.count(headerRef.nodeRef) > 0) {
                scanned++;
                continue;
            }
            
            if (caseSensitive) {
                if (headerRef.from.FindFirst(searchText) >= 0
                    || headerRef.to.FindFirst(searchText) >= 0
                    || headerRef.subject.FindFirst(searchText) >= 0
                    || headerRef.account.FindFirst(searchText) >= 0) {
                    matched = true;
                }
            } else {
                if (headerRef.from.IFindFirst(searchText) >= 0
                    || headerRef.to.IFindFirst(searchText) >= 0
                    || headerRef.subject.IFindFirst(searchText) >= 0
                    || headerRef.account.IFindFirst(searchText) >= 0) {
                    matched = true;
                }
            }
        }
        
        // Search the body text (skip if already matched via headers)
        if (!matched) {
            BString bodyText;
            if (ExtractBodyText(*ref, &bodyText) && bodyText.Length() > 0) {
                if (caseSensitive) {
                    if (bodyText.FindFirst(searchText) >= 0)
                        matched = true;
                } else {
                    if (bodyText.IFindFirst(searchText) >= 0)
                        matched = true;
                }
            }
        }
        
        scanned++;
        
        // Send match immediately
        if (matched && !BODY_IS_STALE()) {
            BMessage msg(kMsgBodySearchBatch);
            msg.AddInt32("queryId", queryId);
            msg.AddInt32("scanned", scanned);
            msg.AddPointer("emailref", new EmailRef(*ref));
            messenger.SendMessage(&msg);
        }
        
        // Send progress update periodically (every 50 emails)
        if ((scanned % 50) == 0 && !BODY_IS_STALE()) {
            BMessage progress(kMsgBodySearchProgress);
            progress.AddInt32("queryId", queryId);
            progress.AddInt32("scanned", scanned);
            messenger.SendMessage(&progress);
        }
    }
    
    // Send completion
    if (!BODY_IS_STALE()) {
        BMessage done(kMsgBodySearchDone);
        done.AddInt32("queryId", queryId);
        done.AddInt32("scanned", scanned);
        messenger.SendMessage(&done);
    }
    
    #undef BODY_IS_STALE
    
    // Clean up refs
    for (int32 i = 0; i < searchData->refs.CountItems(); i++)
        delete (entry_ref*)searchData->refs.ItemAt(i);
    
    delete searchData;
    return 0;
}


void
EmailListView::_StartLiveQueries()
{
    _StopLiveQueries();
    
    if (fQueryPredicate.IsEmpty())
        return;
    
    for (int32 i = 0; i < fQueryVolumes.CountItems(); i++) {
        BVolume* volume = fQueryVolumes.ItemAt(i);
        if (volume == NULL)
            continue;
        
        BQuery* query = new BQuery();
        query->SetVolume(volume);
        query->SetPredicate(fQueryPredicate.String());
        query->SetTarget(BMessenger(this));
        
        if (query->Fetch() == B_OK) {
            fLiveQueries.AddItem(query);
        } else {
            delete query;
        }
    }
}


void
EmailListView::_StartInterimLiveQueries()
{
    // Start live queries with a tight time window (last 60 seconds) to catch
    // new emails arriving during phase 2 loading.  The narrow predicate keeps
    // the initial result set tiny (all duplicates of phase 1, discarded by the
    // HashMap dedup check in B_ENTRY_CREATED).  New emails that arrive after
    // Fetch() are caught by the live monitoring regardless of timestamps.
    _StopLiveQueries();
    
    if (fQueryPredicate.IsEmpty())
        return;
    
    time_t recentCutoff = time(NULL) - 60;
    BString interimPredicate;
    interimPredicate.SetToFormat("((%s)&&(MAIL:when>=%ld))",
        fQueryPredicate.String(), recentCutoff);
    
    for (int32 i = 0; i < fQueryVolumes.CountItems(); i++) {
        BVolume* volume = fQueryVolumes.ItemAt(i);
        if (volume == NULL)
            continue;
        
        BQuery* query = new BQuery();
        query->SetVolume(volume);
        query->SetPredicate(interimPredicate.String());
        query->SetTarget(BMessenger(this));
        
        if (query->Fetch() == B_OK) {
            fLiveQueries.AddItem(query);
        } else {
            delete query;
        }
    }
}


void
EmailListView::_StopLiveQueries()
{
    fLiveQueries.MakeEmpty();
}


void
EmailListView::_SendLoadingUpdate(bool loading, bool complete)
{
    if (fTarget == NULL)
        return;
    
    BMessage msg(kMsgLoadingUpdate);
    msg.AddBool("loading", loading);
    msg.AddInt32("count", fLoadedCount);
    msg.AddBool("complete", complete);
    
    BMessenger(fTarget).SendMessage(&msg);
}


void
EmailListView::_NotifyCountChanged()
{
    // Throttled notification to parent window to update email count.
    // Called when live queries add or remove emails.
    static bigtime_t lastNotifyTime = 0;
    bigtime_t now = system_time();
    
    if (now - lastNotifyTime < 500000)  // 500ms throttle
        return;
    lastNotifyTime = now;
    
    _SendLoadingUpdate(false, false);
}


// =============================================================================
// Container and status methods
// =============================================================================

status_t
EmailListView::SaveColumnState(BMessage* into) const
{
    if (fColumnHeader == NULL || into == NULL)
        return B_BAD_VALUE;

    return fColumnHeader->SaveState(into);
}


status_t
EmailListView::RestoreColumnState(const BMessage* from)
{
    if (fColumnHeader == NULL || from == NULL)
        return B_BAD_VALUE;

    status_t result = fColumnHeader->RestoreState(from);

    if (result == B_OK) {
        // Apply restored sort state to the list
        EmailColumn sortCol = fColumnHeader->SortColumn();
        ::SortOrder headerSortOrder = fColumnHeader->GetSortOrder();

        SortColumn listSortCol;
        switch (sortCol) {
            case kColumnStatus:
                listSortCol = kSortByStatus;
                break;
            case kColumnStar:
                listSortCol = kSortByStar;
                break;
            case kColumnAttachment:
                listSortCol = kSortByAttachment;
                break;
            case kColumnFrom:
                listSortCol = kSortByFrom;
                break;
            case kColumnTo:
                listSortCol = kSortByTo;
                break;
            case kColumnSubject:
                listSortCol = kSortBySubject;
                break;
            case kColumnDate:
                listSortCol = kSortByDate;
                break;
            case kColumnAccount:
                listSortCol = kSortByAccount;
                break;
            default:
                listSortCol = kSortByDate;
                break;
        }

        SortOrder listSortOrder = (headerSortOrder == ::kSortAscending)
            ? kSortAscending
            : kSortDescending;

        SetSortColumn(listSortCol, listSortOrder);
    }

    return result;
}


void
EmailListView::SetStatusText(const char* text)
{
    if (fStatusLabel != NULL)
        fStatusLabel->SetText(text);
}


void
EmailListView::StartLoadingDots()
{
    if (fLoadingDots != NULL)
        fLoadingDots->Start();
}


void
EmailListView::StopLoadingDots()
{
    if (fLoadingDots != NULL)
        fLoadingDots->Stop();
}
