# Ordinary chat contract v1

`chat` accepts contract_version1,op chat,question nonempty<=2048 UTF-8 bytes.
It uses the shared exact rendered-prompt budget/provider pin/session/generation
policy and requires no Sources command. Returns model answer, empty citations and
admitted_evidence, usage/context_policy/continuation and acceptance diagnostics.
No model prose becomes literal evidence; source citations are never invented by
this path. Unknown fields refuse before dependencies.

`submit_chat` accepts conversation_id,expected_revision,operation_id,question.
Same bounded transactional admission/messages/runs/events/retry/recovery as document
submit. Success commits selected_scope {kind:chat}, no admitted source evidence,
semantic history and provider reference. An ordinary `submit` without selector can
follow a committed chat scope. Explicit document selectors switch back to document
policy. Switching to chat is explicit; failed switch retains prior good scope,
evidence and continuation. Operation normalization binds chat scope distinctly.

Cold restart rebuilds from Agent semantic messages, independent of model files or
client state. Hot continuation uses only public Inference handles. Explicit compact
retains original messages, derives bounded summary, resets session handle and
keeps provider/context pin. It starts zero source reading. Models remain optional:
missing Inference returns a durable failed run without replacing good context.

Current request/state/response/session limits are the existing conversation-v1 and
question-v1 contracts. Native fixture tests cover chat/document switches, failed
switch, offline exact retry, compaction and prompt/history inspection. Real native
provider tests separately check arithmetic4 and remembered7319 hot/cold/compact;
these are bounded quality fixtures, not universal model accuracy claims.
