import llmgate.config;
import llmgate.config_docs;
import llmgate.json;
import std;

namespace {

using namespace llmgate;

constexpr char example_bytes[] = {
#embed LLMGATE_EXAMPLE_CONFIG
};

constexpr std::string_view example{example_bytes, sizeof example_bytes};

constexpr bool example_is_current() {
    auto const parsed = json::parse(example);
    auto const shape = config_docs::describe<gateway::config>();
    return parsed.has_value() && config_docs::conforms(*parsed, shape) && config_docs::every_key_is_shown(shape, example);
}

static_assert(example_is_current(), "config.example.jsonc is stale: run tools/update_config_docs.sh");

}  // namespace

int main() {}
