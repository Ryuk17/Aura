#include "utils/error.h"

const char *aura_strerror(aura_err_t err)
{
    switch (err) {
    case AURA_OK:              return "ok";
    case AURA_ERR_FAIL:        return "generic failure";
    case AURA_ERR_INVALID_ARG: return "invalid argument";
    case AURA_ERR_NOMEM:       return "out of memory";
    case AURA_ERR_TIMEOUT:     return "timeout";
    case AURA_ERR_AGAIN:       return "try again";
    case AURA_ERR_FULL:        return "buffer full";
    case AURA_ERR_EMPTY:       return "buffer empty";
    case AURA_ERR_NOT_FOUND:   return "not found";
    case AURA_ERR_UNSUPPORTED: return "unsupported";
    case AURA_ERR_STATE:       return "invalid state";
    case AURA_ERR_IO:          return "io error";
    case AURA_ERR_MODEL:       return "model error";
    case AURA_ERR_ABORTED:     return "aborted";
    case AURA_ERR_BUSY:        return "busy";
    case AURA_ERR_EXIST:       return "already exists";
    case AURA_ERR_DSP:         return "dsp engine error";
    default:                   return "unknown error";
    }
}
