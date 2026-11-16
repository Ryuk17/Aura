/* Aura — orchestrator stage（实现） */
#include "agent/stages/orchestrator_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/flow/flow_stage.h"
#include "agent/stages/stage_proto.h"
#include "utils/logger/logger.h"

#define TAG "orchestrator"

#define AURA_ORCH_MAX_TOOL_ROUNDS 2
#define AURA_ORCH_INTENT_MAX      24
#define AURA_ORCH_TOOL_MAX        32
#define AURA_ORCH_TEXT_MAX        400
#define AURA_ORCH_ARGS_MAX        256
#define AURA_ORCH_SYSTEM_MAX      384

typedef struct aura_orch_turn {
    bool            active;
    aura_flow_ctx_t ctx;
    char            intent[AURA_ORCH_INTENT_MAX];
    char            text[AURA_ORCH_TEXT_MAX];
    char            tool[AURA_ORCH_TOOL_MAX];
    char            args[AURA_ORCH_ARGS_MAX];
    uint32_t        tool_round;
    bool            waiting_tool;
    uint32_t        pending_call_id;
} aura_orch_turn_t;

struct aura_orchestrator_stage {
    aura_flow_bus_t        *bus;
    aura_session_runtime_t *rt;
    aura_agent_memory_t    *memory;
    aura_llm_backend_t      backend;
    char                    system_prompt[AURA_ORCH_SYSTEM_MAX];
    aura_flow_stage_t      *stage;
    aura_orch_turn_t        turn;
    uint32_t                next_call_id;
};

static void sanitize_line(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    if (in == NULL) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; in[i] != '\0' && j + 1 < cap; i++) {
        char c   = in[i];
        out[j++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    out[j] = '\0';
}

static void reset_turn(aura_orchestrator_stage_t *s)
{
    memset(&s->turn, 0, sizeof(s->turn));
}

static void publish_error(aura_orchestrator_stage_t *s, const aura_flow_ctx_t *ctx,
                          const char *text)
{
    (void)aura_stage_publish(s->bus, "error.runtime", AURA_FLOW_ERROR, ctx, text);
}

static void publish_reply(aura_orchestrator_stage_t *s, const char *text)
{
    char safe[AURA_LLM_TEXT_MAX];
    sanitize_line(text, safe, sizeof(safe));
    if (safe[0] == '\0') {
        snprintf(safe, sizeof(safe), "好的");
    }
    (void)aura_stage_publish(s->bus, AURA_TOPIC_TTS_SAY, AURA_FLOW_TEXT, &s->turn.ctx, safe);
    AURA_LOGI(TAG, "reply: %s", safe);

    if (s->memory != NULL) {
        (void)aura_agent_memory_add(s->memory, s->turn.ctx.session_id, s->turn.ctx.speaker_id,
                                    "assistant", safe);
    }
    char payload[640];
    snprintf(payload, sizeof(payload), "role=assistant\ntext=%s\n", safe);
    (void)aura_stage_publish(s->bus, AURA_TOPIC_MEMORY_WRITE, AURA_FLOW_MEMORY_OP,
                             &s->turn.ctx, payload);
}

static void start_tool_call(aura_orchestrator_stage_t *s);

static void generate_reply(aura_orchestrator_stage_t *s, const char *tool_name,
                           const char *tool_result)
{
    if (!s->turn.active) {
        return;
    }
    if (aura_flow_is_cancelled(s->bus, &s->turn.ctx)) {
        reset_turn(s);
        return;
    }

    char memory_text[512];
    memory_text[0] = '\0';
    if (s->memory != NULL) {
        if (aura_agent_memory_recent(s->memory, s->turn.ctx.session_id,
                                     s->turn.ctx.speaker_id, memory_text,
                                     sizeof(memory_text)) != AURA_OK) {
            memory_text[0] = '\0';
        }
    }

    aura_llm_request_t req;
    memset(&req, 0, sizeof(req));
    req.ctx           = &s->turn.ctx;
    req.system_prompt = s->system_prompt;
    req.user_text     = s->turn.text;
    req.intent        = s->turn.intent;
    req.memory        = memory_text;
    req.tool_name     = tool_name;
    req.tool_result   = tool_result;

    aura_llm_response_t resp;
    memset(&resp, 0, sizeof(resp));
    aura_err_t rc = (s->backend.generate != NULL)
                        ? s->backend.generate(s->backend.user, &req, &resp)
                        : AURA_ERR_UNSUPPORTED;
    if (rc != AURA_OK) {
        publish_error(s, &s->turn.ctx, aura_strerror(rc));
        reset_turn(s);
        return;
    }
    if (aura_flow_is_cancelled(s->bus, &s->turn.ctx)) {
        reset_turn(s);
        return;
    }

    /* LLM 请求工具：进入下一轮 tool.call（受 AURA_ORCH_MAX_TOOL_ROUNDS 限制）。 */
    if (resp.tool_name[0] != '\0') {
        if (s->turn.tool_round >= AURA_ORCH_MAX_TOOL_ROUNDS) {
            publish_reply(s, "抱歉，工具调用次数过多。");
            reset_turn(s);
            return;
        }
        snprintf(s->turn.tool, sizeof(s->turn.tool), "%s", resp.tool_name);
        snprintf(s->turn.args, sizeof(s->turn.args), "%s", resp.tool_args);
        start_tool_call(s);
        return;
    }

    publish_reply(s, resp.text);
}

static void start_tool_call(aura_orchestrator_stage_t *s)
{
    if (!s->turn.active || s->turn.tool[0] == '\0') {
        return;
    }
    if (s->turn.tool_round >= AURA_ORCH_MAX_TOOL_ROUNDS) {
        publish_reply(s, "抱歉，工具调用次数过多。");
        reset_turn(s);
        return;
    }

    uint32_t call_id       = ++s->next_call_id;
    s->turn.pending_call_id = call_id;
    s->turn.waiting_tool    = true;
    s->turn.tool_round++;

    char payload[AURA_ORCH_ARGS_MAX + 96];
    snprintf(payload, sizeof(payload), "name=%s\ncall_id=c%u\n%s", s->turn.tool, call_id,
             s->turn.args);
    (void)aura_stage_publish(s->bus, AURA_TOPIC_TOOL_CALL, AURA_FLOW_TOOL_CALL, &s->turn.ctx,
                             payload);
    AURA_LOGI(TAG, "tool call: %s (call_id=c%u)", s->turn.tool, call_id);
}

static void on_dialogue_intent(aura_orchestrator_stage_t *s, aura_flow_msg_t *msg)
{
    aura_stage_kv_t kvs[AURA_STAGE_KV_MAX];
    uint32_t        n      = aura_stage_kv_parse(msg->payload, kvs, AURA_STAGE_KV_MAX);
    const char     *intent = aura_stage_kv_get(kvs, n, "intent");
    const char     *text   = aura_stage_kv_get(kvs, n, "text");
    const char     *tool   = aura_stage_kv_get(kvs, n, "tool");

    if (intent == NULL || text == NULL) {
        return;
    }

    aura_flow_ctx_t ctx;
    if (msg->ctx.session_id != 0) {
        ctx = msg->ctx;
    } else if (!aura_session_runtime_ensure_turn(s->rt, &ctx)) {
        return;
    }

    if (!s->turn.active || s->turn.ctx.session_id != ctx.session_id ||
        s->turn.ctx.turn_id != ctx.turn_id) {
        reset_turn(s);
        s->turn.active = true;
        s->turn.ctx    = ctx;
    }

    snprintf(s->turn.intent, sizeof(s->turn.intent), "%.*s",
             (int)(sizeof(s->turn.intent) - 1), intent);
    snprintf(s->turn.text, sizeof(s->turn.text), "%.*s", (int)(sizeof(s->turn.text) - 1),
             text);
    snprintf(s->turn.tool, sizeof(s->turn.tool), "%.*s", (int)(sizeof(s->turn.tool) - 1),
             (tool != NULL) ? tool : "");

    /* dialogue 把槽位编码为 slot.*；工具参数统一改成 arg.*。 */
    size_t off = 0;
    s->turn.args[0] = '\0';
    for (uint32_t i = 0; i < n; i++) {
        if (strncmp(kvs[i].key, "slot.", 5) != 0) {
            continue;
        }
        int w = snprintf(s->turn.args + off, sizeof(s->turn.args) - off, "arg.%s=%s\n",
                         kvs[i].key + 5, kvs[i].val);
        if (w < 0 || (size_t)w >= sizeof(s->turn.args) - off) {
            break;
        }
        off += (size_t)w;
    }

    s->turn.tool_round   = 0;
    s->turn.waiting_tool = false;

    if (s->turn.tool[0] != '\0') {
        start_tool_call(s);
    } else {
        generate_reply(s, NULL, NULL);
    }
}

static void on_tool_result(aura_orchestrator_stage_t *s, aura_flow_msg_t *msg)
{
    if (!s->turn.active || !s->turn.waiting_tool) {
        return;
    }

    aura_stage_kv_t kvs[AURA_STAGE_KV_MAX];
    uint32_t        n       = aura_stage_kv_parse(msg->payload, kvs, AURA_STAGE_KV_MAX);
    const char     *call_id = aura_stage_kv_get(kvs, n, "call_id");
    const char     *output  = aura_stage_kv_get(kvs, n, "output");
    if (call_id == NULL || output == NULL) {
        return;
    }

    char expected[16];
    snprintf(expected, sizeof(expected), "c%u", s->turn.pending_call_id);
    if (strcmp(call_id, expected) != 0) {
        return; /* 过期/其它轮次的工具结果 */
    }

    s->turn.waiting_tool = false;
    generate_reply(s, s->turn.tool, output);
}

static aura_err_t orchestrator_handler(aura_flow_msg_t *msg, void *user)
{
    aura_orchestrator_stage_t *s = (aura_orchestrator_stage_t *)user;
    if (s == NULL || msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    if (strcmp(msg->topic, AURA_TOPIC_DIALOGUE_INTENT) == 0) {
        on_dialogue_intent(s, msg);
    } else if (strcmp(msg->topic, AURA_TOPIC_TOOL_RESULT) == 0) {
        on_tool_result(s, msg);
    } else if (strcmp(msg->topic, AURA_TOPIC_TURN_START) == 0) {
        /* 同一 turn 的重复通知不打断在途工具/LLM；新 turn 才清状态。 */
        if (!s->turn.active || s->turn.ctx.session_id != msg->ctx.session_id ||
            s->turn.ctx.turn_id != msg->ctx.turn_id) {
            reset_turn(s);
        }
    } else if (strcmp(msg->topic, AURA_TOPIC_SESSION_CLOSED) == 0) {
        reset_turn(s);
    }
    return AURA_OK;
}

aura_orchestrator_stage_t *aura_orchestrator_stage_create(aura_flow_bus_t *bus,
                                                          aura_session_runtime_t *rt,
                                                          aura_agent_memory_t *memory,
                                                          const char *system_prompt)
{
    if (bus == NULL || rt == NULL) {
        return NULL;
    }
    aura_orchestrator_stage_t *s = (aura_orchestrator_stage_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->bus    = bus;
    s->rt     = rt;
    s->memory = memory;
    snprintf(s->system_prompt, sizeof(s->system_prompt), "%.*s",
             (int)(sizeof(s->system_prompt) - 1), (system_prompt != NULL) ? system_prompt : "");
    aura_llm_mock_backend(&s->backend);

    s->stage = aura_flow_stage_create(bus, "orchestrator", orchestrator_handler, s, 32,
                                      AURA_FLOW_DROP_OLDEST);
    if (s->stage == NULL) {
        free(s);
        return NULL;
    }
    const char *topics[] = {AURA_TOPIC_DIALOGUE_INTENT, AURA_TOPIC_TOOL_RESULT,
                            AURA_TOPIC_TURN_START, AURA_TOPIC_SESSION_CLOSED};
    for (size_t i = 0; i < sizeof(topics) / sizeof(topics[0]); i++) {
        if (aura_flow_stage_subscribe(s->stage, topics[i]) != AURA_OK) {
            aura_flow_stage_destroy(s->stage);
            free(s);
            return NULL;
        }
    }
    return s;
}

void aura_orchestrator_stage_destroy(aura_orchestrator_stage_t *stage)
{
    if (stage == NULL) {
        return;
    }
    aura_flow_stage_destroy(stage->stage);
    free(stage);
}

aura_err_t aura_orchestrator_stage_set_backend(aura_orchestrator_stage_t *stage,
                                               const aura_llm_backend_t *backend)
{
    if (stage == NULL || backend == NULL || backend->generate == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    stage->backend = *backend;
    return AURA_OK;
}

aura_err_t aura_orchestrator_stage_start(aura_orchestrator_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_start(stage->stage);
}

aura_err_t aura_orchestrator_stage_stop(aura_orchestrator_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_stop(stage->stage);
}
