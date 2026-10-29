export module llmgate.bedrock_options;

import std;
import llmgate.doc;

export namespace llmgate::bedrock {

struct options {
    [[= reflect::doc{"anthropic-beta tokens Claude Code may pass to Bedrock; any other token is dropped and counted. Empty means the built-in list of tokens Bedrock documents."}]]
    [[= reflect::example{R"(["interleaved-thinking-2025-05-14", "context-management-2025-06-27"])"}]] [[= reflect::commented]] std::vector<std::string> betas{};
};

}  // namespace llmgate::bedrock
