#pragma once

#include "opcuapp/client/async_request.h"
#include "opcuapp/client/channel.h"
#include "opcuapp/requests.h"
#include "opcuapp/signal.h"
#include "opcuapp/structs.h"
#include "opcuapp/status_code.h"
#include "opcuapp/types.h"
#include "opcuapp/node_id.h"

#include <memory>
#include <map>
#include <mutex>
#include <vector>

namespace opcua {
namespace client {

class Subscription;

struct SessionInfo {
  NodeId session_id;
  NodeId authentication_token;
  Double revised_timeout;
  ByteString server_nonce;
  ByteString server_certificate;
};

class Session {
 public:
  explicit Session(Channel& channel);

  Channel& channel() { return channel_; }
  StatusCode status_code() const { std::lock_guard<std::mutex> lock{mutex_}; return status_code_; }

  void Create();
  void Delete();

  using BrowseCallback = std::function<void(StatusCode status_code, Span<OpcUa_BrowseResult> results)>;
  void Browse(Span<const OpcUa_BrowseDescription> descriptions, const BrowseCallback& callback);

  using ReadCallback = std::function<void(StatusCode status_code, Span<OpcUa_DataValue> results)>;
  void Read(Span<const OpcUa_ReadValueId> read_ids, const ReadCallback& callback);

  Signal<void(StatusCode status_code)> status_changed;

 private:
  void SetStatus(StatusCode status_code);

  void CommitCreate();

  void Activate();

  void InitRequestHeader(OpcUa_RequestHeader& header) const;

  using NotificationHandler = std::function<void(Span<OpcUa_ExtensionObject> notifications)>;
  void StartPublishing(SubscriptionId subscription_id, NotificationHandler handler);
  void StopPublishing(SubscriptionId subscription_id);

  void Publish();
  void OnPublishResponse(StatusCode status_code, SubscriptionId subscription_id,
      Span<SequenceNumber> available_sequence_numbers, bool more_notifications,
      OpcUa_NotificationMessage& notification_message, Span<OpcUa_StatusCode> results);

  void OnActivated(ByteString server_nonce);
  void OnError(StatusCode status_code);

  Channel& channel_;
  ScopedSignalConnection session_status_connection_;

  mutable std::mutex mutex_;
  bool created_ = false;
  bool creation_requested_ = false;
  StatusCode status_code_{OpcUa_Bad};
  SessionInfo info_;
  std::map<SubscriptionId, NotificationHandler> subscriptions_;
  std::vector<OpcUa_SubscriptionAcknowledgement> acknowledgements_;
  std::vector<OpcUa_SubscriptionAcknowledgement> sent_acknowledgements_;
  bool publishing_ = false;

  friend class Subscription;
};

Session::Session(Channel& channel)
    : channel_{channel} {
  session_status_connection_ = channel_.status_changed.Connect([this](StatusCode status_code) {
    if (!status_code)
      return;
    if (!created_ && creation_requested_)
      CommitCreate();
    else if (created_)
      Activate();
  });
}

inline void Session::Create() {
  creation_requested_ = true;
  if (channel_.status_code())
    CommitCreate();
}

inline void Session::CommitCreate() {
  using Request = AsyncRequest<CreateSessionResponse>;
  auto async_request = std::make_unique<Request>([this](CreateSessionResponse& response) {
    const StatusCode status_code{response.ResponseHeader.ServiceResult};
    if (!status_code)
      return OnError(status_code);
    {
      std::lock_guard<std::mutex> lock{mutex_};
      created_ = true;
      info_.session_id.swap(response.SessionId);
      info_.authentication_token.swap(response.AuthenticationToken);
      info_.revised_timeout = response.RevisedSessionTimeout;
      info_.server_nonce.swap(response.ServerNonce);
      info_.server_certificate.swap(response.ServerCertificate);
    }
    Activate();
  });

  CreateSessionRequest request;
  StatusCode status_code = OpcUa_ClientApi_BeginCreateSession(
      channel_.handle(),
      &request.RequestHeader,
      &request.ClientDescription,
      &request.ServerUri,
      &request.EndpointUrl,
      &request.SessionName,
      &request.ClientNonce,
      &request.ClientCertificate,
      request.RequestedSessionTimeout,
      request.MaxResponseMessageSize,
      &Request::OnComplete,
      async_request.release());

  if (!status_code)
    OnError(status_code);
}

inline void Session::Activate() {
  /*{
    std::lock_guard<std::mutex> lock{mutex_};
    acknowledgements_.insert(acknowledgements_.begin(), sent_acknowledgements_.begin(),
                                                        sent_acknowledgements_.end());
    sent_acknowledgements_.clear();
    publishing_ = false;
  }*/

  ActivateSessionRequest request;
  InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<ActivateSessionResponse>;
  auto async_request = std::make_unique<Request>([this](ActivateSessionResponse& response) {
    const StatusCode status_code{response.ResponseHeader.ServiceResult};
    if (!status_code)
      return OnError(status_code);
    OnActivated(std::move(response.ServerNonce));
  });

  StatusCode status_code = OpcUa_ClientApi_BeginActivateSession(
      channel_.handle(),
      &request.RequestHeader,
      &request.ClientSignature,
      request.NoOfClientSoftwareCertificates,
      request.ClientSoftwareCertificates,
      request.NoOfLocaleIds,
      request.LocaleIds,
      &request.UserIdentityToken,
      &request.UserTokenSignature,
      &Request::OnComplete,
      async_request.release());

  if (!status_code)
    OnError(status_code);
}

inline void Session::Browse(Span<const OpcUa_BrowseDescription> descriptions, const BrowseCallback& callback) {
  BrowseRequest request;
  InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<BrowseResponse>;
  auto async_request = std::make_unique<Request>([callback](BrowseResponse& response) {
    callback(response.ResponseHeader.ServiceResult, {response.Results, static_cast<size_t>(response.NoOfResults)});
  });

  StatusCode status_code = OpcUa_ClientApi_BeginBrowse(
      channel_.handle(),
      &request.RequestHeader,
      &request.View,
      request.RequestedMaxReferencesPerNode,
      descriptions.size(),
      descriptions.data(),
      &Request::OnComplete,
      async_request.release());

  request.NodesToBrowse = nullptr;
  request.NoOfNodesToBrowse = 0;

  if (!status_code)
    callback(status_code, {});
}

inline void Session::Read(Span<const OpcUa_ReadValueId> read_ids, const ReadCallback& callback) {
  ReadRequest request;
  InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<ReadResponse>;
  auto async_request = std::make_unique<Request>([callback](ReadResponse& response) {
    callback(response.ResponseHeader.ServiceResult, {response.Results, static_cast<size_t>(response.NoOfResults)});
  });

  StatusCode status_code = OpcUa_ClientApi_BeginRead(
      channel_.handle(),
      &request.RequestHeader,
      request.MaxAge,
      request.TimestampsToReturn,
      read_ids.size(),
      read_ids.data(),
      &Request::OnComplete,
      async_request.release());

  request.NoOfNodesToRead = 0;
  request.NodesToRead = nullptr;

  if (!status_code)
    callback(status_code, {});
}

inline void Session::Delete() {
  std::lock_guard<std::mutex> lock{mutex_};
  subscriptions_.clear();
  acknowledgements_.clear();
  sent_acknowledgements_.clear();
  publishing_ = false;
}

inline void Session::InitRequestHeader(OpcUa_RequestHeader& header) const {
  std::lock_guard<std::mutex> lock{mutex_};
  header.TimeoutHint = 60000;
  header.Timestamp = ::OpcUa_DateTime_UtcNow();
  info_.authentication_token.CopyTo(header.AuthenticationToken);
}

inline void Session::StartPublishing(SubscriptionId subscription_id, NotificationHandler handler) {
  bool publishing = false;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    subscriptions_.emplace(subscription_id, std::move(handler));
    publishing = publishing_;
  }

  if (!publishing)
    Publish();
}

inline void Session::StopPublishing(SubscriptionId subscription_id) {
  std::lock_guard<std::mutex> lock{mutex_};
  subscriptions_.erase(subscription_id);
}

inline void Session::Publish() {
  std::vector<OpcUa_SubscriptionAcknowledgement> acknowledgements;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    if (publishing_)
      return;
    publishing_ = true;
    acknowledgements = std::move(acknowledgements_);
    acknowledgements_.clear();
    sent_acknowledgements_ = acknowledgements;
  }

  PublishRequest request;
  InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<PublishResponse>;
  auto async_request = std::make_unique<Request>([this](PublishResponse& response) {
    OnPublishResponse(response.ResponseHeader.ServiceResult,
        response.SubscriptionId,
        {response.AvailableSequenceNumbers, static_cast<size_t>(response.NoOfAvailableSequenceNumbers)},
        response.MoreNotifications != OpcUa_False,
        response.NotificationMessage,
        {response.Results, static_cast<size_t>(response.NoOfResults)});
  });

  StatusCode status_code = OpcUa_ClientApi_BeginPublish(
      channel_.handle(),
      &request.RequestHeader,
      acknowledgements.size(),
      acknowledgements.data(),
      &Request::OnComplete,
      async_request.release());

  if (!status_code)
    OnError(status_code);
}

inline void Session::OnPublishResponse(StatusCode status_code, SubscriptionId subscription_id,
    Span<SequenceNumber> available_sequence_numbers, bool more_notifications,
    OpcUa_NotificationMessage& message, Span<OpcUa_StatusCode> results) {
  if (!status_code)
    return OnError(status_code);

  for (StatusCode result : results) {
    if (!result)
      return OnError(result);
  }

  NotificationHandler handler;

  if (message.NoOfNotificationData) {
    std::lock_guard<std::mutex> lock{mutex_};
    assert(publishing_);
    publishing_ = false;
    sent_acknowledgements_.clear();
    acknowledgements_.push_back({subscription_id, message.SequenceNumber});

    auto i = subscriptions_.find(subscription_id);
    if (i != subscriptions_.end())
      handler = i->second;
  }

  Publish();

  if (handler)
    handler({message.NotificationData, static_cast<size_t>(message.NoOfNotificationData)});
}

inline void Session::OnError(StatusCode status_code) {
  SetStatus(status_code);
}

inline void Session::OnActivated(ByteString server_nonce) {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    //assert(!publishing_);
    status_code_ = OpcUa_Good;
    info_.server_nonce = std::move(server_nonce);
  }

  status_changed(OpcUa_Good);
}

inline void Session::SetStatus(StatusCode status_code) {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    status_code_ = status_code;
  }
  status_changed(status_code);
}

} // namespace client
} // namespace opcua