// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/loader/layered_resource_handler.h"

#include <utility>

#include "base/logging.h"
#include "content/browser/loader/resource_controller.h"

#include "content/browser/loader/resource_request_info_impl.h"

namespace content {

LayeredResourceHandler::LayeredResourceHandler(
    net::URLRequest* request,
    std::unique_ptr<ResourceHandler> next_handler)
    : ResourceHandler(request), next_handler_(std::move(next_handler)) {}

LayeredResourceHandler::~LayeredResourceHandler() {
}

void LayeredResourceHandler::SetDelegate(Delegate* delegate) {
  ResourceHandler::SetDelegate(delegate);
  next_handler_->SetDelegate(delegate);
}

void LayeredResourceHandler::OnRequestRedirected(
    const net::RedirectInfo& redirect_info,
    ResourceResponse* response,
    std::unique_ptr<ResourceController> controller) {
  DCHECK(next_handler_.get());
  next_handler_->OnRequestRedirected(redirect_info, response,
                                     std::move(controller));
}

void LayeredResourceHandler::OnResponseStarted(
    ResourceResponse* response,
    std::unique_ptr<ResourceController> controller) {
  //return OnResponseStarted(response, std::move(controller), false, false);
  // Vivaldi specific save info override.
  ResourceRequestInfoImpl* info = GetRequestInfo();
  return OnResponseStarted(response, std::move(controller),
                           info != nullptr && info->open_when_downloaded(),
                           info != nullptr && info->ask_for_save_target());
}

void LayeredResourceHandler::OnResponseStarted(
    ResourceResponse* response,
    std::unique_ptr<ResourceController> controller,
    bool open_when_done,
    bool ask_for_target) {
  /*
  DCHECK(next_handler_.get());
  next_handler_->OnResponseStarted(response, std::move(controller),
                                   open_when_done, ask_for_target);
                                          */
  return OnResponseStarted(response, std::move(controller));
}

void LayeredResourceHandler::OnWillStart(
    const GURL& url,
    std::unique_ptr<ResourceController> controller) {
  DCHECK(next_handler_.get());
  next_handler_->OnWillStart(url, std::move(controller));
}

void LayeredResourceHandler::OnWillRead(
    scoped_refptr<net::IOBuffer>* buf,
    int* buf_size,
    std::unique_ptr<ResourceController> controller) {
  DCHECK(next_handler_.get());
  return next_handler_->OnWillRead(buf, buf_size, std::move(controller));
}

void LayeredResourceHandler::OnReadCompleted(
    int bytes_read,
    std::unique_ptr<ResourceController> controller) {
  DCHECK(next_handler_.get());
  next_handler_->OnReadCompleted(bytes_read, std::move(controller));
}

void LayeredResourceHandler::OnResponseCompleted(
    const net::URLRequestStatus& status,
    std::unique_ptr<ResourceController> controller) {
  DCHECK(next_handler_.get());
  next_handler_->OnResponseCompleted(status, std::move(controller));
}

void LayeredResourceHandler::OnDataDownloaded(int bytes_downloaded) {
  DCHECK(next_handler_.get());
  next_handler_->OnDataDownloaded(bytes_downloaded);
}

}  // namespace content
