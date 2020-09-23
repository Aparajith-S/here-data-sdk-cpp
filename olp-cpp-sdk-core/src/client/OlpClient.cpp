/*
 * Copyright (C) 2019-2020 HERE Europe B.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 * License-Filename: LICENSE
 */

#include "olp/core/client/OlpClient.h"

#include <cctype>
#include <chrono>
#include <future>
#include <sstream>
#include <thread>

#include "PendingUrlRequests.h"
#include "olp/core/client/Condition.h"
#include "olp/core/client/ErrorCode.h"
#include "olp/core/http/HttpStatusCode.h"
#include "olp/core/http/NetworkConstants.h"
#include "olp/core/logging/Log.h"
#include "olp/core/utils/Url.h"

namespace {
constexpr auto kLogTag = "OlpClient";
constexpr auto kApiKeyParam = "apiKey=";

struct RequestSettings {
  explicit RequestSettings(const int initial_backdown_period, const int timeout)
      : current_backdown_period{initial_backdown_period},
        max_wait_time{timeout} {}

  int current_try{0};
  std::chrono::milliseconds accumulated_wait_time{0};
  std::chrono::milliseconds current_backdown_period{0};
  const std::chrono::milliseconds max_wait_time{0};
};
}  // namespace

namespace olp {
namespace client {

namespace {

using PendingUrlRequestsPtr = std::shared_ptr<PendingUrlRequests>;
using PendingUrlRequestPtr = PendingUrlRequests::PendingUrlRequestPtr;
using NetworkRequestPtr = std::shared_ptr<http::NetworkRequest>;
using RequestSettingsPtr = std::shared_ptr<RequestSettings>;

using NetworkCallbackType = std::function<void(const http::RequestId request_id,
                                               HttpResponse response)>;

static const auto kCancelledErrorResponse =
    http::NetworkResponse()
        .WithStatus(static_cast<int>(http::ErrorCode::CANCELLED_ERROR))
        .WithError("Operation Cancelled.");

static const auto kTimeoutErrorResponse =
    http::NetworkResponse()
        .WithStatus(static_cast<int>(http::ErrorCode::TIMEOUT_ERROR))
        .WithError("Network request timed out.");

NetworkStatistics GetStatistics(const http::NetworkResponse& response) {
  return NetworkStatistics(response.GetBytesUploaded(),
                           response.GetBytesDownloaded());
}

HttpResponse ToHttpResponse(const http::NetworkResponse& response) {
  HttpResponse http_response{response.GetStatus(), response.GetError()};
  http_response.SetNetworkStatistics(GetStatistics(response));
  return http_response;
}

HttpResponse ToHttpResponse(const http::SendOutcome& outcome) {
  return {static_cast<int>(outcome.GetErrorCode()),
          http::ErrorCodeToString(outcome.GetErrorCode())};
}

bool StatusSuccess(int status) {
  return status >= 0 && status < http::HttpStatusCode::BAD_REQUEST;
}

bool CaseInsensitiveCompare(const std::string& str1, const std::string& str2) {
  return (str1.size() == str2.size()) &&
         std::equal(str1.begin(), str1.end(), str2.begin(),
                    [](const char& c1, const char& c2) {
                      return (c1 == c2) ||
                             (std::tolower(c1) == std::tolower(c2));
                    });
}

RequestSettingsPtr GetRequestSettings(const RetrySettings& retry_settings) {
  return std::make_shared<RequestSettings>(
      retry_settings.initial_backdown_period, retry_settings.timeout);
}

bool CheckRetryCondition(const RequestSettings& request,
                         const RetrySettings& settings,
                         HttpResponse& response) {
  return (request.current_try > settings.max_attempts ||
          !settings.retry_condition(response) ||
          request.accumulated_wait_time >= request.max_wait_time);
}

std::chrono::milliseconds CalculateNextWaitTime(const RetrySettings& settings,
                                                size_t current_try) {
  return settings.backdown_strategy
             ? settings.backdown_strategy(
                   std::chrono::milliseconds(settings.initial_backdown_period),
                   current_try)
             : std::chrono::milliseconds::zero();
}

void ExecuteSingleRequest(const std::shared_ptr<http::Network>& network,
                          const PendingUrlRequestPtr& pending_request,
                          const http::NetworkRequest& request,
                          const NetworkCallbackType& callback) {
  auto response_body = std::make_shared<std::stringstream>();
  auto headers = std::make_shared<http::Headers>();

  pending_request->ExecuteOrCancelled([&](http::RequestId& id) {
    OLP_SDK_LOG_INFO_F(kLogTag, "execute_request - ExecuteOrCancelled");

    auto send_outcome = network->Send(
        request, response_body,
        [=](const http::NetworkResponse& response) {
          auto status = response.GetStatus();

          OLP_SDK_LOG_INFO_F(
              kLogTag, "execute_request - network response, status=%d", status);

          if (!StatusSuccess(status)) {
            OLP_SDK_LOG_INFO_F(
                kLogTag, "execute_request - status error, response_body=%u",
                response_body != nullptr);

            response_body->str(
                response.GetError().empty()
                    ? "Error occurred, please check HTTP status code"
                    : response.GetError());
          }

          OLP_SDK_LOG_INFO_F(kLogTag, "execute_request - callback");

          callback(response.GetRequestId(),
                   {status, std::move(*response_body), std::move(*headers)});
        },
        [=](std::string key, std::string value) {
          headers->emplace_back(std::move(key), std::move(value));
        });

    if (!send_outcome.IsSuccessful()) {
      callback(PendingUrlRequest::kInvalidRequestId,
               ToHttpResponse(send_outcome));
      return CancellationToken();
    }

    id = send_outcome.GetRequestId();

    OLP_SDK_LOG_INFO_F(kLogTag,
                       "execute_request - Send successful, request_id=%lu", id);

    return CancellationToken([=] { network->Cancel(id); });
  } /*TODO: Do we need a cancelled callback*/);

  /*
    auto execute_request = [=](const std::shared_ptr<http::Network>& network,
                               PendingUrlRequestPtr& pending_request,
                               const http::NetworkRequest& request,
                               const NetworkCallbackType& callback) {
      pending_request->ExecuteOrCancelled([&](http::RequestId& id) {
        OLP_SDK_LOG_INFO_F(kLogTag, "execute_request - ExecuteOrCancelled");

        auto response_body = std::make_shared<std::stringstream>();
        auto headers = std::make_shared<http::Headers>();

        auto send_outcome = network->Send(
            request, response_body,
            [=](const http::NetworkResponse& response) {
              auto status = response.GetStatus();

              OLP_SDK_LOG_INFO_F(kLogTag,
                                 "execute_request - network response,
    status=%d", status);

              if (!StatusSuccess(status)) {
                OLP_SDK_LOG_INFO_F(
                    kLogTag, "execute_request - status error, response_body=%u",
                    response_body != nullptr);

                response_body->str(
                    response.GetError().empty()
                        ? "Error occurred, please check HTTP status code"
                        : response.GetError());
              }

              OLP_SDK_LOG_INFO_F(kLogTag, "execute_request - callback");

              callback(response.GetRequestId(),
                       {status, std::move(*response_body),
    std::move(*headers)});
            },
            [=](std::string key, std::string value) {
              headers->emplace_back(std::move(key), std::move(value));
            });

        if (!send_outcome.IsSuccessful()) {
          callback(PendingUrlRequest::kInvalidRequestId,
                   ToHttpResponse(send_outcome));
          return CancellationToken();
        }

        OLP_SDK_LOG_INFO_F(kLogTag, "execute_request - Send successful");

        id = send_outcome.GetRequestId();
        return CancellationToken([=] { network->Cancel(id); });
      });
    };
    */
}

NetworkCallbackType GetNewRetryCallback(
    bool merge, const RequestSettingsPtr& settings,
    const RetrySettings& retry_settings,
    const std::shared_ptr<http::Network>& network,
    const PendingUrlRequestsPtr& pending_requests,
    const PendingUrlRequestPtr& pending_request,
    const NetworkRequestPtr& request) {
  return [=](const http::RequestId request_id, HttpResponse response) mutable {
    ++settings->current_try;

    OLP_SDK_LOG_INFO_F(kLogTag, "retry_callback - called, try=%u",
                       settings->current_try);

    if (CheckRetryCondition(*settings, retry_settings, response)) {
      // Response is either successull or retries count/time expired
      if (pending_request->GetRequestId() != request_id) {
        OLP_SDK_LOG_WARNING_F(
            kLogTag,
            "Wrong response received, pending_request=%" PRIu64
            ", request_id=%" PRIu64,
            pending_request->GetRequestId(), request_id);
        return;
      }

      if (merge) {
        OLP_SDK_LOG_INFO_F(kLogTag,
                           "retry_callback - merged request completed");
        const auto& url = request->GetUrl();
        pending_requests->OnRequestCompleted(request_id, url,
                                             std::move(response));
      } else {
        OLP_SDK_LOG_INFO_F(kLogTag,
                           "retry_callback - unmerged request completed");
        pending_request->OnRequestCompleted(std::move(response));
      }
      return;
    }

    const auto start = std::chrono::steady_clock::now();

    // TODO: Do not block thread but instead implement an event queue that will
    // trigger next retry once the time expired!
    const auto actual_wait_time =
        std::min(settings->current_backdown_period,
                 settings->max_wait_time - settings->accumulated_wait_time);
    std::this_thread::sleep_for(actual_wait_time);

    settings->current_backdown_period =
        CalculateNextWaitTime(retry_settings, settings->current_try);

    OLP_SDK_LOG_INFO_F(kLogTag,
                       "retry_callback - retrigger after sleep, "
                       "actual_wait_time=%" PRId64 "ms, slept=%" PRId64 "ms",
                       actual_wait_time.count(),
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count());

    ExecuteSingleRequest(
        network, pending_request, *request,
        GetNewRetryCallback(merge, settings, retry_settings, network,
                            pending_requests, pending_request, request));
  };
}

CancellationToken ExecuteSingleRequest(
    std::weak_ptr<http::Network> weak_network,
    const http::NetworkRequest& request, const NetworkAsyncCallback& callback) {
  auto response_body = std::make_shared<std::stringstream>();
  auto headers = std::make_shared<http::Headers>();
  auto network = weak_network.lock();

  if (!network) {
    callback({static_cast<int>(http::ErrorCode::OFFLINE_ERROR),
              "Network layer offline or missing."});
    return CancellationToken();
  }

  auto send_outcome = network->Send(
      request, response_body,
      [=](const http::NetworkResponse& response) {
        int status = response.GetStatus();
        if (!StatusSuccess(status)) {
          response_body->str(
              response.GetError().empty()
                  ? "Error occurred. Please check HTTP status code"
                  : response.GetError());
        }

        callback({status, std::move(*response_body), std::move(*headers)});
      },
      [headers](std::string key, std::string value) {
        headers->emplace_back(std::move(key), std::move(value));
      });

  if (!send_outcome.IsSuccessful()) {
    callback(ToHttpResponse(send_outcome));
    return CancellationToken();
  }

  auto request_id = send_outcome.GetRequestId();

  return CancellationToken([weak_network, request_id]() {
    auto network = weak_network.lock();
    if (network) {
      network->Cancel(request_id);
    }
  });
}

http::NetworkRequest::HttpVerb GetHttpVerb(const std::string& verb) {
  if (verb.compare("GET") == 0) {
    return http::NetworkRequest::HttpVerb::GET;
  } else if (verb.compare("PUT") == 0) {
    return http::NetworkRequest::HttpVerb::PUT;
  } else if (verb.compare("POST") == 0) {
    return http::NetworkRequest::HttpVerb::POST;
  } else if (verb.compare("DELETE") == 0) {
    return http::NetworkRequest::HttpVerb::DEL;
  } else if (verb.compare("OPTIONS") == 0) {
    return http::NetworkRequest::HttpVerb::OPTIONS;
  }

  return http::NetworkRequest::HttpVerb::GET;
}

HttpResponse SendRequest(const http::NetworkRequest& request,
                         const olp::client::OlpClientSettings& settings,
                         const olp::client::RetrySettings& retry_settings,
                         client::CancellationContext context) {
  struct ResponseData {
    Condition condition;
    http::NetworkResponse response{kCancelledErrorResponse};
    http::Headers headers;
  };

  auto response_data = std::make_shared<ResponseData>();
  auto response_body = std::make_shared<std::stringstream>();
  http::SendOutcome outcome{http::ErrorCode::CANCELLED_ERROR};
  const auto timeout = std::chrono::seconds(retry_settings.timeout);

  context.ExecuteOrCancelled(
      [&]() {
        outcome = settings.network_request_handler->Send(
            request, response_body,
            [response_data](http::NetworkResponse response) {
              response_data->response = std::move(response);
              response_data->condition.Notify();
            },
            [response_data](std::string key, std::string value) {
              response_data->headers.emplace_back(std::move(key),
                                                  std::move(value));
            });

        if (!outcome.IsSuccessful()) {
          OLP_SDK_LOG_WARNING_F(kLogTag,
                                "SendRequest: sending request failed, url=%s",
                                request.GetUrl().c_str());
          return CancellationToken();
        }

        auto request_handler = settings.network_request_handler;
        auto request_id = outcome.GetRequestId();

        return CancellationToken([=]() {
          request_handler->Cancel(request_id);
          response_data->condition.Notify();
        });
      },
      [&]() { response_data->condition.Notify(); });

  if (!outcome.IsSuccessful()) {
    return ToHttpResponse(outcome);
  }

  if (!response_data->condition.Wait(timeout)) {
    OLP_SDK_LOG_WARNING_F(kLogTag, "Request %" PRIu64 " timed out!",
                          outcome.GetRequestId());
    context.CancelOperation();
    return ToHttpResponse(kTimeoutErrorResponse);
  }

  if (context.IsCancelled()) {
    return ToHttpResponse(kCancelledErrorResponse);
  }

  HttpResponse response{response_data->response.GetStatus(),
                        std::move(*response_body),
                        std::move(response_data->headers)};

  response.SetNetworkStatistics(GetStatistics(response_data->response));

  return response;
}

NetworkAsyncCallback GetRetryCallback(
    int current_try, std::chrono::milliseconds current_backdown_period,
    std::chrono::milliseconds accumulated_wait_time,
    const RetrySettings& settings, const NetworkAsyncCallback& callback,
    const std::shared_ptr<http::NetworkRequest>& network_request,
    std::weak_ptr<http::Network> network,
    const std::weak_ptr<CancellationContext>& weak_cancel_context) {
  ++current_try;
  return [=](HttpResponse response) {
    const auto max_wait_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(settings.timeout));

    if (current_try > settings.max_attempts ||
        !settings.retry_condition(response) ||
        accumulated_wait_time >= max_wait_time) {
      // Response is either successull or retries count/time expired
      callback(std::move(response));
      return;
    }

    // TODO there must be a better way
    const auto actual_wait_time = std::min(
        current_backdown_period, max_wait_time - accumulated_wait_time);
    std::this_thread::sleep_for(actual_wait_time);

    const auto next_wait_time = CalculateNextWaitTime(settings, current_try);

    auto cancel_context = weak_cancel_context.lock();
    if (cancel_context) {
      cancel_context->ExecuteOrCancelled(
          [&]() -> CancellationToken {
            return ExecuteSingleRequest(
                network, *network_request,
                GetRetryCallback(current_try, next_wait_time,
                                 accumulated_wait_time + actual_wait_time,
                                 settings, callback, network_request, network,
                                 weak_cancel_context));
          },
          [callback] { callback(ToHttpResponse(kCancelledErrorResponse)); });
    } else {
      // Last (and only) strong reference lives in cancellation
      // token, which is reset on CancelOperation.
      callback(ToHttpResponse(kCancelledErrorResponse));
    }
  };
}

bool IsPending(const PendingUrlRequestPtr& request) {
  // A request is pending when it already triggered a Network call
  return request &&
         request->GetRequestId() != PendingUrlRequest::kInvalidRequestId;
}

}  // namespace

class OlpClient::OlpClientImpl {
 public:
  OlpClientImpl();
  explicit OlpClientImpl(const OlpClientSettings& settings,
                         std::string base_url);
  ~OlpClientImpl();

  void SetBaseUrl(const std::string& base_url);
  const std::string& GetBaseUrl() const;

  ParametersType& GetMutableDefaultHeaders();
  void SetSettings(const OlpClientSettings& settings);

  CancellationToken CallApi(const std::string& path, const std::string& method,
                            const ParametersType& query_params,
                            const ParametersType& header_params,
                            const ParametersType& form_params,
                            const RequestBodyType& post_body,
                            const std::string& content_type,
                            const NetworkAsyncCallback& callback) const;

  HttpResponse CallApi(std::string path, std::string method,
                       ParametersType query_params,
                       ParametersType header_params, ParametersType form_params,
                       RequestBodyType post_body, std::string content_type,
                       CancellationContext context) const;

  std::shared_ptr<http::NetworkRequest> CreateRequest(
      const std::string& path, const std::string& method,
      const ParametersType& query_params, const ParametersType& header_params,
      const RequestBodyType& post_body, const std::string& content_type) const;

  void AddBearer(bool query_empty, http::NetworkRequest& request) const;

 private:
  /// Mostly used to protect base_url_
  mutable std::mutex mutex_;
  std::string base_url_;
  ParametersType default_headers_;
  OlpClientSettings settings_;
  PendingUrlRequestsPtr pending_requests_;
};

OlpClient::OlpClientImpl::OlpClientImpl()
    : pending_requests_{std::make_shared<PendingUrlRequests>()} {}

OlpClient::OlpClientImpl::OlpClientImpl(const OlpClientSettings& settings,
                                        std::string base_url)
    : settings_{settings},
      base_url_{std::move(base_url)},
      pending_requests_{std::make_shared<PendingUrlRequests>()} {}

OlpClient::OlpClientImpl::~OlpClientImpl() {
  pending_requests_->CancelAllAndWait();
}

void OlpClient::OlpClientImpl::SetBaseUrl(const std::string& base_url) {
  std::lock_guard<std::mutex> lock(mutex_);
  base_url_ = base_url;
}

const std::string& OlpClient::OlpClientImpl::GetBaseUrl() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return base_url_;
}

OlpClient::ParametersType&
OlpClient::OlpClientImpl::GetMutableDefaultHeaders() {
  return default_headers_;
}

void OlpClient::OlpClientImpl::SetSettings(const OlpClientSettings& settings) {
  // I would not expect that settings change during lifetime of the instance.
  settings_ = settings;
}

void OlpClient::OlpClientImpl::AddBearer(bool query_empty,
                                         http::NetworkRequest& request) const {
  const auto& settings = settings_.authentication_settings;
  if (!settings) {
    return;
  }

  if (settings->api_key_provider) {
    const auto& api_key = settings->api_key_provider();
    request.WithUrl(request.GetUrl() + (query_empty ? "?" : "&") +
                    kApiKeyParam + api_key);
    return;
  }

  if (settings->provider) {
    std::string bearer =
        http::kBearer + std::string(" ") + settings->provider();
    request.WithHeader(http::kAuthorizationHeader, bearer);
  }
}

std::shared_ptr<http::NetworkRequest> OlpClient::OlpClientImpl::CreateRequest(
    const std::string& path, const std::string& method,
    const OlpClient::ParametersType& query_params,
    const OlpClient::ParametersType& header_params,
    const RequestBodyType& post_body, const std::string& content_type) const {
  std::string url;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    url = utils::Url::Construct(base_url_, path, query_params);
  }

  auto network_request = std::make_shared<http::NetworkRequest>(std::move(url));

  network_request->WithVerb(GetHttpVerb(method));

  for (const auto& header : default_headers_) {
    network_request->WithHeader(header.first, header.second);
  }

  std::string custom_user_agent;
  for (const auto& header : header_params) {
    // Merge all User-Agent headers into one header.
    // This is required for (at least) iOS network implementation,
    // which uses headers dictionary without any duplicates.
    // User agents entries are usually separated by a whitespace, e.g.
    // Mozilla/5.0 (Windows NT 6.1; Win64; x64; rv:47.0) Firefox/47.0
    if (CaseInsensitiveCompare(header.first, http::kUserAgentHeader)) {
      custom_user_agent += header.second + std::string(" ");
    } else {
      network_request->WithHeader(header.first, header.second);
    }
  }

  custom_user_agent += http::kOlpSdkUserAgent;
  network_request->WithHeader(http::kUserAgentHeader, custom_user_agent);

  if (!content_type.empty()) {
    network_request->WithHeader(http::kContentTypeHeader, content_type);
  }

  network_request->WithBody(post_body);
  return network_request;
}

CancellationToken OlpClient::OlpClientImpl::CallApi(
    const std::string& path, const std::string& method,
    const OlpClient::ParametersType& query_params,
    const OlpClient::ParametersType& header_params,
    const OlpClient::ParametersType& /*form_params*/,
    const OlpClient::RequestBodyType& post_body,
    const std::string& content_type,
    const NetworkAsyncCallback& callback) const {
  if (!settings_.network_request_handler) {
    callback({static_cast<int>(http::ErrorCode::OFFLINE_ERROR),
              "Network layer offline or missing."});
    return CancellationToken();
  }

  OLP_SDK_LOG_INFO_F(kLogTag, "CreateRequest - start");

  auto network_request = CreateRequest(path, method, query_params,
                                       header_params, post_body, content_type);

  OLP_SDK_LOG_INFO_F(kLogTag, "CreateRequest - end");

  PendingUrlRequestPtr request_ptr = nullptr;
  auto& pending_requests = pending_requests_;
  const auto& url = network_request->GetUrl();
  CancellationToken cancellation_token;

  // Only merge same request in case there is no body as a body can alter the
  // outcome of the request and may not match the response of a request with a
  // different body.
  bool merge = (!post_body || post_body->empty());
  if (merge) {
    request_ptr = (*pending_requests)[url];

    // Add callback and prepare CancellationToken
    auto call_id = request_ptr->Append(callback);
    cancellation_token =
        CancellationToken([=] { pending_requests->Cancel(url, call_id); });

    if (IsPending(request_ptr)) {
      // Network call is already triggered, we only need to append our callback
      // Cancels this callback; internally once all pending callbacks are
      // cancelled the Network call will be automatically cancelled also
      return std::move(cancellation_token);
    }
  } else {
    // In case we do not merge same requests create a single PendingUrlRequest
    // as it is handling the cancellation the same as we would do normally
    request_ptr = std::make_shared<PendingUrlRequest>();

    // Add callback and prepare CancellationToken
    auto call_id = request_ptr->Append(callback);
    cancellation_token =
        CancellationToken([=] { request_ptr->Cancel(call_id); });
  }

  OLP_SDK_LOG_INFO_F(kLogTag, "GettingRequest");

  AddBearer(query_params.empty(), *network_request);

  const auto& retry_settings = settings_.retry_settings;
  auto proxy = settings_.proxy_settings.value_or(http::NetworkProxySettings());
  network_request->WithSettings(
      http::NetworkSettings()
          .WithConnectionTimeout(retry_settings.timeout)
          .WithTransferTimeout(retry_settings.timeout)
          .WithRetries(retry_settings.max_attempts)
          .WithProxySettings(std::move(proxy)));

  auto network = settings_.network_request_handler;
  auto request_settings = GetRequestSettings(retry_settings);

  // This is the final callback which decides if the operation is successfull
  // and calls the user callback or if we need another retry. This also takes
  // care that we do not exceed the total timeout specified by the user in
  // RetrySettings
  /*
  NetworkCallbackType retry_callback;
  retry_callback = [=](const http::RequestId request_id,
                       HttpResponse response) mutable {
    ++request_settings->current_try;

    OLP_SDK_LOG_INFO_F(kLogTag, "retry_callback - called, try=%u",
                       request_settings->current_try);

    if (CheckRetryCondition(*request_settings, retry_settings, response)) {
      // Response is either successull or retries count/time expired
      if (request_ptr->GetRequestId() != request_id) {
        OLP_SDK_LOG_WARNING_F(kLogTag,
                              "Wrong response received, request_ptr=%" PRIu64
                              ", request_id=%" PRIu64,
                              request_ptr->GetRequestId(), request_id);
        return;
      }

      if (merge) {
        OLP_SDK_LOG_INFO_F(kLogTag,
                           "retry_callback - merged request completed");
        pending_requests->OnRequestCompleted(request_id, url,
                                             std::move(response));
      } else {
        OLP_SDK_LOG_INFO_F(kLogTag,
                           "retry_callback - unmerged request completed");
        request_ptr->OnRequestCompleted(std::move(response));
      }
      return;
    }

    const auto start = std::chrono::steady_clock::now();

    // TODO: Do not block thread but instead implement an event queue that will
    // trigger next retry once the time expired!
    const auto actual_wait_time =
        std::min(request_settings->current_backdown_period,
                 request_settings->max_wait_time -
                     request_settings->accumulated_wait_time);
    std::this_thread::sleep_for(actual_wait_time);

    request_settings->current_backdown_period =
        CalculateNextWaitTime(retry_settings, request_settings->current_try);

    OLP_SDK_LOG_INFO_F(kLogTag,
                       "retry_callback - retrigger after sleep, "
                       "actual_wait_time=%" PRId64 "ms, slept=%" PRId64 "ms",
                       actual_wait_time.count(),
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count());

    ExecuteSingleRequest(settings_.network_request_handler, request_ptr,
                         *network_request, retry_callback);
  };
  */

  OLP_SDK_LOG_INFO_F(kLogTag, "First request");

  ExecuteSingleRequest(
      network, request_ptr, *network_request,
      GetNewRetryCallback(merge, request_settings, retry_settings, network,
                          pending_requests, request_ptr, network_request));

  OLP_SDK_LOG_INFO_F(kLogTag, "First request finished");

  return cancellation_token;

  /*
 auto cancel_context = std::make_shared<CancellationContext>();
 std::weak_ptr<olp::http::Network> network = settings_.network_request_handler;

 auto retry_callback = GetRetryCallback(
     0, std::chrono::milliseconds(retry_settings.initial_backdown_period),
     std::chrono::milliseconds::zero(), retry_settings, callback,
     network_request, network, cancel_context);

 cancel_context->ExecuteOrCancelled(
     [=]() -> CancellationToken {
       return ExecuteSingleRequest(network, *network_request, retry_callback);
     },
     [callback] { callback(ToHttpResponse(kCancelledErrorResponse)); });

 return CancellationToken(
     [cancel_context]() { cancel_context->CancelOperation(); });
     */
}

HttpResponse OlpClient::OlpClientImpl::CallApi(
    std::string path, std::string method,
    OlpClient::ParametersType query_params,
    OlpClient::ParametersType header_params,
    OlpClient::ParametersType /*forms_params*/,
    OlpClient::RequestBodyType post_body, std::string content_type,
    CancellationContext context) const {
  if (!settings_.network_request_handler) {
    return HttpResponse(static_cast<int>(olp::http::ErrorCode::OFFLINE_ERROR),
                        "Network request handler is empty.");
  }

  const auto& retry_settings = settings_.retry_settings;
  auto network_settings =
      http::NetworkSettings()
          .WithTransferTimeout(retry_settings.timeout)
          .WithConnectionTimeout(retry_settings.timeout)
          .WithProxySettings(
              settings_.proxy_settings.value_or(http::NetworkProxySettings()));

  std::string url;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    url = utils::Url::Construct(base_url_, path, query_params);
  }

  auto network_request = http::NetworkRequest(url);

  network_request.WithVerb(GetHttpVerb(method))
      .WithBody(std::move(post_body))
      .WithSettings(std::move(network_settings));

  for (const auto& header : default_headers_) {
    network_request.WithHeader(header.first, header.second);
  }

  std::string custom_user_agent;
  for (const auto& header : header_params) {
    // Merge all User-Agent headers into one header.
    // This is required for (at least) iOS network implementation,
    // which uses headers dictionary without any duplicates.
    // User agents entries are usually separated by a whitespace, e.g.
    // Mozilla/5.0 (Windows NT 6.1; Win64; x64; rv:47.0) Firefox/47.0
    if (CaseInsensitiveCompare(header.first, http::kUserAgentHeader)) {
      custom_user_agent += header.second + std::string(" ");
    } else {
      network_request.WithHeader(header.first, header.second);
    }
  }

  custom_user_agent += http::kOlpSdkUserAgent;
  network_request.WithHeader(http::kUserAgentHeader, custom_user_agent);

  if (!content_type.empty()) {
    network_request.WithHeader(http::kContentTypeHeader, content_type);
  }

  auto backdown_period =
      std::chrono::milliseconds(retry_settings.initial_backdown_period);

  AddBearer(query_params.empty(), network_request);

  auto response =
      SendRequest(network_request, settings_, retry_settings, context);

  NetworkStatistics accumulated_statistics = response.GetNetworkStatistics();

  // Make sure that we don't wait longer than `timeout` in retry settings
  auto accumulated_wait_time = backdown_period;
  const auto max_wait_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::seconds(retry_settings.timeout));
  for (int i = 1; i <= retry_settings.max_attempts && !context.IsCancelled() &&
                  accumulated_wait_time < max_wait_time;
       i++) {
    if (StatusSuccess(response.status)) {
      return response;
    }

    if (!retry_settings.retry_condition(response)) {
      return response;
    }

    // do the periodical sleep and check for cancellation status in between.
    auto duration_to_sleep =
        std::min(backdown_period, max_wait_time - accumulated_wait_time);
    accumulated_wait_time += duration_to_sleep;

    while (duration_to_sleep.count() > 0 && !context.IsCancelled()) {
      const auto sleep_ms =
          std::min(std::chrono::milliseconds(1000), duration_to_sleep);
      std::this_thread::sleep_for(sleep_ms);
      duration_to_sleep -= sleep_ms;
    }

    backdown_period = CalculateNextWaitTime(retry_settings, i);
    response = SendRequest(network_request, settings_, retry_settings, context);

    // In case we retry, accumulate the stats
    accumulated_statistics += response.GetNetworkStatistics();
  }

  response.SetNetworkStatistics(accumulated_statistics);

  return response;
}

OlpClient::OlpClient() : impl_(std::make_shared<OlpClientImpl>()){};
OlpClient::OlpClient(const OlpClientSettings& settings, std::string base_url)
    : impl_(std::make_shared<OlpClientImpl>(settings, std::move(base_url))) {}
OlpClient::~OlpClient() = default;

OlpClient::OlpClient(const OlpClient&) = default;
OlpClient& OlpClient::operator=(const OlpClient&) = default;
OlpClient::OlpClient(OlpClient&&) noexcept = default;
OlpClient& OlpClient::operator=(OlpClient&&) noexcept = default;

void OlpClient::SetBaseUrl(const std::string& base_url) {
  impl_->SetBaseUrl(base_url);
}

const std::string& OlpClient::GetBaseUrl() const { return impl_->GetBaseUrl(); }

OlpClient::ParametersType& OlpClient::GetMutableDefaultHeaders() {
  return impl_->GetMutableDefaultHeaders();
}

void OlpClient::SetSettings(const OlpClientSettings& settings) {
  impl_->SetSettings(settings);
}

CancellationToken OlpClient::CallApi(
    const std::string& path, const std::string& method,
    const ParametersType& query_params, const ParametersType& header_params,
    const ParametersType& form_params, const RequestBodyType& post_body,
    const std::string& content_type,
    const NetworkAsyncCallback& callback) const {
  return impl_->CallApi(path, method, query_params, header_params, form_params,
                        post_body, content_type, callback);
}

HttpResponse OlpClient::CallApi(std::string path, std::string method,
                                ParametersType query_params,
                                ParametersType header_params,
                                ParametersType form_params,
                                RequestBodyType post_body,
                                std::string content_type,
                                CancellationContext context) const {
  return impl_->CallApi(std::move(path), std::move(method),
                        std::move(query_params), std::move(header_params),
                        std::move(form_params), std::move(post_body),
                        std::move(content_type), std::move(context));
}

}  // namespace client
}  // namespace olp
