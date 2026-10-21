export module llmgate.discovery;

import std;
import llmgate.async;
import llmgate.fetch;
import llmgate.gateway;
import llmgate.tls;

export namespace llmgate::discovery {

async::task<std::expected<fetch::reply, std::string>> fetch_models(async::reactor& loop, tls::client_context const& tls_context, gateway::provider_config const& provider,
                                                                     gateway::credential& credential);

}  // namespace llmgate::discovery
