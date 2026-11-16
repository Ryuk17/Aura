/* Aura — LLM 后端抽象（mock 实现） */
#include "agent/llm/llm.h"

#include <stdio.h>
#include <string.h>

static aura_err_t mock_generate(void *user, const aura_llm_request_t *req,
                                aura_llm_response_t *resp)
{
    (void)user;
    if (req == NULL || resp == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    memset(resp, 0, sizeof(*resp));

    const char *intent      = (req->intent != NULL) ? req->intent : "";
    const char *user_text   = (req->user_text != NULL) ? req->user_text : "";
    const char *tool_result = req->tool_result;

    if (tool_result != NULL) {
        if (strcmp(intent, "weather") == 0) {
            snprintf(resp->text, sizeof(resp->text), "当前天气：%s", tool_result);
        } else if (strcmp(intent, "time") == 0) {
            snprintf(resp->text, sizeof(resp->text), "现在是 %s", tool_result);
        } else if (strcmp(intent, "volume") == 0) {
            snprintf(resp->text, sizeof(resp->text), "好的，%s", tool_result);
        } else {
            snprintf(resp->text, sizeof(resp->text), "工具返回：%s", tool_result);
        }
        return AURA_OK;
    }

    snprintf(resp->text, sizeof(resp->text), "收到：%s", user_text);
    return AURA_OK;
}

void aura_llm_mock_backend(aura_llm_backend_t *out)
{
    if (out == NULL) {
        return;
    }
    out->name     = "mock";
    out->generate = mock_generate;
    out->user     = NULL;
}
