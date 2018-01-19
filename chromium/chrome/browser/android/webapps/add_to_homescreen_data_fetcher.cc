// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/webapps/add_to_homescreen_data_fetcher.h"

#include <vector>

#include "base/bind.h"
#include "base/location.h"
#include "base/metrics/user_metrics.h"
#include "base/task_scheduler/post_task.h"
#include "chrome/browser/android/shortcut_helper.h"
#include "chrome/browser/android/webapk/webapk_web_manifest_checker.h"
#include "chrome/browser/favicon/favicon_service_factory.h"
#include "chrome/browser/installable/installable_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/render_messages.h"
#include "chrome/common/web_application_info.h"
#include "components/dom_distiller/core/url_utils.h"
#include "components/favicon/core/favicon_service.h"
#include "components/favicon_base/favicon_types.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/manifest_icon_selector.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/manifest.h"
#include "third_party/WebKit/public/platform/modules/screen_orientation/WebScreenOrientationLockType.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/favicon_size.h"
#include "url/gurl.h"

namespace {

// Looks up the original, online URL of the site requested.  The URL from the
// WebContents may be a distilled article which is not appropriate for a home
// screen shortcut.
GURL GetShortcutUrl(content::BrowserContext* browser_context,
                    const GURL& actual_url) {
  return dom_distiller::url_utils::GetOriginalUrlFromDistillerUrl(actual_url);
}

InstallableParams ParamsToPerformManifestAndIconFetch(
    int ideal_icon_size_in_px,
    int minimum_icon_size_in_px,
    int badge_size_in_px,
    bool check_webapk_compatibility) {
  InstallableParams params;
  params.ideal_primary_icon_size_in_px = ideal_icon_size_in_px;
  params.minimum_primary_icon_size_in_px = minimum_icon_size_in_px;
  params.fetch_valid_primary_icon = true;
  if (check_webapk_compatibility) {
    params.fetch_valid_badge_icon = true;
    params.ideal_badge_icon_size_in_px = badge_size_in_px;
    params.minimum_badge_icon_size_in_px = badge_size_in_px;
  }
  return params;
}

InstallableParams ParamsToPerformInstallableCheck(
    bool check_webapk_compatibility) {
  InstallableParams params;
  params.check_installable = check_webapk_compatibility;
  return params;
}

// Creates a launcher icon from |icon|. |start_url| is used to generate the icon
// if |icon| is empty or is not large enough. Returns a std::pair with:
// - the generated icon
// - whether |icon| was used in generating the launcher icon
std::pair<SkBitmap, bool> CreateLauncherIconInBackground(const GURL& start_url,
                                                         const SkBitmap& icon) {
  base::ThreadRestrictions::AssertIOAllowed();

  bool is_generated = false;
  SkBitmap primary_icon = ShortcutHelper::FinalizeLauncherIconInBackground(
      icon, start_url, &is_generated);
  return std::make_pair(primary_icon, is_generated);
}

// Creates a launcher icon from |bitmap_result|. |start_url| is used to
// generate the icon if there is no bitmap in |bitmap_result| or the bitmap is
// not large enough. Returns a std::pair with:
// - the generated icon
// - whether the bitmap in |bitmap_result| was used in generating the launcher
//   icon
std::pair<SkBitmap, bool> CreateLauncherIconFromFaviconInBackground(
    const GURL& start_url,
    const favicon_base::FaviconRawBitmapResult& bitmap_result) {
  base::ThreadRestrictions::AssertIOAllowed();

  SkBitmap decoded;
  if (bitmap_result.is_valid()) {
    gfx::PNGCodec::Decode(bitmap_result.bitmap_data->front(),
                          bitmap_result.bitmap_data->size(), &decoded);
  }
  return CreateLauncherIconInBackground(start_url, decoded);
}

}  // namespace

AddToHomescreenDataFetcher::AddToHomescreenDataFetcher(
    content::WebContents* web_contents,
    int ideal_icon_size_in_px,
    int minimum_icon_size_in_px,
    int ideal_splash_image_size_in_px,
    int minimum_splash_image_size_in_px,
    int badge_size_in_px,
    int data_timeout_ms,
    bool check_webapk_compatibility,
    Observer* observer)
    : content::WebContentsObserver(web_contents),
      installable_manager_(InstallableManager::FromWebContents(web_contents)),
      observer_(observer),
      shortcut_info_(GetShortcutUrl(web_contents->GetBrowserContext(),
                                    web_contents->GetLastCommittedURL())),
      ideal_icon_size_in_px_(ideal_icon_size_in_px),
      minimum_icon_size_in_px_(minimum_icon_size_in_px),
      ideal_splash_image_size_in_px_(ideal_splash_image_size_in_px),
      minimum_splash_image_size_in_px_(minimum_splash_image_size_in_px),
      badge_size_in_px_(badge_size_in_px),
      data_timeout_ms_(data_timeout_ms),
      check_webapk_compatibility_(check_webapk_compatibility),
      is_waiting_for_web_application_info_(true),
      weak_ptr_factory_(this) {
  DCHECK(minimum_icon_size_in_px <= ideal_icon_size_in_px);
  DCHECK(minimum_splash_image_size_in_px <= ideal_splash_image_size_in_px);

  // Send a message to the renderer to retrieve information about the page.
  content::RenderFrameHost* main_frame = web_contents->GetMainFrame();
  main_frame->Send(
      new ChromeFrameMsg_GetWebApplicationInfo(main_frame->GetRoutingID()));
}

AddToHomescreenDataFetcher::~AddToHomescreenDataFetcher() {}

void AddToHomescreenDataFetcher::OnDidGetWebApplicationInfo(
    const WebApplicationInfo& received_web_app_info) {
  is_waiting_for_web_application_info_ = false;
  if (!web_contents())
    return;

  // Sanitize received_web_app_info.
  WebApplicationInfo web_app_info = received_web_app_info;
  web_app_info.title =
      web_app_info.title.substr(0, chrome::kMaxMetaTagAttributeLength);

  // Simply set the user-editable title to be the page's title
  shortcut_info_.user_title = web_app_info.title.empty()
                                  ? web_contents()->GetTitle()
                                  : web_app_info.title;
  shortcut_info_.short_name = shortcut_info_.user_title;
  shortcut_info_.name = shortcut_info_.user_title;

  if (web_app_info.mobile_capable == WebApplicationInfo::MOBILE_CAPABLE ||
      web_app_info.mobile_capable == WebApplicationInfo::MOBILE_CAPABLE_APPLE) {
    shortcut_info_.display = blink::kWebDisplayModeStandalone;
    shortcut_info_.UpdateSource(
        ShortcutInfo::SOURCE_ADD_TO_HOMESCREEN_STANDALONE);
  }

  // Record what type of shortcut was added by the user.
  switch (web_app_info.mobile_capable) {
    case WebApplicationInfo::MOBILE_CAPABLE:
      base::RecordAction(
          base::UserMetricsAction("webapps.AddShortcut.AppShortcut"));
      break;
    case WebApplicationInfo::MOBILE_CAPABLE_APPLE:
      base::RecordAction(
          base::UserMetricsAction("webapps.AddShortcut.AppShortcutApple"));
      break;
    case WebApplicationInfo::MOBILE_CAPABLE_UNSPECIFIED:
      base::RecordAction(
          base::UserMetricsAction("webapps.AddShortcut.Bookmark"));
      break;
  }

  // Kick off a timeout for downloading data. If we haven't finished within the
  // timeout, fall back to using a dynamically-generated launcher icon.
  data_timeout_timer_.Start(
      FROM_HERE, base::TimeDelta::FromMilliseconds(data_timeout_ms_),
      base::Bind(&AddToHomescreenDataFetcher::OnDataTimedout,
                 weak_ptr_factory_.GetWeakPtr()));

  installable_manager_->GetData(
      ParamsToPerformManifestAndIconFetch(
          ideal_icon_size_in_px_, minimum_icon_size_in_px_, badge_size_in_px_,
          check_webapk_compatibility_),
      base::Bind(&AddToHomescreenDataFetcher::OnDidGetManifestAndIcons,
                 weak_ptr_factory_.GetWeakPtr()));
}

bool AddToHomescreenDataFetcher::OnMessageReceived(
    const IPC::Message& message,
    content::RenderFrameHost* sender) {
  if (!is_waiting_for_web_application_info_)
    return false;

  bool handled = true;

  IPC_BEGIN_MESSAGE_MAP(AddToHomescreenDataFetcher, message)
    IPC_MESSAGE_HANDLER(ChromeFrameHostMsg_DidGetWebApplicationInfo,
                        OnDidGetWebApplicationInfo)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()

  return handled;
}

void AddToHomescreenDataFetcher::OnDataTimedout() {
  weak_ptr_factory_.InvalidateWeakPtrs();

  if (!web_contents())
    return;

  if (check_webapk_compatibility_)
    observer_->OnDidDetermineWebApkCompatibility(false);
  observer_->OnUserTitleAvailable(shortcut_info_.user_title);

  CreateLauncherIcon(raw_primary_icon_);
}

void AddToHomescreenDataFetcher::OnDidGetManifestAndIcons(
    const InstallableData& data) {
  if (!web_contents())
    return;

  if (!data.manifest.IsEmpty()) {
    base::RecordAction(base::UserMetricsAction("webapps.AddShortcut.Manifest"));
    shortcut_info_.UpdateFromManifest(data.manifest);
    shortcut_info_.manifest_url = data.manifest_url;
  }

  // Do this after updating from the manifest for the case where a site has
  // a manifest with name and standalone specified, but no icons.
  if (data.manifest.IsEmpty() || !data.primary_icon) {
    if (check_webapk_compatibility_)
      observer_->OnDidDetermineWebApkCompatibility(false);
    observer_->OnUserTitleAvailable(shortcut_info_.user_title);
    data_timeout_timer_.Stop();
    FetchFavicon();
    return;
  }

  raw_primary_icon_ = *data.primary_icon;
  shortcut_info_.best_primary_icon_url = data.primary_icon_url;

  // Save the splash screen URL for the later download.
  shortcut_info_.splash_image_url =
      content::ManifestIconSelector::FindBestMatchingIcon(
          data.manifest.icons, ideal_splash_image_size_in_px_,
          minimum_splash_image_size_in_px_,
          content::Manifest::Icon::IconPurpose::ANY);
  shortcut_info_.ideal_splash_image_size_in_px = ideal_splash_image_size_in_px_;
  shortcut_info_.minimum_splash_image_size_in_px =
      minimum_splash_image_size_in_px_;
  if (data.badge_icon) {
    shortcut_info_.best_badge_icon_url = data.badge_icon_url;
    badge_icon_ = *data.badge_icon;
  }

  installable_manager_->GetData(
      ParamsToPerformInstallableCheck(check_webapk_compatibility_),
      base::Bind(&AddToHomescreenDataFetcher::OnDidPerformInstallableCheck,
                 weak_ptr_factory_.GetWeakPtr()));
}

void AddToHomescreenDataFetcher::OnDidPerformInstallableCheck(
    const InstallableData& data) {
  data_timeout_timer_.Stop();

  if (!web_contents())
    return;

  bool webapk_compatible = false;
  if (check_webapk_compatibility_) {
    webapk_compatible =
        (data.error_code == NO_ERROR_DETECTED && data.is_installable &&
         AreWebManifestUrlsWebApkCompatible(data.manifest));
    observer_->OnDidDetermineWebApkCompatibility(webapk_compatible);
  }

  observer_->OnUserTitleAvailable(shortcut_info_.user_title);
  if (webapk_compatible) {
    shortcut_info_.UpdateSource(ShortcutInfo::SOURCE_ADD_TO_HOMESCREEN_PWA);
    NotifyObserver(std::make_pair(raw_primary_icon_, false /* is_generated */));
  } else {
    CreateLauncherIcon(raw_primary_icon_);
  }
}

void AddToHomescreenDataFetcher::FetchFavicon() {
  if (!web_contents())
    return;

  // Grab the best, largest icon we can find to represent this bookmark.
  // TODO(dfalcantara): Try combining with the new BookmarksHandler once its
  //                    rewrite is further along.
  std::vector<int> icon_types{
      favicon_base::WEB_MANIFEST_ICON, favicon_base::FAVICON,
      favicon_base::TOUCH_PRECOMPOSED_ICON | favicon_base::TOUCH_ICON};

  favicon::FaviconService* favicon_service =
      FaviconServiceFactory::GetForProfile(
          Profile::FromBrowserContext(web_contents()->GetBrowserContext()),
          ServiceAccessType::EXPLICIT_ACCESS);

  // Using favicon if its size is not smaller than platform required size,
  // otherwise using the largest icon among all avaliable icons.
  int threshold_to_get_any_largest_icon = ideal_icon_size_in_px_ - 1;
  favicon_service->GetLargestRawFaviconForPageURL(
      shortcut_info_.url, icon_types, threshold_to_get_any_largest_icon,
      base::Bind(&AddToHomescreenDataFetcher::OnFaviconFetched,
                 weak_ptr_factory_.GetWeakPtr()),
      &favicon_task_tracker_);
}

void AddToHomescreenDataFetcher::OnFaviconFetched(
    const favicon_base::FaviconRawBitmapResult& bitmap_result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!web_contents())
    return;

  shortcut_info_.best_primary_icon_url = bitmap_result.icon_url;

  // The user is waiting for the icon to be processed before they can proceed
  // with add to homescreen. But if we shut down, there's no point starting the
  // image processing. Use USER_VISIBLE with MayBlock and SKIP_ON_SHUTDOWN.
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&CreateLauncherIconFromFaviconInBackground,
                     shortcut_info_.url, bitmap_result),
      base::BindOnce(&AddToHomescreenDataFetcher::NotifyObserver,
                     weak_ptr_factory_.GetWeakPtr()));
}

void AddToHomescreenDataFetcher::CreateLauncherIcon(const SkBitmap& icon) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // The user is waiting for the icon to be processed before they can proceed
  // with add to homescreen. But if we shut down, there's no point starting the
  // image processing. Use USER_VISIBLE with MayBlock and SKIP_ON_SHUTDOWN.
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&CreateLauncherIconInBackground, shortcut_info_.url, icon),
      base::BindOnce(&AddToHomescreenDataFetcher::NotifyObserver,
                     weak_ptr_factory_.GetWeakPtr()));
}

void AddToHomescreenDataFetcher::NotifyObserver(
    const std::pair<SkBitmap, bool /*is_generated*/>& primary_icon) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!web_contents())
    return;

  primary_icon_ = primary_icon.first;
  if (primary_icon.second)
    shortcut_info_.best_primary_icon_url = GURL();
  observer_->OnDataAvailable(shortcut_info_, primary_icon_, badge_icon_);
}