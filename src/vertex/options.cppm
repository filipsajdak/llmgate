export module llmgate.vertex_options;

import std;
import llmgate.doc;

export namespace llmgate::vertex {

struct options {
    [[= reflect::doc{"anthropic-beta tokens Claude Code may pass to Vertex; any other token is dropped and counted. Empty means the built-in list of tokens Vertex accepts."}]]
    [[= reflect::example{R"(["interleaved-thinking-2025-05-14", "context-1m-2025-08-07"])"}]] [[= reflect::commented]] std::vector<std::string> betas{};
    [[= reflect::doc{"Turn a tool_choice of any or tool into auto, for a model that refuses forced tool use."}]] [[= reflect::commented]] bool drop_forced_tool_choice = false;
};

}  // namespace llmgate::vertex
