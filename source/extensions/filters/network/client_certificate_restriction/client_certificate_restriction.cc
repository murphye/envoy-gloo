#include "extensions/filters/network/client_certificate_restriction/client_certificate_restriction.h"

#include "envoy/buffer/buffer.h"
#include "envoy/network/connection.h"

#include "common/common/assert.h"
#include "common/http/message_impl.h"
#include "common/ssl/ssl_socket.h"

#include "authorize.pb.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ClientCertificateRestriction {

const std::string ClientCertificateRestrictionFilter::AUTHORIZE_PATH =
    "/v1/agent/connect/authorize";

ClientCertificateRestrictionConfig::ClientCertificateRestrictionConfig(
    const envoy::config::filter::network::client_certificate_restriction::v2::
        ClientCertificateRestriction &config)
    : target_(config.target()),
      authorize_hostname_(config.authorize_hostname()),
      authorize_cluster_name_(config.authorize_cluster_name()),
      request_timeout_(
          PROTOBUF_GET_MS_OR_DEFAULT(config, request_timeout, 1000)) {}

ClientCertificateRestrictionFilter::ClientCertificateRestrictionFilter(
    ClientCertificateRestrictionConfigSharedPtr config,
    Upstream::ClusterManager &cm)
    : config_(config), cm_(cm) {}

Network::FilterStatus
ClientCertificateRestrictionFilter::onData(Buffer::Instance &, bool) {
  return has_been_authorized_ ? Network::FilterStatus::Continue
                              : Network::FilterStatus::StopIteration;
}

Network::FilterStatus ClientCertificateRestrictionFilter::onNewConnection() {
  return Network::FilterStatus::StopIteration;
}

void ClientCertificateRestrictionFilter::onEvent(
    Network::ConnectionEvent event) {
  if (event != Network::ConnectionEvent::Connected) {
    return;
  }

  auto &&connection{read_callbacks_->connection()};
  if (!connection.ssl()) {
    closeConnection();
    return;
  }

  // TODO(talnordan): Convert the serial number to colon-hex-encoded formatting.
  // TODO(talnordan): First call `connection.ssl()->peerCertificatePresented()`.
  std::string uri_san{connection.ssl()->uriSanPeerCertificate()};
  std::string serial_number{getSerialNumber()};
  if (uri_san.empty() || serial_number.empty()) {
    ENVOY_CONN_LOG(trace,
                   "client_certificate_restriction: Authorize REST not called",
                   connection);
    closeConnection();
    return;
  }

  // TODO(talnordan): Remove tracing.
  std::string payload{getPayload(config_->target(), uri_san, serial_number)};
  ENVOY_CONN_LOG(error, "client_certificate_restriction: payload is {}",
                 connection, payload);

  auto &&authorize_host{config_->authorizeHostname()};
  Http::MessagePtr request{getRequest(authorize_host, payload)};
  auto &&request_timeout{config_->requestTimeout()};

  auto &&authorize_cluster_name{config_->authorizeClusterName()};
  auto &&http_async_client{
      cm_.httpAsyncClientForCluster(authorize_cluster_name)};

  // TODO(talnordan): Own the return value for cancellation support.
  http_async_client.send(std::move(request), *this, request_timeout);
  status_ = Status::Calling;
}

void ClientCertificateRestrictionFilter::onSuccess(Http::MessagePtr &&m) {
  auto &&connection{read_callbacks_->connection()};
  std::string json{getBodyString(std::move(m))};
  ENVOY_CONN_LOG(trace,
                 "client_certificate_restriction: Authorize REST call "
                 "succeeded, status={}, body={}",
                 connection, m->headers().Status()->value().c_str(), json);
  status_ = Status::Complete;
  agent::connect::authorize::v1::AuthorizeResponse authorize_response;
  const auto status =
      Protobuf::util::JsonStringToMessage(json, &authorize_response);
  if (status.ok() && authorize_response.authorized()) {
    ENVOY_CONN_LOG(trace, "client_certificate_restriction: authorized",
                   connection);
    has_been_authorized_ = true;
    read_callbacks_->continueReading();
  } else {
    ENVOY_CONN_LOG(error, "client_certificate_restriction: unauthorized",
                   connection);
    closeConnection();
  }
}

void ClientCertificateRestrictionFilter::onFailure(
    Http::AsyncClient::FailureReason) {
  // TODO(talnordan): Log reason.
  auto &&connection{read_callbacks_->connection()};
  ENVOY_CONN_LOG(error,
                 "client_certificate_restriction: Authorize REST call failed",
                 connection);
  status_ = Status::Complete;
  closeConnection();
}

void ClientCertificateRestrictionFilter::closeConnection() {
  read_callbacks_->connection().close(Network::ConnectionCloseType::NoFlush);
}

std::string ClientCertificateRestrictionFilter::getSerialNumber() const {
  // TODO(talnordan): This is a PoC implementation that assumes the subtype of
  // the `Ssl::Connection` pointer.
  auto &&connection{read_callbacks_->connection()};
  Ssl::Connection *ssl{connection.ssl()};
  Ssl::SslSocket *ssl_socket = dynamic_cast<Ssl::SslSocket *>(ssl);
  if (ssl_socket == nullptr) {
    ENVOY_CONN_LOG(
        error, "client_certificate_restriction: unknown SSL connection type",
        connection);
    return "";
  }

  // TODO(talnordan): Avoid relying on the `rawSslForTest()` function.
  SSL *raw_ssl{ssl_socket->rawSslForTest()};
  bssl::UniquePtr<X509> cert(SSL_get_peer_certificate(raw_ssl));
  if (!cert) {
    ENVOY_CONN_LOG(
        error,
        "client_certificate_restriction: failed to retrieve peer certificate",
        connection);
    return "";
  }

  return getSerialNumber(cert.get());
}

std::string
ClientCertificateRestrictionFilter::getSerialNumber(const X509 *cert) {
  ASSERT(cert);
  ASN1_INTEGER *serial_number = X509_get_serialNumber(const_cast<X509 *>(cert));
  BIGNUM num_bn;
  BN_init(&num_bn);
  ASN1_INTEGER_to_BN(serial_number, &num_bn);
  char *char_serial_number = BN_bn2hex(&num_bn);
  BN_free(&num_bn);
  if (char_serial_number != nullptr) {
    std::string serial_number(char_serial_number);
    OPENSSL_free(char_serial_number);
    return serial_number;
  }
  return "";
}

std::string ClientCertificateRestrictionFilter::getPayload(
    const std::string &target, const std::string &client_cert_uri,
    const std::string &client_cert_serial) {
  agent::connect::authorize::v1::AuthorizePayload proto_payload{};
  proto_payload.set_target(target);
  proto_payload.set_clientcerturi(client_cert_uri);
  proto_payload.set_clientcertserial(client_cert_serial);

  return MessageUtil::getJsonStringFromMessage(proto_payload);
}

Http::MessagePtr
ClientCertificateRestrictionFilter::getRequest(const std::string &host,
                                               const std::string &payload) {
  Http::MessagePtr request(new Http::RequestMessageImpl());
  request->headers().insertContentType().value().setReference(
      Http::Headers::get().ContentTypeValues.Json);
  request->headers().insertPath().value().setReference(AUTHORIZE_PATH);
  request->headers().insertHost().value().setReference(host);
  request->headers().insertMethod().value().setReference(
      Http::Headers::get().MethodValues.Post);
  request->headers().insertContentLength().value(payload.length());
  request->body().reset(new Buffer::OwnedImpl(payload));
  return request;
}

std::string
ClientCertificateRestrictionFilter::getBodyString(Http::MessagePtr &&m) {
  Buffer::InstancePtr &body = m->body();
  return Buffer::BufferUtility::bufferToString(*body);
}

} // namespace ClientCertificateRestriction
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
