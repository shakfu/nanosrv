#pragma once
#include "platform.hpp"

namespace nanosrv {

enum class TimerMode : unsigned { Once = 0, Repeat = 1, RunNow = 2 };

struct Timer {
    uint64_t period_ms;
    uint64_t expire;
    unsigned flags;
    void (*fn)(void*);
    void* arg;
    struct Timer* next;
};

void timer_init(struct Timer** head, struct Timer* timer,
                   uint64_t milliseconds, unsigned flags, void (*fn)(void*),
                   void* arg);
void timer_free(struct Timer** head, struct Timer*);
void timer_poll(struct Timer** head, uint64_t new_ms);
bool timer_expired(uint64_t* expiration, uint64_t period, uint64_t now);

} // namespace nanosrv
