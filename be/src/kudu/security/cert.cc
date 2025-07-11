// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/security/cert.h"

#include <openssl/asn1.h>
#include <openssl/crypto.h>
#include <openssl/obj_mac.h>
#include <openssl/objects.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>

#include <glog/logging.h>

#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/macros.h"
#include "kudu/security/crypto.h"
#include "kudu/util/openssl_util.h"
#include "kudu/util/openssl_util_bio.h"
#include "kudu/util/status.h"

using std::nullopt;
using std::optional;
using std::string;
using std::vector;
using strings::Substitute;

namespace kudu {
namespace security {

template<> struct SslTypeTraits<GENERAL_NAMES> {
  static constexpr auto kFreeFunc = &GENERAL_NAMES_free;
};

// This OID is generated via the UUID method.
static const char* kKuduKerberosPrincipalOidStr = "2.25.243346677289068076843480765133256509912";

string X509NameToString(X509_NAME* name) {
  SCOPED_OPENSSL_NO_PENDING_ERRORS;
  CHECK(name);
  auto bio = ssl_make_unique(BIO_new(BIO_s_mem()));
  OPENSSL_CHECK(bio, "could not create memory BIO");
  OPENSSL_CHECK_OK(X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_ONELINE));

  BUF_MEM* membuf;
  OPENSSL_CHECK_OK(BIO_get_mem_ptr(bio.get(), &membuf));
  return string(membuf->data, membuf->length);
}

int GetKuduKerberosPrincipalOidNid() {
  InitializeOpenSSL();
  static std::once_flag flag;
  static int nid;
  std::call_once(flag, [&] () {
      nid = OBJ_create(kKuduKerberosPrincipalOidStr, "kuduPrinc", "kuduKerberosPrincipal");
      CHECK_NE(nid, NID_undef) << "failed to create kuduPrinc oid: " << GetOpenSSLErrors();
  });
  return nid;
}

X509* Cert::GetTopOfChainX509() const {
  CHECK_GT(chain_len(), 0);
  return sk_X509_value(data_.get(), 0);
}

Status Cert::FromString(const std::string& data, DataFormat format) {
  RETURN_NOT_OK(::kudu::security::FromString(data, format, &data_));
  if (sk_X509_num(data_.get()) < 1) {
    return Status::RuntimeError("Certificate chain is empty. Expected at least one certificate.");
  }
  return Status::OK();
}

Status Cert::ToString(std::string* data, DataFormat format) const {
  return ::kudu::security::ToString(data, format, data_.get());
}

Status Cert::FromFile(const std::string& fpath, DataFormat format) {
  RETURN_NOT_OK(::kudu::security::FromFile(fpath, format, &data_));
  if (sk_X509_num(data_.get()) < 1) {
    return Status::RuntimeError("Certificate chain is empty. Expected at least one certificate.");
  }
  return Status::OK();
}

string Cert::SubjectName() const {
  return X509NameToString(X509_get_subject_name(GetTopOfChainX509()));
}

string Cert::IssuerName() const {
  return X509NameToString(X509_get_issuer_name(GetTopOfChainX509()));
}

optional<string> Cert::UserId() const {
  SCOPED_OPENSSL_NO_PENDING_ERRORS;
  X509_NAME* name = X509_get_subject_name(GetTopOfChainX509());
  char buf[1024];
  int len = X509_NAME_get_text_by_NID(name, NID_userId, buf, arraysize(buf));
  if (len < 0) {
    return nullopt;
  }
  return string(buf, len);
}

vector<string> Cert::Hostnames() const {
  SCOPED_OPENSSL_NO_PENDING_ERRORS;
  vector<string> result;
  auto gens = ssl_make_unique(reinterpret_cast<GENERAL_NAMES*>(X509_get_ext_d2i(
      GetTopOfChainX509(), NID_subject_alt_name, nullptr, nullptr)));
  if (gens) {
    for (int i = 0; i < sk_GENERAL_NAME_num(gens.get()); ++i) {
      GENERAL_NAME* gen = sk_GENERAL_NAME_value(gens.get(), i);
      if (gen->type != GEN_DNS) {
        continue;
      }
      const ASN1_STRING* cstr = gen->d.dNSName;
      if (cstr->type != V_ASN1_IA5STRING || cstr->data == nullptr) {
        LOG(DFATAL) << "invalid DNS name in the SAN field";
        return {};
      }
      result.emplace_back(reinterpret_cast<char*>(cstr->data), cstr->length);
    }
  }
  return result;
}

optional<string> Cert::KuduKerberosPrincipal() const {
  SCOPED_OPENSSL_NO_PENDING_ERRORS;
  int idx = X509_get_ext_by_NID(GetTopOfChainX509(), GetKuduKerberosPrincipalOidNid(), -1);
  if (idx < 0) {
    return nullopt;
  }
  X509_EXTENSION* ext = X509_get_ext(GetTopOfChainX509(), idx);
  ASN1_OCTET_STRING* octet_str = X509_EXTENSION_get_data(ext);
  const unsigned char* octet_str_data = octet_str->data;
  long len; // NOLINT
  int tag, xclass;
  if (ASN1_get_object(&octet_str_data, &len, &tag, &xclass, octet_str->length) != 0 ||
      tag != V_ASN1_UTF8STRING) {
    LOG(DFATAL) << "invalid extension value in cert " << SubjectName();
    return nullopt;
  }

  return string(reinterpret_cast<const char*>(octet_str_data), len);
}

Status Cert::CheckKeyMatch(const PrivateKey& key) const {
  SCOPED_OPENSSL_NO_PENDING_ERRORS;
  OPENSSL_RET_NOT_OK(X509_check_private_key(GetTopOfChainX509(), key.GetRawData()),
                     "certificate does not match private key");
  return Status::OK();
}

Status Cert::GetSignatureHashAlgorithm(int* digest_nid_out) const {
  SCOPED_OPENSSL_NO_PENDING_ERRORS;
  // Find the signature type of the certificate. This corresponds to the digest
  // (hash) algorithm, and the public key type which signed the cert.

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
  int signature_nid = X509_get_signature_nid(GetTopOfChainX509());
#else
  // Older version of OpenSSL appear not to have a public way to get the
  // signature digest method from a certificate. Instead, we reach into the
  // 'private' internals.
  int signature_nid = OBJ_obj2nid(GetTopOfChainX509()->sig_alg->algorithm);
#endif

  // Retrieve the digest algorithm type.
  //
  // Prefer OpenSSL 1.1.1's X509_get_signature_info when available, as it has extra
  // handling to determine the hash algorithm for signature types that store the hash
  // algorithm separately (e.g. RSASSA-PSS).
  int digest_nid;
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
  X509_get_signature_info(GetTopOfChainX509(), &digest_nid, nullptr, nullptr, nullptr);
#else
  // Return a better error message for RSASSA-PSS, because we know that
  // OBJ_find_sigid_algs() won't handle it properly.
  if (PREDICT_FALSE(signature_nid == NID_rsassaPss)) {
    return Status::NotSupported("Server certificate uses an RSASSA-PSS signature. "
        "RSASSA-PSS signatures are not supported for OpenSSL < 1.1.1");
  }
  OBJ_find_sigid_algs(signature_nid, &digest_nid, nullptr);
#endif

  // RFC 5929: if the certificate's signatureAlgorithm uses no hash functions or
  // uses multiple hash functions, then this channel binding type's channel
  // bindings are undefined at this time (updates to is channel binding type may
  // occur to address this issue if it ever arises).
  //
  // TODO(dan): can the multiple hash function scenario actually happen? What
  // does OBJ_find_sigid_algs do in that scenario?
  if (PREDICT_FALSE(digest_nid == NID_undef)) {
    std::string signature_type(OBJ_nid2ln(signature_nid));
    return Status::NotSupported(Substitute(
        "server certificate using '$0' signature algorithm has no signature "
        "digest (hash) algorithm", signature_type));
  }

  *digest_nid_out = digest_nid;
  return Status::OK();
}

Status Cert::GetServerEndPointChannelBindings(string* channel_bindings) const {
  SCOPED_OPENSSL_NO_PENDING_ERRORS;

  // Get the hash algorithm used for the signature
  int digest_nid;
  RETURN_NOT_OK(GetSignatureHashAlgorithm(&digest_nid));
  DCHECK_NE(digest_nid, NID_undef);

  // RFC 5929: if the certificate's signatureAlgorithm uses a single hash
  // function, and that hash function is either MD5 [RFC1321] or SHA-1
  // [RFC3174], then use SHA-256 [FIPS-180-3];
  if (digest_nid == NID_md5 || digest_nid == NID_sha1) {
    digest_nid = NID_sha256;
  }

  const EVP_MD* md = EVP_get_digestbynid(digest_nid);
  OPENSSL_RET_IF_NULL(md, "digest for nid not found");

  // Create a digest BIO. All data written to the BIO will be sent through the
  // digest (hash) function. The digest BIO requires a null BIO to writethrough to.
  auto null_bio = ssl_make_unique(BIO_new(BIO_s_null()));
  OPENSSL_RET_IF_NULL(null_bio, "could not create null BIO");
  auto md_bio = ssl_make_unique(BIO_new(BIO_f_md()));
  OPENSSL_RET_IF_NULL(md_bio, "could not create message digest BIO");
  OPENSSL_RET_NOT_OK(BIO_set_md(md_bio.get(), md), "failed to set digest for BIO");
  BIO_push(md_bio.get(), null_bio.get());

  // Write the cert to the digest BIO.
  RETURN_NOT_OK(ToBIO(md_bio.get(), DataFormat::DER, data_.get()));

  // Read the digest from the BIO and append it to 'channel_bindings'.
  char buf[EVP_MAX_MD_SIZE];
  int digest_len = BIO_gets(md_bio.get(), buf, sizeof(buf));
  OPENSSL_RET_NOT_OK(digest_len, "failed to get cert digest from BIO");
  channel_bindings->assign(buf, digest_len);
  return Status::OK();
}

void Cert::AdoptAndAddRefRawData(RawDataType* data) {
  DCHECK_EQ(sk_X509_num(data), 1);
  X509* cert = sk_X509_value(data, sk_X509_num(data) - 1);

  DCHECK(cert);
#if OPENSSL_VERSION_NUMBER < 0x10100000L
  CHECK_GT(CRYPTO_add(&cert->references, 1, CRYPTO_LOCK_X509), 1) << "X509 use-after-free detected";
#else
  OPENSSL_CHECK_OK(X509_up_ref(cert)) << "X509 use-after-free detected: " << GetOpenSSLErrors();
#endif
  // We copy the STACK_OF() object, but the copy and the original both internally point to the
  // same elements.
  AdoptRawData(sk_X509_dup(data));
}

void Cert::AdoptX509(X509* cert) {
  // Free current STACK_OF(X509).
  sk_X509_pop_free(data_.get(), X509_free);
  // Allocate new STACK_OF(X509) and populate with 'cert'.
  STACK_OF(X509)* sk = sk_X509_new_null();
  DCHECK(sk);
  sk_X509_push(sk, cert);
  AdoptRawData(sk);
}

void Cert::AdoptAndAddRefX509(X509* cert) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
  CHECK_GT(CRYPTO_add(&cert->references, 1, CRYPTO_LOCK_X509), 1) << "X509 use-after-free detected";
#else
  OPENSSL_CHECK_OK(X509_up_ref(cert)) << "X509 use-after-free detected: " << GetOpenSSLErrors();
#endif
  AdoptX509(cert);
}

Status Cert::GetPublicKey(PublicKey* key) const {
  SCOPED_OPENSSL_NO_PENDING_ERRORS;
  EVP_PKEY* raw_key = X509_get_pubkey(GetTopOfChainX509());
  OPENSSL_RET_IF_NULL(raw_key, "unable to get certificate public key");
  key->AdoptRawData(raw_key);
  return Status::OK();
}

Status CertSignRequest::FromString(const std::string& data, DataFormat format) {
  return ::kudu::security::FromString(data, format, &data_);
}

Status CertSignRequest::ToString(std::string* data, DataFormat format) const {
  return ::kudu::security::ToString(data, format, data_.get());
}

Status CertSignRequest::FromFile(const std::string& fpath, DataFormat format) {
  return ::kudu::security::FromFile(fpath, format, &data_);
}

CertSignRequest CertSignRequest::Clone() const {
  X509_REQ* cloned_req;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
  CHECK_GT(CRYPTO_add(&data_->references, 1, CRYPTO_LOCK_X509_REQ), 1)
    << "X509_REQ use-after-free detected";
  cloned_req = GetRawData();
#else
  // With OpenSSL 1.1, data structure internals are hidden, and there doesn't
  // seem to be a public method that increments data_'s refcount.
  cloned_req = X509_REQ_dup(GetRawData());
  CHECK(cloned_req != nullptr)
    << "X509 allocation failure detected: " << GetOpenSSLErrors();
#endif

  CertSignRequest clone;
  clone.AdoptRawData(cloned_req);
  return clone;
}

Status CertSignRequest::GetPublicKey(PublicKey* key) const {
  SCOPED_OPENSSL_NO_PENDING_ERRORS;
  EVP_PKEY* raw_key = X509_REQ_get_pubkey(data_.get());
  OPENSSL_RET_IF_NULL(raw_key, "unable to get CSR public key");
  key->AdoptRawData(raw_key);
  return Status::OK();
}

} // namespace security
} // namespace kudu
