/*
 * ToolBarView.h - Custom toolbar with label-below-icon buttons
 * Distributed under the terms of the MIT License.
 *
 * Drop-in replacement for BToolBar that draws icons centered above labels.
 * BToolBar/BButton hardcode label-right-of-icon with no public API to change
 * the layout, so this reimplements button rendering using BControlLook for
 * native frame/background drawing (hover, pressed, disabled, flat states).
 *
 * ToolBarView exposes the same API surface as BToolBar (AddAction, FindButton,
 * SetActionEnabled, SetActionVisible, AddGlue, AddSeparator) to minimize
 * changes in calling code.  AllAttached() equalizes button widths for a
 * uniform toolbar appearance.
 */

#ifndef TOOLBAR_VIEW_H
#define TOOLBAR_VIEW_H

#include <Bitmap.h>
#include <GroupView.h>
#include <View.h>


class ToolBarButton : public BView {
public:
								ToolBarButton(const char* name,
									const BBitmap* icon, BMessage* message,
									BHandler* target);
	virtual						~ToolBarButton();

	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseUp(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 code,
									const BMessage* dragMessage);
	virtual	void				AttachedToWindow();

	virtual	BSize				MinSize();
	virtual	BSize				MaxSize();
	virtual	BSize				PreferredSize();

			void				SetLabel(const char* label);
			const char*			Label() const { return fLabel.Length() > 0 ? fLabel.String() : NULL; }

			void				SetEnabled(bool enabled);
			bool				IsEnabled() const { return fEnabled; }

			void				SetToolTip(const char* text);

			BMessage*			Message() const { return fMessage; }
			uint32				Command() const;

			void				SetTarget(BHandler* target);

private:
			BSize				_CalculateSize();

			BBitmap*			fIcon;
			BString				fLabel;
			BMessage*			fMessage;
			BHandler*			fTarget;
			bool				fEnabled;
			bool				fInside;
			bool				fPressed;
			float				fScale;
  
			BSize				fCachedSize;
};


class ToolBarView : public BGroupView {
public:
								ToolBarView();
	virtual						~ToolBarView();

	virtual	void				Hide();
	virtual	void				AllAttached();

			void				AddAction(uint32 command, BHandler* target,
									const BBitmap* icon,
									const char* toolTipText = NULL,
									const char* text = NULL);
			void				AddSeparator();
			void				AddGlue();
			void				AddView(BView* view);

			void				SetActionEnabled(uint32 command, bool enabled);
			void				SetActionVisible(uint32 command, bool visible);

			ToolBarButton*		FindButton(uint32 command) const;

private:
			void				_HideToolTips() const;
};


#endif // TOOLBAR_VIEW_H
