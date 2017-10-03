#pragma once

#include <string>
#include "envoy/http/header_map.h"
#include "openssl/sha.h"
#include "envoy/buffer/buffer.h"


namespace Solo {
namespace Lambda {

class AwsAuthenticator {
public:
  AwsAuthenticator(std::string&& access_key, std::string&& secret_key, std::string&& service);  
  
  ~AwsAuthenticator();

  void update_payload_hash(const Envoy::Buffer::Instance& data);

  void sign(Envoy::Http::HeaderMap* request_headers, std::list<Envoy::Http::LowerCaseString>&& headers_to_sign,const std::string& region);

private:
//  void lambdafy();
  const Envoy::Http::HeaderEntry* get_maybe_inline_header(Envoy::Http::HeaderMap* request_headers, const Envoy::Http::LowerCaseString& im);

  static bool lowercasecompare(const Envoy::Http::LowerCaseString& i,const Envoy::Http::LowerCaseString& j);

  std::string access_key_;
  std::string first_key_;
  std::string service_;
  std::string host_;


  SHA256_CTX body_sha_;
  
  
  static const std::string ALGORITHM;
  
};

} // Lambda
} // Solo