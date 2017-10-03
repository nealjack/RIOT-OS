/*
 * Copyright (C) 2017 Fundacion Inria Chile
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @{
 * @ingroup     net
 * @file
 * @brief       Implementation of OpenThread alarm platform abstraction
 *
 * @author      Jose Ignacio Alamos <jialamos@uc.cl>
 * @}
 */

#include <stdint.h>

#include "msg.h"
#include "openthread/platform/alarm-milli.h"
#include "openthread/tasklet.h"
#include "ot.h"
#include "thread.h"
#include "xtimer.h"
#include "timex.h"

#define ENABLE_DEBUG (0)
#include "debug.h"

#define OPENTHREAD_QUEUE_LEN      (5)
static msg_t _queue[OPENTHREAD_QUEUE_LEN];

static kernel_pid_t _pid;

static xtimer_t ot_timer;
static msg_t ot_alarm_msg;

/**
 * Set the alarm to fire at @p aDt milliseconds after @p aT0.
 *
 * @param[in] aInstance  The OpenThread instance structure.
 * @param[in] aT0        The reference time.
 * @param[in] aDt        The time delay in milliseconds from @p aT0.
 */
void otPlatAlarmMilliStartAt(otInstance *aInstance, uint32_t aT0, uint32_t aDt)
{
    DEBUG("openthread: otPlatAlarmMilliStartAt: aT0: %" PRIu32 ", aDT: %" PRIu32 "\n", aT0, aDt);
    ot_alarm_msg.type = OPENTHREAD_XTIMER_MSG_TYPE_EVENT;

    if (aDt == 0) {
        msg_send(&ot_alarm_msg, _pid);
    }
    else {
        int dt = aDt * US_PER_MS;
        xtimer_set_msg(&ot_timer, dt, &ot_alarm_msg, _pid);
    }
}

/* OpenThread will call this to stop alarms */
void otPlatAlarmMilliStop(otInstance *aInstance)
{
    DEBUG("openthread: otPlatAlarmMilliStop\n");
    xtimer_remove(&ot_timer);
}

/* OpenThread will call this for getting running time in millisecs */
uint32_t otPlatAlarmMilliGetNow(void)
{
    uint32_t now = xtimer_now_usec() / US_PER_MS;
    DEBUG("openthread: otPlatAlarmMilliGetNow: %" PRIu32 "\n", now);
    return now;
}

static void *_openthread_timer_thread(void *arg) {
    _pid = thread_getpid();

    printf("timer thread start with %u\n", _pid);
    msg_init_queue(_queue, OPENTHREAD_QUEUE_LEN);
    msg_t msg;

    while (1) {
        otTaskletsProcess(openthread_get_instance());
        if (otTaskletsArePending(openthread_get_instance()) == false) {
            msg_receive(&msg);
            switch (msg.type) {
                case OPENTHREAD_XTIMER_MSG_TYPE_EVENT:
                    /* Tell OpenThread a time event was received */
                    otPlatAlarmMilliFired(openthread_get_instance());
                    break;
            }
        }
    }

    return NULL;
}

/* starts OpenThread timer thread */
int openthread_timer_init(char *stack, int stacksize, char priority, const char *name) {

    _pid = thread_create(stack, stacksize, priority, THREAD_CREATE_STACKTEST,
                         _openthread_timer_thread, NULL, name);

    if (_pid <= 0) {
        return -EINVAL;
    }

    return _pid;
}
