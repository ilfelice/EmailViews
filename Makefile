## Haiku Generic Makefile ##

NAME = EmailViews
TYPE = APP
APP_MIME_SIG = application/x-vnd.EmailViews
LOCALES = en ja de ca fr tr es
SRCS = EmailViews.cpp \
       AboutWindow.cpp \
       ToolBarView.cpp \
       TrashItemView.cpp \
       QueryItem.cpp \
       QueryListView.cpp \
       SearchBarView.cpp \
       EmailListView.cpp \
       EmailColumnHeader.cpp \
       EmailAccountMap.cpp \
       QueryNameDialog.cpp \
       AttachmentStripView.cpp \
       TimeRangeSlider.cpp \
       PlaceholderTextView.cpp \
       LoadingDots.cpp \
       reader/EmailReaderWindow.cpp \
       reader/Content.cpp \
       reader/Header.cpp \
       reader/Enclosures.cpp \
       reader/AddressTextControl.cpp \
       reader/Signature.cpp \
       reader/FindWindow.cpp \
       reader/KUndoBuffer.cpp \
       reader/MailPopUpMenu.cpp \
       reader/MessageStatus.cpp \
       reader/People.cpp \
       reader/QueryList.cpp \
       reader/Utilities.cpp \
       reader/Prefs.cpp \
       reader/Words.cpp \
       reader/WIndex.cpp \
       reader/AutoCompleter.cpp \
       reader/AutoCompleterDefaultImpl.cpp \
       reader/TextViewCompleter.cpp \
       reader/ReaderSettings.cpp \
       reader/ReaderSupport.cpp
LIBS = be mail tracker textencoding localestub shared stdc++
RDEFS = EmailViews.rdef
LINKER_FLAGS = -Wl,--export-dynamic -s
COMPILER_FLAGS = -I. -Ireader -I/boot/system/develop/headers/private/shared -I/boot/system/develop/headers/private/mail -I/boot/system/develop/headers/private/textencoding -I/boot/system/develop/headers/private/storage -I/boot/system/develop/headers/private/interface -DBUILD_TIMESTAMP="\"$(shell date '+%Y-%m-%d %H:%M')\""

## Include the Makefile rules
DEVEL_DIRECTORY := $(shell findpaths -r "makefile_engine" B_FIND_PATH_DEVELOP_DIRECTORY)
include $(DEVEL_DIRECTORY)/etc/makefile-engine
