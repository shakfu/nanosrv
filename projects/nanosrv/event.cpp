#include "nanosrv/nanosrv.hpp"

namespace nanosrv {

// ---- module: event ----

void call(struct Connection* c, int ev, void* ev_data)
{
#if MG_ENABLE_PROFILE
    const char* names[] = { "EV_ERROR",     "EV_OPEN",     "EV_POLL",
                            "EV_RESOLVE",   "EV_CONNECT",  "EV_ACCEPT",
                            "EV_TLS_HS",    "EV_READ",     "EV_WRITE",
                            "EV_CLOSE",     "EV_HTTP_MSG", "EV_HTTP_CHUNK",
                            "EV_WS_OPEN",   "EV_WS_MSG",   "EV_WS_CTL",
                            "EV_MQTT_CMD",  "EV_MQTT_MSG", "EV_MQTT_OPEN",
                            "EV_SNTP_TIME", "EV_USER" };
    if (ev != MG_EV_POLL && ev < (int)(sizeof(names) / sizeof(names[0]))) {
        MG_PROF_ADD(c, names[ev]);
    }
#endif
    // Fire protocol handler first, user handler second. See #2559
    if (c->pfn != NULL)
        c->pfn(c, ev, ev_data);
    if (c->fn != NULL)
        c->fn(c, ev, ev_data);
}

void error(struct Connection* c, const char* fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_(buf, sizeof(buf), fmt, &ap);
    va_end(ap);
    MG_ERROR(("%lu %ld %s", c->id, c->fd, buf));
    c->is_closing = 1;            // Set is_closing before sending MG_EV_CALL
    call(c, MG_EV_ERROR, buf); // Let user handler override it
}

} // namespace nanosrv
