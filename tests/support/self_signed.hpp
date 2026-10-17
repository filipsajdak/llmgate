#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace llmgate::testing {

struct credentials {
    std::string certificate_pem;
    std::string private_key_pem;
};

namespace detail {

constexpr long valid_for_seconds = 3600;

struct free_bio {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};
struct free_key {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};
struct free_x509 {
    void operator()(X509* certificate) const noexcept { X509_free(certificate); }
};

inline std::string drain(BIO* bio) {
    char* data = nullptr;
    auto const size = BIO_get_mem_data(bio, &data);
    return std::string{data, static_cast<std::size_t>(size)};
}

}  // namespace detail

inline credentials make_self_signed(std::string const& dns_name) {
    std::unique_ptr<EVP_PKEY, detail::free_key> key{EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256")};
    std::unique_ptr<X509, detail::free_x509> certificate{X509_new()};
    X509_set_version(certificate.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(certificate.get()), detail::valid_for_seconds);
    X509_set_pubkey(certificate.get(), key.get());
    auto* const name = X509_get_subject_name(certificate.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<unsigned char const*>(dns_name.c_str()), -1, -1, 0);
    X509_set_issuer_name(certificate.get(), name);
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
    auto const alternative = "DNS:" + dns_name;
    std::unique_ptr<X509_EXTENSION, decltype([](X509_EXTENSION* e) { X509_EXTENSION_free(e); })> const extension{
        X509V3_EXT_nconf_nid(nullptr, &context, NID_subject_alt_name, alternative.c_str())};
    X509_add_ext(certificate.get(), extension.get(), -1);
    X509_sign(certificate.get(), key.get(), EVP_sha256());

    std::unique_ptr<BIO, detail::free_bio> certificate_out{BIO_new(BIO_s_mem())};
    std::unique_ptr<BIO, detail::free_bio> key_out{BIO_new(BIO_s_mem())};
    PEM_write_bio_X509(certificate_out.get(), certificate.get());
    PEM_write_bio_PrivateKey(key_out.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    return {.certificate_pem = detail::drain(certificate_out.get()), .private_key_pem = detail::drain(key_out.get())};
}

}  // namespace llmgate::testing
