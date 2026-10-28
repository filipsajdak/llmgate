export module llmgate.chat_quirks;

import std;
import llmgate.doc;

export namespace llmgate::chat {

struct quirks {
    [[= reflect::doc{"Send the output limit as max_completion_tokens instead of max_tokens, which the reasoning models of OpenAI and Azure OpenAI require."}]] [[= reflect::commented]] bool use_max_completion_tokens = false;
    [[= reflect::doc{"Leave stop sequences out of the request, for models that refuse the stop field."}]] [[= reflect::commented]] bool drop_stop_sequences = false;
    [[= reflect::doc{"Leave temperature and top_p out of the request, for models that refuse any value but their default."}]] [[= reflect::commented]] bool drop_sampling = false;
    [[= reflect::doc{"Leave reasoning_effort out of the request, for models and servers that do not know it. Claude Code asks for thinking on every request, so a model that does not reason needs this."}]] [[= reflect::commented]] bool drop_reasoning_effort = false;
    [[= reflect::doc{"Leave reasoning_effort out of a request that carries tools, which some reasoning models refuse on the chat-completions endpoint (they ask for the Responses endpoint or an effort of none)."}]] [[= reflect::commented]] bool drop_reasoning_effort_with_tools = false;
};

}  // namespace llmgate::chat
