#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <timer.h>

#include "py/mpconfig.h"
#include "py/nlr.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "util/mpirq.h"

#include "esp_system.h"
#include "machtimer_alarm.h"


#define CMP_ALARM_WHEN(a, b) ((a)->when <= (b)->when)
#define ALARM_HEAP_MAX_ELEMENTS (16U)

#define CLK_FREQ (APB_CLK_FREQ / 2)

typedef struct {
    mp_obj_base_t base;
    uint64_t when;
    uint64_t interval;
    uint32_t heap_index;
    mp_obj_t handler;
    mp_obj_t handler_arg;
    bool periodic;
} mp_obj_alarm_t;

struct {
    uint32_t count;
    mp_obj_alarm_t **data;
} alarm_heap;

IRAM_ATTR void timer_alarm_isr(void *arg);
STATIC void load_next_alarm(void);
STATIC mp_obj_t alarm_delete(mp_obj_t self_in);
STATIC void alarm_set_callback_helper(mp_obj_t self_in, mp_obj_t handler, mp_obj_t handler_arg);

void alarm_preinit(void) {
    timer_isr_register(TIMER_GROUP_0, TIMER_0, timer_alarm_isr, NULL, ESP_INTR_FLAG_IRAM, NULL);
}

void init_alarm_heap(void) {
    alarm_heap.count = 0;
    MP_STATE_PORT(mp_alarm_heap) = gc_alloc(ALARM_HEAP_MAX_ELEMENTS * sizeof(mp_obj_alarm_t *), false);
    alarm_heap.data = MP_STATE_PORT(mp_alarm_heap);
    if (alarm_heap.data == NULL) {
        printf("ERROR: no enough memory for the alarms heap\n");
        for (;;);
    }
}

/*
 * Insert a new alarm into the heap
 * Note: It has already been checked that there is at least 1 free space on the heap
 * Note: The heap will remain ordered after the operation.
 * Note: If the new element is placed at the first place, which means its timestamp is the smallest, it is loaded immediatelly
 */
STATIC IRAM_ATTR void insert_alarm(mp_obj_alarm_t *alarm) {

    uint32_t index = 0, index2;

    /* The list is ordered at any given time. Find the place where the new element shall be inserted*/
    if(alarm_heap.count != 0) {
        for (; index < alarm_heap.count; ++index){
            if (alarm->when <= alarm_heap.data[index]->when) {
                break;
            }
        }

        /* Restructuring the list while keeping the order so the new element can be inserted at its correct place*/
        for(index2 = alarm_heap.count; index2 > index; index2--) {
            alarm_heap.data[index2] = alarm_heap.data[index2-1];
            alarm_heap.data[index2]->heap_index = index2;
        }
    }

    /* Insert the new element */
    alarm_heap.data[index] = alarm;
    alarm->heap_index = index;
    alarm_heap.count++;

    /* Start the newly added alarm if it is the first in the list*/
    if(index == 0 ) {
        load_next_alarm();
    }
}

/*
 * Remove the alarm from the heap at position alarm_heap_index
 * Note: The heap will remain ordered after the operation.
 */
STATIC IRAM_ATTR void remove_alarm(uint32_t alarm_heap_index) {
    uint32_t index;

    /* Invalidate the element */
    alarm_heap.data[alarm_heap_index]->heap_index = -1;
    alarm_heap.data[alarm_heap_index] = NULL;

    alarm_heap.count--;

    for (index = alarm_heap_index; index < alarm_heap.count; ++index){
        alarm_heap.data[index] = alarm_heap.data[index+1];
        alarm_heap.data[index]->heap_index = index;
        alarm_heap.data[index+1] = NULL;
    }
}

STATIC IRAM_ATTR void load_next_alarm(void) {
    TIMERG0.hw_timer[0].config.alarm_en = 0; // disable the alarm system
    // everything here done without calling any timers function, so it works inside the interrupts
    if (alarm_heap.count > 0) {
        uint64_t when;
        when = alarm_heap.data[0]->when;
        TIMERG0.hw_timer[0].alarm_high = (uint32_t) (when >> 32);
        TIMERG0.hw_timer[0].alarm_low = (uint32_t) when;
        TIMERG0.hw_timer[0].config.alarm_en = 1; // enable the alarm system
    }
}

STATIC IRAM_ATTR void set_alarm_when(mp_obj_alarm_t *alarm, uint64_t delta) {
    TIMERG0.hw_timer[0].update = 1;
    alarm->when = ((uint64_t) TIMERG0.hw_timer[0].cnt_high << 32)
        | (TIMERG0.hw_timer[0].cnt_low);
    alarm->when += delta;
}

STATIC void alarm_handler(void *arg) {
    // this function will be called by the interrupt thread
    mp_obj_alarm_t *alarm = arg;

    if (alarm->handler != mp_const_none) {
        mp_call_function_1(alarm->handler, alarm->handler_arg);
    }
}

IRAM_ATTR void timer_alarm_isr(void *arg) {
    TIMERG0.int_clr_timers.t0 = 1; // acknowledge the interrupt

    /* Need to check whether all timer have been removed from the list or not since the last time the HW alarm was set up*/
    if(alarm_heap.count > 0) {
        mp_obj_alarm_t *alarm = alarm_heap.data[0];

        remove_alarm(0);

        if (alarm->periodic) {
            set_alarm_when(alarm, alarm->interval);
            insert_alarm(alarm);
        }

        /* Start the next alarm */
        load_next_alarm();

        mp_irq_queue_interrupt(alarm_handler, alarm);
    }
}

STATIC mp_obj_t alarm_make_new(const mp_obj_type_t *type, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *all_args) {

    STATIC const mp_arg_t allowed_args[] = {
        { MP_QSTR_handler,      MP_ARG_OBJ  | MP_ARG_REQUIRED,   {.u_obj = mp_const_none} },
        { MP_QSTR_s,            MP_ARG_OBJ,                      {.u_obj = mp_const_none} },
        { MP_QSTR_ms,           MP_ARG_INT  | MP_ARG_KW_ONLY,    {.u_int = 0} },
        { MP_QSTR_us,           MP_ARG_INT  | MP_ARG_KW_ONLY,    {.u_int = 0} },
        { MP_QSTR_arg,          MP_ARG_OBJ  | MP_ARG_KW_ONLY,    {.u_obj = mp_const_none} },
        { MP_QSTR_periodic,     MP_ARG_BOOL | MP_ARG_KW_ONLY,    {.u_bool = false} },
    };

    // parse arguments
    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, all_args + n_args);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, all_args, &kw_args, MP_ARRAY_SIZE(args), allowed_args, args);

    float s = 0.0;
    if (args[1].u_obj != mp_const_none) {
        s = mp_obj_get_float(args[1].u_obj);
    }
    uint32_t ms = args[2].u_int;
    uint32_t us = args[3].u_int;

    if (((s != 0.0) + (ms != 0) + (us != 0)) != 1) {
        mp_raise_ValueError("please provide a single duration");
    }

    if (s < 0.0 || ms < 0 || us < 0) {
        mp_raise_ValueError("please provide a positive number");
    }

    uint64_t clocks = (uint64_t) (s * CLK_FREQ + 0.5) + ms * (CLK_FREQ / 1000) + us * (CLK_FREQ / 1000000);

    mp_obj_alarm_t *self = m_new_obj_with_finaliser(mp_obj_alarm_t);

    self->base.type = type;
    self->interval = clocks;
    self->periodic = args[5].u_bool;

    self->heap_index = -1;
    alarm_set_callback_helper(self, args[0].u_obj, args[4].u_obj);
    return self;
}

STATIC void alarm_set_callback_helper(mp_obj_t self_in, mp_obj_t handler, mp_obj_t handler_arg) {
    bool error = false;
    mp_obj_alarm_t *self = self_in;

    /* Do as much as possible outside the atomic section*/
    self->handler = handler;

    if (handler_arg == mp_const_none) {
        handler_arg = self_in;
    }
    self->handler_arg = handler_arg;

    mp_irq_handler_add(handler);

    /* Both remove_alarm and insert_alarm need to be guarded for the following reasons:
     * remove_alarm: If the ISR removes the 0th alarm then then the whole heap is restructured the indexes are changed,
     *               not the correct alarm would be deleted.
     * insert_alarm: If the ISR removes (re-adds) the 0th alarm then then the whole heap is restructured the indexes are changed,
     *               the current alarm may not be added to the correct position.
     */

    uint32_t state = MICROPY_BEGIN_ATOMIC_SECTION();
    /* Remove the alarm from the active alarms list if the handler is none */
    if(handler == mp_const_none){
        /* Check whether this alarm is currently active so can be removed */
        if(self->heap_index != -1) {
            remove_alarm(self->heap_index);
        }
    } else {
        /* Check whether this alarm is currently not active so it can be added */
        if (self->heap_index == -1) {
            if(alarm_heap.count == ALARM_HEAP_MAX_ELEMENTS) {
                error = true;
            } else {
                set_alarm_when(self, self->interval);
                insert_alarm(self);
            }
        }
    }

    MICROPY_END_ATOMIC_SECTION(state);

    if(error == true) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_MemoryError, "Alarm cannot be scheduled currently, maximum 16 alarms can be ran in parallel !"));
    }
}

STATIC mp_obj_t alarm_callback(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_handler,  MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = mp_const_none} },
        { MP_QSTR_arg,      MP_ARG_OBJ | MP_ARG_KW_ONLY,  {.u_obj = mp_const_none} },
    };

    mp_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    alarm_set_callback_helper(self, args[0].u_obj, args[1].u_obj);

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(alarm_callback_obj, 1, alarm_callback);

STATIC mp_obj_t alarm_delete(mp_obj_t self_in) {
    mp_obj_alarm_t *self = self_in;

    /* Atomic section is placed here due to the following reasons:
     * 1. If user calls "cancel" function it might be interrupted by the ISR when reordering the list because
     *    in this case the alarm is an active alarm which means it is placed on the alarm_heap and its heap_index != -1.
     * 2. When GC calls this function it is 100% percent sure that the heap_index is -1, because
     *    GC will only collect this object if it is not referred from the alarm_heap, which means it is not active thus
     *    its heap_index = 1.
     */
    if (self->heap_index != -1) {
        uint32_t state = MICROPY_BEGIN_ATOMIC_SECTION();
        remove_alarm(self->heap_index);
        MICROPY_END_ATOMIC_SECTION(state);
    }

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(alarm_delete_obj, alarm_delete);


STATIC const mp_map_elem_t mach_timer_alarm_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__),            MP_OBJ_NEW_QSTR(MP_QSTR_alarm) },
    { MP_OBJ_NEW_QSTR(MP_QSTR___del__),             (mp_obj_t) &alarm_delete_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_callback),            (mp_obj_t) &alarm_callback_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_cancel),              (mp_obj_t) &alarm_delete_obj },
};

STATIC MP_DEFINE_CONST_DICT(mach_timer_alarm_dict, mach_timer_alarm_dict_table);

const mp_obj_type_t mach_timer_alarm_type = {
    { &mp_type_type },
    .name = MP_QSTR_Alarm,
    .make_new = alarm_make_new,
    .locals_dict = (mp_obj_t)&mach_timer_alarm_dict,
};
