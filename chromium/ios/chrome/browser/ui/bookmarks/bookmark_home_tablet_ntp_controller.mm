// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/bookmarks/bookmark_home_tablet_ntp_controller.h"

#include <memory>

#include "base/ios/block_types.h"
#include "base/logging.h"
#include "base/metrics/user_metrics.h"
#include "base/metrics/user_metrics_action.h"
#include "base/strings/sys_string_conversions.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/strings/grit/components_strings.h"
#include "google_apis/gaia/google_service_auth_error.h"
#include "ios/chrome/browser/bookmarks/bookmark_model_factory.h"
#include "ios/chrome/browser/bookmarks/bookmarks_utils.h"
#include "ios/chrome/browser/browser_state/chrome_browser_state.h"
#import "ios/chrome/browser/metrics/new_tab_page_uma.h"
#import "ios/chrome/browser/ui/alert_coordinator/action_sheet_coordinator.h"
#import "ios/chrome/browser/ui/bookmarks/bars/bookmark_editing_bar.h"
#import "ios/chrome/browser/ui/bookmarks/bars/bookmark_navigation_bar.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_collection_view.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_edit_view_controller.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_folder_editor_view_controller.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_folder_view_controller.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_home_primary_view.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_home_view_controller_protected.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_home_waiting_view.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_menu_item.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_menu_view.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_navigation_controller.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_panel_view.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_promo_controller.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_utils_ios.h"
#import "ios/chrome/browser/ui/rtl_geometry.h"
#import "ios/chrome/browser/ui/uikit_ui_util.h"
#import "ios/chrome/browser/ui/url_loader.h"
#include "ios/chrome/grit/ios_strings.h"
#include "ios/web/public/referrer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/l10n_util_mac.h"
#include "ui/base/page_transition_types.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using bookmarks::BookmarkNode;

namespace {
// The margin on top to the navigation bar.
const CGFloat kNavigationBarTopMargin = 8.0;
}  // namespace

@interface BookmarkHomeTabletNTPController ()<BookmarkMenuViewDelegate>

#pragma mark - Properties and methods akin to BookmarkHomeHandsetViewController
// When the view is first shown on the screen, this property represents the
// cached value of the y of the content offset of the primary view. This
// property is set to nil after it is used.
@property(nonatomic, strong)
    NSNumber* cachedContentPosition;  // FIXME: INACTIVE

#pragma mark Specific to this class.

// Opens the url.
- (void)loadURL:(const GURL&)url;
#pragma mark View loading, laying out, and switching.
// This method should be called at most once in the life-cycle of the class.
// It should be called at the soonest possible time after the view has been
// loaded, and the bookmark model is loaded.
- (void)loadBookmarkViews;
// Returns whether the menu should be in a side panel that slides in.
- (BOOL)shouldPresentMenuInSlideInPanel;
// Returns the leading margin of the primary view.
- (CGFloat)primaryViewLeadingMargin;
// Moves the menu and primary view to their correct parent views depending on
// the layout.
- (void)moveMenuAndPrimaryViewToAdequateParent;
// Updates the frame of the primary view.
- (void)refreshFrameOfPrimaryView;
// Returns the frame of the primary view.
- (CGRect)frameForPrimaryView;

// The menu button is pressed on the editing bar.
- (void)toggleMenuAnimated;

#pragma mark Navigation bar
- (void)updateNavigationBarWithDuration:(CGFloat)duration
                            orientation:(UIInterfaceOrientation)orientation;
// Whether the edit button on the navigation bar should be shown.
- (BOOL)shouldShowEditButton;

@end

@implementation BookmarkHomeTabletNTPController

@synthesize cachedContentPosition = _cachedContentPosition;
// Property declared in NewTabPagePanelProtocol.
@synthesize delegate = _delegate;

#pragma mark - UIViewController method.

- (void)viewWillLayoutSubviews {
  [super viewWillLayoutSubviews];
  if (![self primaryView] && ![self primaryMenuItem] &&
      self.bookmarks->loaded()) {
    BookmarkMenuItem* item = nil;
    CGFloat position = 0;
    BOOL found =
        bookmark_utils_ios::GetPositionCache(self.bookmarks, &item, &position);
    if (!found)
      item = [self.menuView defaultMenuItem];

    [self updatePrimaryMenuItem:item animated:NO];
  }

  [self moveMenuAndPrimaryViewToAdequateParent];
  CGFloat leadingMargin = [self primaryViewLeadingMargin];

  // Prevent the panelView from hijacking the gestures so that the
  // NTPController's scrollview can still scroll with the gestures.
  [self.panelView enableSideSwiping:NO];

  CGFloat width = self.view.bounds.size.width;
  LayoutRect navBarLayout =
      LayoutRectMake(leadingMargin, width, 0, width - leadingMargin,
                     CGRectGetHeight([self navigationBarFrame]));
  self.navigationBar.frame = LayoutRectGetRect(navBarLayout);
  [self.editingBar setFrame:[self editingBarFrame]];

  UIInterfaceOrientation orient = GetInterfaceOrientation();
  [self refreshFrameOfPrimaryView];
  [[self primaryView] changeOrientation:orient];
  [self updateNavigationBarWithDuration:0 orientation:orient];
  if (![self shouldPresentMenuInSlideInPanel])
    [self updateMenuViewLayout];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = bookmark_utils_ios::mainBackgroundColor();
  self.navigationBar.autoresizingMask = UIViewAutoresizingFlexibleWidth;

  [self.navigationBar setMenuTarget:self action:@selector(toggleMenuAnimated)];
  [self.view addSubview:self.navigationBar];

  if (self.bookmarks->loaded())
    [self loadBookmarkViews];
  else
    [self loadWaitingView];
}

#pragma mark - Views

- (void)updateMenuViewLayout {
  LayoutRect menuLayout =
      LayoutRectMake(0, self.view.bounds.size.width, 0, self.menuWidth,
                     self.view.bounds.size.height);
  self.menuView.frame = LayoutRectGetRect(menuLayout);
}

#pragma mark - Superclass overrides

- (void)loadBookmarkViews {
  [super loadBookmarkViews];
  DCHECK(self.bookmarks->loaded());

  self.menuView.delegate = self;

  [self moveMenuAndPrimaryViewToAdequateParent];

  // Load the last primary menu item which the user had active.
  BookmarkMenuItem* item = nil;
  CGFloat position = 0;
  BOOL found =
      bookmark_utils_ios::GetPositionCache(self.bookmarks, &item, &position);
  if (!found)
    item = [self.menuView defaultMenuItem];

  [self updatePrimaryMenuItem:item animated:NO];

  [[self primaryView] applyContentPosition:position];

  if (found) {
    // If the view has already been laid out, then immediately apply the content
    // position.
    if (self.view.window) {
      [[self primaryView] applyContentPosition:position];
    } else {
      // Otherwise, save the position to be applied once the view has been laid
      // out.
      self.cachedContentPosition = [NSNumber numberWithFloat:position];
    }
  }
}

- (void)updatePrimaryMenuItem:(BookmarkMenuItem*)menuItem
                     animated:(BOOL)animated {
  if (![self.view superview])
    return;

  [super updatePrimaryMenuItem:menuItem animated:animated];

  [self moveMenuAndPrimaryViewToAdequateParent];

  // [self.view sendSubviewToBack:primaryView];
  [self refreshFrameOfPrimaryView];

  self.navigationBar.hidden = NO;
  [self updateNavigationBarAnimated:animated
                        orientation:GetInterfaceOrientation()];
  [self updateEditBarShadow];
}

- (CGRect)editingBarFrame {
  return CGRectInset(self.navigationBar.frame, 24.0, 0);
}

- (void)showEditingBarAnimated:(BOOL)animated {
  CGRect endFrame = [self editingBarFrame];
  if (self.editingBar.hidden) {
    CGRect startFrame = endFrame;
    startFrame.origin.y = -CGRectGetHeight(startFrame);
    self.editingBar.frame = startFrame;
  }
  self.editingBar.hidden = NO;
  [UIView animateWithDuration:animated ? 0.2 : 0
      delay:0
      options:UIViewAnimationOptionBeginFromCurrentState
      animations:^{
        self.editingBar.alpha = 1;
        self.editingBar.frame = endFrame;
      }
      completion:^(BOOL finished) {
        if (finished)
          self.navigationBar.hidden = YES;
      }];
}

- (void)hideEditingBarAnimated:(BOOL)animated {
  CGRect frame = [self editingBarFrame];
  if (!self.editingBar.hidden) {
    frame.origin.y = -CGRectGetHeight(frame);
  }
  self.navigationBar.hidden = NO;
  [UIView animateWithDuration:animated ? 0.2 : 0
      delay:0
      options:UIViewAnimationOptionBeginFromCurrentState
      animations:^{
        self.editingBar.alpha = 0;
        self.editingBar.frame = frame;
      }
      completion:^(BOOL finished) {
        if (finished)
          self.editingBar.hidden = YES;
      }];
}

- (void)navigateToBookmarkURL:(const GURL&)url {
  [self cachePosition];
  // Before passing the URL to the block, make sure the block has a copy of the
  // URL and not just a reference.
  const GURL localUrl(url);
  dispatch_async(dispatch_get_main_queue(), ^{
    [self loadURL:localUrl];
  });
}

- (ActionSheetCoordinator*)createActionSheetCoordinatorOnView:(UIView*)view {
  return [[ActionSheetCoordinator alloc]
      initWithBaseViewController:self.view.window.rootViewController
                           title:nil
                         message:nil
                            rect:view.bounds
                            view:view];
}

#pragma mark - private methods

- (void)loadURL:(const GURL&)url {
  if (url == GURL() || url.SchemeIs(url::kJavaScriptScheme))
    return;

  new_tab_page_uma::RecordAction(self.browserState,
                                 new_tab_page_uma::ACTION_OPENED_BOOKMARK);
  base::RecordAction(
      base::UserMetricsAction("MobileBookmarkManagerEntryOpened"));
  [self.loader loadURL:url
               referrer:web::Referrer()
             transition:ui::PAGE_TRANSITION_AUTO_BOOKMARK
      rendererInitiated:NO];
}

- (BOOL)shouldPresentMenuInSlideInPanel {
  return IsCompactTablet();
}

- (CGFloat)primaryViewLeadingMargin {
  if ([self shouldPresentMenuInSlideInPanel])
    return 0;
  return [self menuWidth];
}

- (void)moveMenuAndPrimaryViewToAdequateParent {
  // Remove the menuView, panelView, and primaryView from the view hierarchy.
  if ([self.menuView superview])
    [self.menuView removeFromSuperview];
  if ([self.panelView superview])
    [self.panelView removeFromSuperview];
  UIView* primaryView = [self primaryView];
  if ([primaryView superview])
    [primaryView removeFromSuperview];

  if ([self shouldPresentMenuInSlideInPanel]) {
    // Add the panelView to the view hierarchy.
    [self.view addSubview:self.panelView];
    CGSize size = self.view.bounds.size;
    CGFloat navBarHeight = CGRectGetHeight([self navigationBarFrame]);
    LayoutRect panelLayout = LayoutRectMake(
        0, size.width, navBarHeight, size.width, size.height - navBarHeight);

    // Initialize the panelView with the menuView and the primaryView.
    [self.panelView setFrame:LayoutRectGetRect(panelLayout)];
    [self.panelView.menuView addSubview:self.menuView];
    self.menuView.frame = self.panelView.menuView.bounds;
    [self.panelView.contentView addSubview:primaryView];
  } else {
    [self.view addSubview:self.menuView];
    [self.view addSubview:primaryView];
  }

  // Make sure the navigation bar is the frontmost subview.
  [self.view bringSubviewToFront:self.navigationBar];
}

- (void)refreshFrameOfPrimaryView {
  [self primaryView].frame = [self frameForPrimaryView];
}

- (CGRect)frameForPrimaryView {
  CGFloat topInset = 0;
  if (!IsCompactTablet())
    topInset = CGRectGetHeight([self navigationBarFrame]);

  CGFloat leadingMargin = [self primaryViewLeadingMargin];
  CGSize size = self.view.bounds.size;
  LayoutRect primaryViewLayout =
      LayoutRectMake(leadingMargin, size.width, topInset,
                     size.width - leadingMargin, size.height - topInset);
  return LayoutRectGetRect(primaryViewLayout);
}

#pragma mark - BookmarkMenuViewDelegate

- (void)bookmarkMenuView:(BookmarkMenuView*)view
        selectedMenuItem:(BookmarkMenuItem*)menuItem {
  BOOL menuItemChanged = ![[self primaryMenuItem] isEqual:menuItem];
  [self toggleMenuAnimated];
  if (menuItemChanged) {
    [self setEditing:NO animated:YES];
    [self updatePrimaryMenuItem:menuItem animated:YES];
  }
}

- (void)toggleMenuAnimated {
  if ([self.panelView userDrivenAnimationInProgress])
    return;

  if (self.panelView.showingMenu) {
    [self.panelView hideMenuAnimated:YES];
  } else {
    [self.panelView showMenuAnimated:YES];
  }
}

#pragma mark - Navigation bar

- (CGRect)navigationBarFrame {
  return CGRectMake(0, 0, CGRectGetWidth(self.view.bounds),
                    [BookmarkNavigationBar expectedContentViewHeight] +
                        kNavigationBarTopMargin);
}

- (void)updateNavigationBarAnimated:(BOOL)animated
                        orientation:(UIInterfaceOrientation)orientation {
  CGFloat duration = animated ? bookmark_utils_ios::menuAnimationDuration : 0;
  [self updateNavigationBarWithDuration:duration orientation:orientation];
}

- (void)updateNavigationBarWithDuration:(CGFloat)duration
                            orientation:(UIInterfaceOrientation)orientation {
  [self.navigationBar setTitle:[self.primaryMenuItem titleForNavigationBar]];
  if ([self shouldShowEditButton])
    [self.navigationBar showEditButtonWithAnimationDuration:duration];
  else
    [self.navigationBar hideEditButtonWithAnimationDuration:duration];

  if ([self shouldShowBackButtonOnNavigationBar])
    [self.navigationBar showBackButtonInsteadOfMenuButton:duration];
  else
    [self.navigationBar showMenuButtonInsteadOfBackButton:duration];
}

- (BOOL)shouldShowEditButton {
  if (self.primaryMenuItem.type != bookmarks::MenuItemFolder)
    return NO;
  // The type is MenuItemFolder, so it is safe to access |folder|.
  return !self.bookmarks->is_permanent_node(self.primaryMenuItem.folder);
}

#pragma mark - NewTabPagePanelProtocol

- (void)reload {
}

- (void)wasShown {
  [self.folderView wasShown];
}

- (void)wasHidden {
  [self cachePosition];
  [self.folderView wasHidden];
}

- (void)dismissModals {
  [self.actionSheetCoordinator stop];
  self.actionSheetCoordinator = nil;
  [self.editViewController dismiss];
}

- (void)dismissKeyboard {
  // Uses self.view directly instead of going throught self.view to
  // avoid creating the view hierarchy unnecessarily.
  [self.view endEditing:YES];
}

- (void)setScrollsToTop:(BOOL)enabled {
  self.scrollToTop = enabled;
  [[self primaryView] setScrollsToTop:self.scrollToTop];
}

- (CGFloat)alphaForBottomShadow {
  return 0;
}

@end