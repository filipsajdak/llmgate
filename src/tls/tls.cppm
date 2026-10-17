module;

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

export module llmgate.tls;

import std;
import llmgate.reflect;

export namespace llmgate::tls {

enum class errc {
    context_failed = 1,
    connection_failed,
    handshake_failed,
    protocol_error,
    truncated,
    peer_closed,
};

enum class group_policy { post_quantum_required, post_quantum_preferred, classical_only };

enum class want { none, read, write };

struct io_result {
    std::size_t bytes;
    want next;
};

}  // namespace llmgate::tls

namespace llmgate::tls::detail {

constexpr std::string_view post_quantum_groups = "X25519MLKEM768:SecP256r1MLKEM768";
constexpr std::string_view classical_groups = "X25519:secp256r1:secp384r1";
constexpr std::string_view post_quantum_marker = "MLKEM";
constexpr std::array<char const*, 3> ca_bundle_candidates = {
    "/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt"};
constexpr std::array<unsigned char, 9> alpn_http11 = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr std::size_t error_text_size = 256;

struct ssl_ctx_deleter {
    void operator()(SSL_CTX* context) const noexcept { SSL_CTX_free(context); }
};

struct ssl_deleter {
    void operator()(SSL* session) const noexcept { SSL_free(session); }
};

std::string groups_for(group_policy const policy) {
    switch (policy) {
        case group_policy::post_quantum_required:
            return std::string{post_quantum_groups};
        case group_policy::post_quantum_preferred:
            return std::format("{}:{}", post_quantum_groups, classical_groups);
        case group_policy::classical_only:
            break;
    }
    return std::string{classical_groups};
}

struct bio_deleter {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};

struct x509_deleter {
    void operator()(X509* certificate) const noexcept { X509_free(certificate); }
};

struct pkey_deleter {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

using bio_ptr = std::unique_ptr<BIO, bio_deleter>;
using x509_ptr = std::unique_ptr<X509, x509_deleter>;
using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_deleter>;

bio_ptr memory_bio(std::string_view const text) {
    return bio_ptr{BIO_new_mem_buf(text.data(), static_cast<int>(text.size()))};
}

bool trust_pem(SSL_CTX* const context, std::string_view const pem) {
    auto const source = memory_bio(pem);
    auto* const store = SSL_CTX_get_cert_store(context);
    int added = 0;
    while (x509_ptr const certificate{PEM_read_bio_X509(source.get(), nullptr, nullptr, nullptr)}) {
        added += X509_STORE_add_cert(store, certificate.get());
    }
    ERR_clear_error();
    return added > 0;
}

bool configure_protocol(SSL_CTX* const context, group_policy const policy) {
    auto const groups = groups_for(policy);
    return SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) == 1 &&
           SSL_CTX_set1_groups_list(context, groups.c_str()) == 1;
}

bool load_identity(SSL_CTX* const context, std::string_view const certificate_pem, std::string_view const private_key_pem) {
    auto const certificate_source = memory_bio(certificate_pem);
    auto const key_source = memory_bio(private_key_pem);
    x509_ptr const certificate{PEM_read_bio_X509(certificate_source.get(), nullptr, nullptr, nullptr)};
    pkey_ptr const key{PEM_read_bio_PrivateKey(key_source.get(), nullptr, nullptr, nullptr)};
    return certificate && key && SSL_CTX_use_certificate(context, certificate.get()) == 1 &&
           SSL_CTX_use_PrivateKey(context, key.get()) == 1 && SSL_CTX_check_private_key(context) == 1;
}

bool configure_trust(SSL_CTX* const context, std::string_view const extra_trusted_pem) {
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(context);
    for (auto const* const bundle : ca_bundle_candidates) {
        SSL_CTX_load_verify_file(context, bundle);
    }
    return extra_trusted_pem.empty() || trust_pem(context, extra_trusted_pem);
}

std::string drain_errors() {
    std::string text;
    while (auto const code = ERR_get_error()) {
        std::array<char, error_text_size> line{};
        ERR_error_string_n(code, line.data(), line.size());
        text += std::string{line.data()} + "; ";
    }
    return text;
}

}  // namespace llmgate::tls::detail

export namespace std {
template <>
struct is_error_code_enum<llmgate::tls::errc> : true_type {};
}  // namespace std

export namespace llmgate::tls {

std::error_code make_error_code(errc const value) noexcept {
    return reflect::make_code(value);
}

class client_context {
public:
    static std::expected<client_context, std::error_code> make(group_policy const policy,
                                                               std::string_view const extra_trusted_pem = {}) {
        client_context context{SSL_CTX_new(TLS_client_method())};
        auto* const native = context.native_.get();
        auto const configured = native != nullptr && detail::configure_protocol(native, policy) &&
                                SSL_CTX_set_alpn_protos(native, detail::alpn_http11.data(), detail::alpn_http11.size()) == 0 &&
                                detail::configure_trust(native, extra_trusted_pem);
        ERR_clear_error();
        if (!configured) {
            return std::unexpected{make_error_code(errc::context_failed)};
        }
        SSL_CTX_set_mode(native, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        return context;
    }

    SSL_CTX* native() const noexcept { return native_.get(); }

private:
    explicit client_context(SSL_CTX* native) noexcept : native_{native} {}

    std::unique_ptr<SSL_CTX, detail::ssl_ctx_deleter> native_;
};

class server_context {
public:
    static std::expected<server_context, std::error_code> make(group_policy const policy,
                                                               std::string_view const certificate_pem,
                                                               std::string_view const private_key_pem) {
        server_context context{SSL_CTX_new(TLS_server_method())};
        auto* const native = context.native_.get();
        auto const configured = native != nullptr && detail::configure_protocol(native, policy) &&
                                detail::load_identity(native, certificate_pem, private_key_pem);
        ERR_clear_error();
        if (!configured) {
            return std::unexpected{make_error_code(errc::context_failed)};
        }
        return context;
    }

    SSL_CTX* native() const noexcept { return native_.get(); }

private:
    explicit server_context(SSL_CTX* native) noexcept : native_{native} {}

    std::unique_ptr<SSL_CTX, detail::ssl_ctx_deleter> native_;
};

class connection {
public:
    static std::expected<connection, std::error_code> make(client_context const& context, std::string const& host) {
        auto created = attach(SSL_new(context.native()));
        if (!created) {
            return created;
        }
        auto* const session = created->session_.get();
        SSL_set_connect_state(session);
        if (SSL_set_tlsext_host_name(session, host.c_str()) != 1 || SSL_set1_host(session, host.c_str()) != 1) {
            ERR_clear_error();
            return std::unexpected{make_error_code(errc::connection_failed)};
        }
        return created;
    }

    static std::expected<connection, std::error_code> accept(server_context const& context) {
        auto created = attach(SSL_new(context.native()));
        if (created) {
            SSL_set_accept_state(created->session_.get());
        }
        return created;
    }

    void shutdown() noexcept { SSL_shutdown(session_.get()); }

    std::expected<want, std::error_code> handshake() {
        auto const result = SSL_do_handshake(session_.get());
        if (result == 1) {
            return want::none;
        }
        return classify(result).transform([](io_result const& wanted) { return wanted.next; });
    }

    std::expected<io_result, std::error_code> read(std::span<std::byte> destination) {
        std::size_t produced = 0;
        if (SSL_read_ex(session_.get(), destination.data(), destination.size(), &produced) == 1) {
            return io_result{.bytes = produced, .next = want::none};
        }
        return classify(0);
    }

    std::expected<io_result, std::error_code> write(std::span<std::byte const> source) {
        std::size_t consumed = 0;
        if (SSL_write_ex(session_.get(), source.data(), source.size(), &consumed) == 1) {
            return io_result{.bytes = consumed, .next = want::none};
        }
        return classify(0);
    }

    std::expected<void, std::error_code> feed(std::span<std::byte const> ciphertext) {
        std::size_t written = 0;
        if (BIO_write_ex(incoming_, ciphertext.data(), ciphertext.size(), &written) != 1 || written != ciphertext.size()) {
            return std::unexpected{make_error_code(errc::protocol_error)};
        }
        return {};
    }

    void feed_eof() noexcept { BIO_set_mem_eof_return(incoming_, 0); }

    std::vector<std::byte> take_outgoing() {
        std::vector<std::byte> bytes(BIO_ctrl_pending(outgoing_));
        std::size_t read_back = 0;
        BIO_read_ex(outgoing_, bytes.data(), bytes.size(), &read_back);
        bytes.resize(read_back);
        return bytes;
    }

    std::string_view negotiated_group() const noexcept {
        if (SSL_is_init_finished(session_.get()) != 1) {
            return {};
        }
        auto const* const name = SSL_get0_group_name(session_.get());
        return name == nullptr ? std::string_view{} : std::string_view{name};
    }

    bool post_quantum() const noexcept { return negotiated_group().contains(detail::post_quantum_marker); }

    std::string_view version() const noexcept { return SSL_get_version(session_.get()); }

    std::string const& diagnostic() const noexcept { return diagnostic_; }

private:
    explicit connection(SSL* session) noexcept : session_{session} {}

    static std::expected<connection, std::error_code> attach(SSL* const raw) {
        connection created{raw};
        auto* const session = created.session_.get();
        if (session == nullptr) {
            return std::unexpected{make_error_code(errc::connection_failed)};
        }
        created.incoming_ = BIO_new(BIO_s_mem());
        created.outgoing_ = BIO_new(BIO_s_mem());
        if (created.incoming_ == nullptr || created.outgoing_ == nullptr) {
            BIO_free(created.incoming_);
            BIO_free(created.outgoing_);
            return std::unexpected{make_error_code(errc::connection_failed)};
        }
        SSL_set_bio(session, created.incoming_, created.outgoing_);
        return created;
    }

    std::expected<io_result, std::error_code> classify(int const result) {
        switch (SSL_get_error(session_.get(), result)) {
            case SSL_ERROR_WANT_READ:
                return io_result{.bytes = 0, .next = want::read};
            case SSL_ERROR_WANT_WRITE:
                return io_result{.bytes = 0, .next = want::write};
            case SSL_ERROR_ZERO_RETURN:
                return std::unexpected{make_error_code(errc::peer_closed)};
            default:
                break;
        }
        auto const truncated = ERR_GET_REASON(ERR_peek_last_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING;
        diagnostic_ = detail::drain_errors();
        return std::unexpected{make_error_code(truncated ? errc::truncated : errc::handshake_failed)};
    }

    std::unique_ptr<SSL, detail::ssl_deleter> session_;
    BIO* incoming_ = nullptr;
    BIO* outgoing_ = nullptr;
    std::string diagnostic_;
};

}  // namespace llmgate::tls
