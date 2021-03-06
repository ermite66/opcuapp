#include <opcuapp/client/session.h>
#include <opcuapp/client/subscription.h>
#include <opcuapp/proxy_stub.h>
#include <iostream>
#include <sstream>
#include <thread>

using namespace std::chrono_literals;

namespace {

class Log {
 public:
  template <class V>
  const Log& operator<<(const V& value) const {
    std::cout << value;
    return *this;
  }

  ~Log() { std::cout << std::endl; }

 private:
  static std::recursive_mutex s_mutex;
  std::lock_guard<std::recursive_mutex> lock_{s_mutex};
};

std::recursive_mutex Log::s_mutex;

std::string ToString(OpcUa_ServerState state) {
  const char* strings[] = {
      "Running",  "Failed", "NoConfiguration",    "Suspended",
      "Shutdown", "Test",   "CommunicationFault", "Unknown",
  };
  const size_t index =
      std::min(std::size(strings) - 1, static_cast<size_t>(state));
  return strings[index];
}

std::string ToString(OpcUa_Boolean value) {
  return value == OpcUa_False ? "False" : "True";
}

std::string ToString(OpcUa_DateTime date_time) {
  OpcUa_CharA buffer[25] = {};
  if (!OpcUa_IsGood(OpcUa_DateTime_GetStringFromDateTime(date_time, buffer,
                                                         sizeof(buffer)))) {
    assert(false);
    return {};
  }
  return buffer;
}

std::string ToString(const OpcUa_Variant& variant) {
  switch (variant.Datatype) {
    case OpcUaType_Boolean:
      return ToString(variant.Value.Boolean);
    case OpcUaType_DateTime:
      return ToString(variant.Value.DateTime);
    default:
      // FIXME
      assert(false);
      return "Unknown";
  }
}

std::string ToString(OpcUa_StatusCode status_code) {
  return OpcUa_IsGood(status_code)
             ? "Good"
             : OpcUa_IsUncertain(status_code) ? "Uncertain" : "Bad";
}

}  // namespace

class Client {
 public:
  void Connect(const opcua::String& url);

 private:
  void CreateSession();
  void ActivateSession();
  void ReadServerStatus();
  void CreateSubscription();
  void CreateMonitoredItems();

  void OnError(opcua::StatusCode status_code);

  const opcua::ByteString client_certificate_;
  const opcua::Key client_private_key_;
  const OpcUa_P_OpenSSL_CertificateStore_Config pki_config_{OpcUa_NO_PKI};
  const opcua::ByteString server_certificate_;
  const opcua::String requested_security_policy_uri_{OpcUa_SecurityPolicy_None};
  opcua::client::Channel channel_{OpcUa_Channel_SerializerType_Binary};

  opcua::client::Session session_{channel_};
  opcua::client::Subscription subscription_{session_};

  bool session_created_ = false;
  bool session_activated_ = false;
  bool subscription_created_ = false;
};

void Client::Connect(const opcua::String& url) {
  Log() << "Connecting...";

  opcua::client::ChannelContext context{
      url.raw_string(),
      &client_certificate_.get(),
      &client_private_key_,
      &server_certificate_.get(),
      &pki_config_,
      &requested_security_policy_uri_.get(),
      0,
      OpcUa_MessageSecurityMode_None,
      10000,
  };

  channel_.Connect(context, [this](opcua::StatusCode status_code,
                                   OpcUa_Channel_Event event) {
    if (event != eOpcUa_Channel_Event_Connected)
      return OnError(status_code);
    if (!session_created_)
      CreateSession();
  });
}

void Client::CreateSession() {
  assert(!session_created_);

  Log() << "Creating session...";

  opcua::CreateSessionRequest request;
  request.ClientDescription.ApplicationType = OpcUa_ApplicationType_Client;
  OpcUa_String_AttachReadOnly(&request.ClientDescription.ApplicationName.Text,
                              "TestClientName");
  OpcUa_String_AttachReadOnly(&request.ClientDescription.ApplicationUri,
                              "TestClientUri");
  OpcUa_String_AttachReadOnly(&request.ClientDescription.ProductUri,
                              "TestProductUri");

  session_.Create(request, [this](opcua::StatusCode status_code) {
    if (!status_code)
      return OnError(status_code);
    session_created_ = true;
    Log() << "Session created";
    if (!session_activated_)
      ActivateSession();
  });
}

void Client::ActivateSession() {
  assert(session_created_);
  assert(!session_activated_);

  Log() << "Activating session...";

  session_.Activate([this](opcua::StatusCode status_code) {
    assert(!session_activated_);
    if (!status_code)
      return OnError(status_code);
    session_activated_ = true;
    Log() << "Session activated";
    ReadServerStatus();
    if (!subscription_created_)
      CreateSubscription();
  });
}

void Client::ReadServerStatus() {
  Log() << "Reading Server status...";

  opcua::ReadValueId read_id;
  read_id.AttributeId = OpcUa_Attributes_Value;
  opcua::NodeId{OpcUaId_Server_ServerStatus}.release(read_id.NodeId);

  session_.Read({&read_id, 1}, [this](opcua::StatusCode status_code,
                                      opcua::Span<OpcUa_DataValue> results) {
    if (!status_code)
      return OnError(status_code);
    assert(results.size() == 1);
    auto& data_value = results.front();
    assert(OpcUa_IsGood(data_value.StatusCode));
    auto& value = results.front().Value;
    assert(value.Datatype == OpcUaType_ExtensionObject);
    auto& extension = *value.Value.ExtensionObject;
    assert(extension.TypeId ==
           OpcUaId_ServerStatusDataType_Encoding_DefaultBinary);
    assert(extension.Encoding ==
           OpcUa_ExtensionObjectEncoding_EncodeableObject);
    auto& server_status = *reinterpret_cast<OpcUa_ServerStatusDataType*>(
        extension.Body.EncodeableObject.Object);
    Log() << "Server state is " << ToString(server_status.State);
  });
}

void Client::CreateSubscription() {
  assert(session_activated_);
  assert(!subscription_created_);

  Log() << "Creating subscription...";

  opcua::client::SubscriptionParams params{
      500ms,  // publishing_interval
      3000,   // lifetime_count
      10000,  // max_keepalive_count
      0,      // max_notifications_per_publish
      true,   // publishing_enabled
      0,      // priority
  };

  subscription_.Create(params, [this](opcua::StatusCode status_code) {
    assert(!subscription_created_);
    if (!status_code)
      return OnError(status_code);

    Log() << "Subscription created";
    subscription_created_ = true;

    Log() << "Starting subscription publishing...";
    subscription_.StartPublishing(
        [](opcua::StatusCode status_code) {
          Log() << "Subscription status is " << ToString(status_code.code());
        },
        [](OpcUa_DataChangeNotification& notification) {
          opcua::Span<OpcUa_MonitoredItemNotification> items{
              notification.MonitoredItems,
              static_cast<size_t>(notification.NoOfMonitoredItems)};
          for (auto& item : items)
            Log() << "Data changed " << item.ClientHandle << "="
                  << ToString(item.Value.Value);
        });

    CreateMonitoredItems();
  });
}

void Client::CreateMonitoredItems() {
  Log() << "Creating monitored items...";

  opcua::MonitoredItemCreateRequest monitored_item;
  opcua::NodeId{OpcUaId_Server_ServerStatus_CurrentTime}.release(
      monitored_item.ItemToMonitor.NodeId);
  monitored_item.ItemToMonitor.AttributeId = OpcUa_Attributes_Value;
  monitored_item.RequestedParameters.ClientHandle = 1;
  monitored_item.MonitoringMode = OpcUa_MonitoringMode_Reporting;

  subscription_.CreateMonitoredItems(
      {&monitored_item, 1}, OpcUa_TimestampsToReturn_Both,
      [this](opcua::StatusCode status_code,
             opcua::Span<OpcUa_MonitoredItemCreateResult> results) {
        if (!status_code)
          return OnError(status_code);
        assert(results.size() == 1);
        auto& result = results.front();
        if (!OpcUa_IsGood(result.StatusCode))
          return OnError(status_code);
        Log() << "Monitored items created";
      });
}

void Client::OnError(opcua::StatusCode status_code) {
  Log() << "Error 0x" << std::hex << static_cast<unsigned>(status_code.code());
  assert(false);
}

int main() {
  opcua::Platform platform;
  opcua::ProxyStub proxy_stub{platform, opcua::ProxyStubConfiguration{}};

  const opcua::String url =
      /*"opc.tcp://master:51210/UA/SampleServer"*/ "opc.tcp://localhost:4840";

  try {
    Client client;
    client.Connect(url);

    {
      Log() << "Waiting for 5 seconds...";
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }

  } catch (const std::exception& e) {
    Log() << "ERROR: " << e.what();
    return 1;
  }

  return 0;
}
