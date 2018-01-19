// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/predictors/preconnect_manager.h"

#include "base/format_macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/threading/thread_task_runner_handle.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "net/url_request/url_request_test_util.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::_;
using testing::DoAll;
using testing::Mock;
using testing::Return;
using testing::SaveArg;
using testing::StrictMock;

namespace predictors {

class MockPreconnectManagerDelegate
    : public PreconnectManager::Delegate,
      public base::SupportsWeakPtr<MockPreconnectManagerDelegate> {
 public:
  MOCK_METHOD1(PreconnectFinished, void(const GURL&));
};

class MockPreconnectManager : public PreconnectManager {
 public:
  MockPreconnectManager(
      base::WeakPtr<Delegate> delegate,
      scoped_refptr<net::URLRequestContextGetter> context_getter);

  MOCK_CONST_METHOD2(PreconnectUrl,
                     void(const GURL& url,
                          const GURL& first_party_for_cookies));
  MOCK_CONST_METHOD2(PreresolveUrl,
                     int(const GURL& url,
                         const net::CompletionCallback& callback));
};

MockPreconnectManager::MockPreconnectManager(
    base::WeakPtr<Delegate> delegate,
    scoped_refptr<net::URLRequestContextGetter> context_getter)
    : PreconnectManager(delegate, context_getter) {}

class PreconnectManagerTest : public testing::Test {
 public:
  PreconnectManagerTest();
  ~PreconnectManagerTest() override;

 protected:
  content::TestBrowserThreadBundle thread_bundle_;
  std::unique_ptr<StrictMock<MockPreconnectManagerDelegate>> mock_delegate_;
  scoped_refptr<net::URLRequestContextGetter> context_getter_;
  std::unique_ptr<StrictMock<MockPreconnectManager>> preconnect_manager_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PreconnectManagerTest);
};

PreconnectManagerTest::PreconnectManagerTest()
    : mock_delegate_(
          base::MakeUnique<StrictMock<MockPreconnectManagerDelegate>>()),
      context_getter_(new net::TestURLRequestContextGetter(
          base::ThreadTaskRunnerHandle::Get())),
      preconnect_manager_(base::MakeUnique<StrictMock<MockPreconnectManager>>(
          mock_delegate_->AsWeakPtr(),
          context_getter_)) {}

PreconnectManagerTest::~PreconnectManagerTest() = default;

TEST_F(PreconnectManagerTest, TestStartOneUrlPreresolve) {
  GURL main_frame_url("http://google.com");
  GURL url_to_preresolve("http://cdn.google.com");

  EXPECT_CALL(*preconnect_manager_, PreresolveUrl(url_to_preresolve, _))
      .WillOnce(Return(net::OK));
  EXPECT_CALL(*mock_delegate_, PreconnectFinished(main_frame_url));
  preconnect_manager_->Start(main_frame_url, std::vector<GURL>(),
                             {url_to_preresolve});
  // Wait for PreconnectFinished task posted to the UI thread.
  base::RunLoop().RunUntilIdle();
}

TEST_F(PreconnectManagerTest, TestStartOneUrlPreconnect) {
  GURL main_frame_url("http://google.com");
  GURL url_to_preconnect("http://cdn.google.com");

  EXPECT_CALL(*preconnect_manager_, PreresolveUrl(url_to_preconnect, _))
      .WillOnce(Return(net::OK));
  EXPECT_CALL(*preconnect_manager_,
              PreconnectUrl(url_to_preconnect, main_frame_url));
  EXPECT_CALL(*mock_delegate_, PreconnectFinished(main_frame_url));
  preconnect_manager_->Start(main_frame_url, {url_to_preconnect},
                             std::vector<GURL>());
  base::RunLoop().RunUntilIdle();
}

TEST_F(PreconnectManagerTest, TestStopOneUrlBeforePreconnect) {
  GURL main_frame_url("http://google.com");
  GURL url_to_preconnect("http://cdn.google.com");
  net::CompletionCallback callback;

  // Preconnect job isn't started before preresolve is completed asynchronously.
  EXPECT_CALL(*preconnect_manager_, PreresolveUrl(url_to_preconnect, _))
      .WillOnce(DoAll(SaveArg<1>(&callback), Return(net::ERR_IO_PENDING)));
  preconnect_manager_->Start(main_frame_url, {url_to_preconnect},
                             std::vector<GURL>());

  // Stop all jobs for |main_frame_url| before we get the callback.
  preconnect_manager_->Stop(main_frame_url);
  EXPECT_CALL(*mock_delegate_, PreconnectFinished(main_frame_url));
  callback.Run(net::OK);
  base::RunLoop().RunUntilIdle();
}

TEST_F(PreconnectManagerTest, TestGetCallbackAfterDestruction) {
  GURL main_frame_url("http://google.com");
  GURL url_to_preconnect("http://cdn.google.com");
  net::CompletionCallback callback;
  EXPECT_CALL(*preconnect_manager_, PreresolveUrl(url_to_preconnect, _))
      .WillOnce(DoAll(SaveArg<1>(&callback), Return(net::ERR_IO_PENDING)));
  preconnect_manager_->Start(main_frame_url, {url_to_preconnect},
                             std::vector<GURL>());

  // Callback may outlive PreconnectManager but it shouldn't cause a crash.
  preconnect_manager_ = nullptr;
  callback.Run(net::OK);
  base::RunLoop().RunUntilIdle();
}

TEST_F(PreconnectManagerTest, TestUnqueuedPreresolvesCanceled) {
  GURL main_frame_url("http://google.com");
  size_t count = PreconnectManager::kMaxInflightPreresolves;
  std::vector<GURL> urls_to_preconnect;
  // Allocate the space for callbacks at once because we need stable pointers.
  std::vector<net::CompletionCallback> callbacks(count);
  for (size_t i = 0; i < count; ++i) {
    // Exactly PreconnectManager::kMaxInflightPreresolves should be preresolved.
    urls_to_preconnect.emplace_back(
        base::StringPrintf("http://cdn%" PRIuS ".google.com", i));
    EXPECT_CALL(*preconnect_manager_,
                PreresolveUrl(urls_to_preconnect.back(), _))
        .WillOnce(
            DoAll(SaveArg<1>(&callbacks[i]), Return(net::ERR_IO_PENDING)));
  }
  // This url shouldn't be preresolved.
  urls_to_preconnect.emplace_back("http://no.preresolve.com");
  preconnect_manager_->Start(main_frame_url, urls_to_preconnect,
                             std::vector<GURL>());

  preconnect_manager_->Stop(main_frame_url);
  EXPECT_CALL(*mock_delegate_, PreconnectFinished(main_frame_url));
  for (auto& callback : callbacks)
    callback.Run(net::OK);
  base::RunLoop().RunUntilIdle();
}

TEST_F(PreconnectManagerTest, TestTwoConcurrentMainFrameUrls) {
  GURL main_frame_url1("http://google.com");
  GURL url_to_preconnect1("http://cdn.google.com");
  net::CompletionCallback callback1;
  GURL main_frame_url2("http://facebook.com");
  GURL url_to_preconnect2("http://cdn.facebook.com");
  net::CompletionCallback callback2;

  EXPECT_CALL(*preconnect_manager_, PreresolveUrl(url_to_preconnect1, _))
      .WillOnce(DoAll(SaveArg<1>(&callback1), Return(net::ERR_IO_PENDING)));
  EXPECT_CALL(*preconnect_manager_, PreresolveUrl(url_to_preconnect2, _))
      .WillOnce(DoAll(SaveArg<1>(&callback2), Return(net::ERR_IO_PENDING)));
  preconnect_manager_->Start(main_frame_url1, {url_to_preconnect1},
                             std::vector<GURL>());
  preconnect_manager_->Start(main_frame_url2, {url_to_preconnect2},
                             std::vector<GURL>());
  // Check that the first url didn't block the second one.
  Mock::VerifyAndClearExpectations(preconnect_manager_.get());

  preconnect_manager_->Stop(main_frame_url2);
  // Stopping the second url shouldn't stop the first one.
  EXPECT_CALL(*preconnect_manager_,
              PreconnectUrl(url_to_preconnect1, main_frame_url1));
  EXPECT_CALL(*mock_delegate_, PreconnectFinished(main_frame_url1));
  callback1.Run(net::OK);
  // No preconnect for the second url.
  EXPECT_CALL(*mock_delegate_, PreconnectFinished(main_frame_url2));
  callback2.Run(net::OK);
  base::RunLoop().RunUntilIdle();
}

}  // namespace predictors