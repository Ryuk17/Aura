#include "core/node.h"

#include <stdio.h>
#include <string.h>

#include "core/event_bus/event_bus.h"
#include "core/pipeline/pipeline.h"
#include "utils/logger/logger.h"

#define TAG "node"

/* 下游入队的有界等待（见 aura_node_emit_audio 注释）。 */
#define AURA_NODE_EMIT_WAIT_MS 10u

void aura_node_init(aura_node_t *node, const char *name, const aura_node_ops_t *ops,
                    const aura_node_caps_t *caps)
{
    if (node == NULL) {
        return;
    }
    memset(node, 0, sizeof(*node));
    node->name = (name != NULL) ? name : "node";
    node->ops  = ops;
    if (caps != NULL) {
        node->caps = *caps;
    }
}

aura_err_t aura_node_emit_audio(aura_node_t *self, const aura_audio_frame_t *frame)
{
    if (self == NULL || frame == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_pipeline_t *p = self->pipeline;
    if (p == NULL) {
        return AURA_ERR_STATE;
    }
    if (self->next_audio == NULL) {
        /* 本节点是该音频流的终点：丢弃产出，不算错误。 */
        return AURA_OK;
    }
    if (frame->data == NULL || frame->data_bytes == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    if (frame->data_bytes > AURA_AUDIO_FRAME_MAX_BYTES) {
        aura_node_report_error(self, AURA_ERR_INVALID_ARG, "emit audio exceeds max frame bytes");
        return AURA_ERR_INVALID_ARG;
    }

    aura_audio_frame_t *copy = aura_pipeline_frame_alloc(p);
    if (copy == NULL) {
        self->stats.dropped++;
        return AURA_ERR_NOMEM; /* 帧池耗尽：宁可丢帧也不阻塞音频链路 */
    }

    /* 元数据逐字段拷贝（frame->data 指向调用方缓冲，不能带过去）。 */
    copy->sample_rate = frame->sample_rate;
    copy->channels    = frame->channels;
    copy->frame_count = frame->frame_count;
    copy->fmt         = frame->fmt;
    copy->channel_id  = frame->channel_id;
    copy->data_bytes  = frame->data_bytes;

    /* pts 由 pipeline 统一维护：若本节点正在处理一帧输入，则强制沿用输入 pts，
     * 节点自行编造的时间戳一律被纠正（todo.md 4.2）。 */
    if (self->last_in_pts_valid) {
        if (frame->pts_us != self->last_in_pts_us) {
            AURA_LOGD(TAG, "%s: pts corrected %llu -> %llu", self->name,
                      (unsigned long long)frame->pts_us,
                      (unsigned long long)self->last_in_pts_us);
        }
        copy->pts_us = self->last_in_pts_us;
    } else {
        copy->pts_us = frame->pts_us;
    }

    memcpy(copy->data, frame->data, frame->data_bytes);

    /* 有界背压：下游队列满时最多等 AURA_NODE_EMIT_WAIT_MS（10ms，一帧的时长）。
     * 既不让"process 内禁止长阻塞"被破坏（不是无界等待），也避免正常调度抖动
     * 造成丢帧 —— 只有下游真的卡死才会丢帧。 */
    if (aura_osal_queue_push(self->next_audio->in_audio, copy, AURA_NODE_EMIT_WAIT_MS) !=
        AURA_OK) {
        aura_pipeline_frame_free(p, copy);
        self->stats.dropped++;
        return AURA_ERR_FULL;
    }
    return AURA_OK;
}

aura_err_t aura_node_emit_text(aura_node_t *self, const aura_text_chunk_t *chunk)
{
    if (self == NULL || chunk == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_pipeline_t *p = self->pipeline;
    if (p == NULL) {
        return AURA_ERR_STATE;
    }
    if (self->next_text == NULL) {
        return AURA_OK;
    }
    aura_text_chunk_t copy = *chunk;
    if (copy.len > AURA_TEXT_CHUNK_MAX_BYTES) {
        return AURA_ERR_INVALID_ARG;
    }
    if (aura_osal_queue_push(self->next_text->in_text, &copy, AURA_NO_WAIT) != AURA_OK) {
        self->stats.dropped++;
        return AURA_ERR_FULL;
    }
    return AURA_OK;
}

aura_err_t aura_node_emit_event(aura_node_t *self, const aura_event_t *event)
{
    if (self == NULL || event == NULL || self->bus == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_event_bus_publish(self->bus, event);
}

aura_err_t aura_node_post_event(aura_node_t *self, int type, int code)
{
    if (self == NULL || self->bus == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_event_bus_post(self->bus, (aura_event_type_t)type, code, self->name);
}

aura_err_t aura_node_report_error(aura_node_t *self, aura_err_t err, const char *what)
{
    if (self == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    /* 契约：错误上抛，绝不 exit/阻塞（todo.md 4.2）。 */
    AURA_LOGE(TAG, "%s: %s (%d)", self->name, (what != NULL) ? what : "error", (int)err);
    if (self->bus == NULL) {
        return err;
    }
    aura_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = AURA_EVENT_ERROR;
    ev.code = (int32_t)err;
    snprintf(ev.payload, sizeof(ev.payload), "%s: %s", self->name,
             (what != NULL) ? what : aura_strerror(err));
    aura_event_bus_publish(self->bus, &ev);
    return err;
}

uint32_t aura_node_pending(const aura_node_t *self)
{
    if (self == NULL) {
        return 0;
    }
    uint32_t n = 0;
    if (self->in_audio != NULL) {
        n += aura_osal_queue_count(self->in_audio);
    }
    if (self->in_text != NULL) {
        n += aura_osal_queue_count(self->in_text);
    }
    return n;
}

bool aura_node_is_caller_thread(const aura_node_t *self)
{
    if (self == NULL || self->task == NULL) {
        return false;
    }
    return aura_osal_task_id(self->task) == aura_osal_task_self();
}
