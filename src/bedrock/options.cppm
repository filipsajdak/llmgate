export module llmgate.bedrock_options;

import std;
import llmgate.doc;

export namespace llmgate::bedrock {

struct converse_options {
    [[= reflect::doc{"Models (upstream names) that take thinking on the Converse operation: Claude's thinking field is passed in additionalModelRequestFields. Every other model has thinking, output_config and context_management dropped and counted."}]]
    [[= reflect::example{R"(["anthropic.claude-sonnet-4-6-v1:0"])"}]] [[= reflect::commented]] std::vector<std::string> thinking_models{};
    [[= reflect::doc{"Remove the cachePoint blocks that Claude Code's cache_control markers become, for a model that refuses them."}]] [[= reflect::commented]] bool drop_cache_points = false;
};

struct options {
    [[= reflect::doc{"anthropic-beta tokens Claude Code may pass to Bedrock; any other token is dropped and counted. Empty means the built-in list of tokens Bedrock documents."}]]
    [[= reflect::example{R"(["interleaved-thinking-2025-05-14", "context-management-2025-06-27"])"}]] [[= reflect::commented]] std::vector<std::string> betas{};
    [[= reflect::doc{"Settings of the Converse operation, used for models listed with protocol bedrock_converse in model_protocols."}]] [[= reflect::commented]] converse_options converse{};
};

}  // namespace llmgate::bedrock
