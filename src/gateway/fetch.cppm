export module llmgate.fetch;

import std;
import llmgate.async;
import llmgate.http;
import llmgate.httpio;
import llmgate.net;
import llmgate.routing;
import llmgate.tls;

export namespace llmgate::fetch {

struct reply {
    int status;
    std::string body;
};

async::task<std::expected<reply, std::error_code>> request(async::reactor& loop, tls::client_context const& tls_context,
                                                           std::string base_url, httpio::request_spec spec,
                                                           std::chrono::milliseconds limit);

}  // namespace llmgate::fetch
