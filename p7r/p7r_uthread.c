#include    "./p7r_uthread.h"
#include    "./p7r_root_alloc.h"
#include    "./p7r_stack_hint.h"
#include    "./p7r_timing.h"


#define p7r_uthread_reenable(scheduler_, uthread_) \
    do { \
        __auto_type uthread__ = (uthread_); \
        if (uthread__->status != P7R_UTHREAD_RUNNING) { \
            p7r_uthread_detach(uthread__); \
            p7r_uthread_attach(uthread__, &((scheduler_)->runners.sched_queues[P7R_SCHED_QUEUE_RUNNING])); \
            p7r_uthread_change_state_clean(uthread__, P7R_UTHREAD_RUNNING); \
        } \
    } while (0)


// globals

static struct p7r_scheduler *schedulers;


// timer

static
struct p7r_timer_core *p7r_timer_core_init(struct p7r_timer_core *timer, uint64_t timestamp, struct p7r_uthread *uthread) {
    timer->triggered = 0;
    timer->uthread = uthread;
    timer->timestamp = timestamp;
    timer->maplink.key_ref = &(timer->timestamp);
    return timer;
}

static
struct p7r_timer_core *p7r_timer_core_init_diff(struct p7r_timer_core *timer, uint64_t diff, struct p7r_uthread *uthread) {
    return p7r_timer_core_init(timer, get_timestamp_ms_by_diff(diff), uthread);
}

static
void p7r_timer_core_attach(struct p7r_timer_queue *queue, struct p7r_timer_core *timer) {
    scraft_rbt_insert(&(queue->map), &(timer->maplink));
}

static
void p7r_timer_core_detach(struct p7r_timer_core *timer) {
    scraft_rbt_detach(&(timer->maplink));
}

static
int p7r_timer_core_compare(const void *lhs_, const void *rhs_) {
    uint64_t lhs = *((const uint64_t *) lhs_), rhs = *((const uint64_t *) rhs_);
    return (lhs == rhs) ? 0 : ( (lhs < rhs) ? -1 : 1 );
}

static
void p7r_timer_queue_init(struct p7r_timer_queue *queue) {
    scraft_rbt_init(&(queue->map), p7r_timer_core_compare);
}

static
struct p7r_timer_core *p7r_timer_peek_earliest(struct p7r_timer_queue *queue) {
    struct scraft_rbtree_node *node = scraft_rbtree_min(&(queue->map));
    return (node != queue->map.sentinel) ? container_of(node, struct p7r_timer_core, maplink) : NULL;
}


// uthread

static inline
struct p7r_uthread_request *p7r_uthread_request_new(void (*entrance)(void *), void *argument) {
    struct p7r_uthread_request *request = NULL;
    {
        __auto_type allocator = p7r_root_alloc_get_proxy();
        request = scraft_allocate(allocator, sizeof(struct p7r_uthread_request));
        (request) && ((request->user_entrance = entrance), (request->user_argument = argument));
    }
    return request;
}

static inline
void p7r_uthread_request_delete(struct p7r_uthread_request *request) {
    __auto_type allocator = p7r_root_alloc_get_proxy();
    scraft_deallocate(allocator, request);
}

static inline
void p7r_uthread_change_state_clean(struct p7r_uthread *uthread, uint64_t status) {
    __atomic_store_n(&(uthread->status), status, __ATOMIC_RELEASE);
}

static inline
void p7r_uthread_detach(struct p7r_uthread *uthread) {
    list_del(&(uthread->linkable));
}

static inline
void p7r_uthread_attach(struct p7r_uthread *uthread, list_ctl_t *target) {
    list_add_tail(&(uthread->linkable), target);
}

static
void p7r_uthread_lifespan(void *uthread_) {
    struct p7r_uthread *self = uthread_;

    struct p7r_uthread_request *reincarnation = NULL;
    do {
        p7r_uthread_change_state_clean(self, P7R_UTHREAD_RUNNING);
        self->entrance.user_entrance(self->entrance.user_argument);
        p7r_uthread_change_state_clean(self, P7R_UTHREAD_LIMBO);
        // TODO reincarnation, with resched
    } while (reincarnation);

    p7r_uthread_detach(self);
    p7r_uthread_change_state_clean(self, P7R_UTHREAD_DYING);

    // Actually we never return, but that's one of things we would not tell the compiler
    p7r_context_switch(schedulers[self->scheduler_index].runners.carrier_context, &(self->context));
}

static
struct p7r_uthread *p7r_uthread_init(
        struct p7r_uthread *uthread, 
        uint32_t scheduler_index, 
        void (*user_entrance)(void *), 
        void *user_argument, 
        struct p7r_stack_metamark *stack_metamark) {
    uthread->scheduler_index = scheduler_index;
    uthread->stack_metamark = stack_metamark;
    (uthread->entrance.user_entrance = user_entrance), (uthread->entrance.user_argument = user_argument);
    (uthread->entrance.real_entrance = p7r_uthread_lifespan), (uthread->entrance.real_argument = uthread);
    p7r_context_init(
            &(uthread->context), 
            stack_metamark->raw_content_addr, 
            stack_metamark->n_bytes_page * stack_metamark->provider->parent->properties.n_pages_stack_total);
    p7r_context_prepare(&(uthread->context), uthread->entrance.real_entrance, uthread->entrance.real_argument);
    return uthread;
}

static inline
struct p7r_uthread *p7r_uthread_ruin(struct p7r_uthread *uthread) {
    // XXX empty implementation
    return uthread;
}

static
struct p7r_uthread *p7r_uthread_new(
        uint32_t scheduler_index, 
        void (*user_entrance)(void *), 
        void *user_argument, 
        struct p7r_stack_allocator *allocator, 
        uint8_t stack_alloc_policy) {
    struct p7r_stack_metamark *stack_meta = p7r_stack_allocate_hintless(allocator, stack_alloc_policy);
    if (unlikely(stack_meta == NULL))
        return NULL;
    struct p7r_uthread *uthread = (struct p7r_uthread *) stack_meta->user_metadata;
    return p7r_uthread_init(uthread, scheduler_index, user_entrance, user_argument, stack_meta);
}

static inline
void p7r_uthread_delete(struct p7r_uthread *uthread) {
    struct p7r_stack_metamark *stack_meta = p7r_uthread_ruin(uthread)->stack_metamark;
    p7r_stack_free(stack_meta);
}

static inline
void p7r_uthread_switch(struct p7r_uthread *to, struct p7r_uthread *from) {
    p7r_context_switch(&(to->context), &(from->context));
}

static
void u2cc_handler_uthread_request(struct p7r_scheduler *scheduler, struct p7r_internal_message *message) {
    struct p7r_uthread_request *request = (struct p7r_uthread_request *) message->content_buffer;
    list_add_tail(&(request->linkable), &(scheduler->runners.request_queue));
}

static
void (*p7r_internal_handlers[])(struct p7r_scheduler *, struct p7r_internal_message *) = {
    [1] = u2cc_handler_uthread_request,
};

static
int sched_bus_refresh(struct p7r_scheduler *scheduler) {
    // Phase 1 - adjust timeout baseline
    int timeout = 0;
    if (scheduler->bus.consumed) {
        uint64_t current_time_before = get_timestamp_ms_current();
        struct p7r_timer_core *timer_earliest = p7r_timer_peek_earliest(&(scheduler->bus.timers));
        timeout = timer_earliest ? (timer_earliest->timestamp - current_time_before) : -1;
        (timeout < 1) && (timeout = 0);
    }
    (!list_is_empty(&(scheduler->runners.sched_queues[P7R_SCHED_QUEUE_RUNNING]))) && (timeout = 0);

    int n_active_fds = epoll_wait(scheduler->bus.fd_epoll, scheduler->bus.epoll_events, scheduler->bus.n_epoll_events, timeout);
    if (n_active_fds < 0)
        return -1;
    scheduler->bus.consumed = 1;        // XXX consumer flag must be reset here

    // Phase 2 - timeout handling
    uint64_t current_time = get_timestamp_ms_current();
    struct p7r_timer_core *timer_iterator = NULL;
    while (
            ((timer_iterator = p7r_timer_peek_earliest(&(scheduler->bus.timers))) != NULL) &&
            (timer_iterator->timestamp <= current_time)
          ) {
        scraft_rbt_detach(&(timer_iterator->maplink));
        timer_iterator->triggered = 1;
        p7r_uthread_reenable(scheduler, timer_iterator->uthread);
    }

    // Phase 3 - respond delegation events: i/o notification & internal wakeup
    for (int event_index = 0; event_index < n_active_fds; event_index++) {
        struct p7r_delegation *delegation = scheduler->bus.epoll_events[event_index].data.ptr;
        if (delegation == &(scheduler->bus.notification)) {
            uint64_t notification_counter;
            read(scheduler->bus.fd_notification, &notification_counter, sizeof(uint64_t));
        } else {
            p7r_uthread_reenable(scheduler, delegation->uthread);
        }
    }

    // Phase 4 - iuc/u2cc handling
    for (uint32_t carrier_index = 0; carrier_index < scheduler->n_carriers; carrier_index++) {
        if (carrier_index != scheduler->index) {
            list_ctl_t *target_queue = cp_buffer_consume(&(scheduler->bus.message_boxes[carrier_index]));
            scheduler->bus.consumed &= scheduler->bus.message_boxes[carrier_index].consuming;
            list_ctl_t *p, *t;
            list_foreach_remove(p, target_queue, t) {
                list_del(t);
                struct p7r_internal_message *message = container_of(t, struct p7r_internal_message, linkable);
                p7r_internal_handlers[P7R_MESSAGE_REAL_TYPE(message->type)](scheduler, message);    // XXX highly dangerous
            }
        }
    }

    // Phase 5 - iuc discarding
    // TODO implementation

    // Phase 6 - R.I.P. those who chose not to reincarnate
    {
        list_ctl_t *p, *t;
        list_foreach_remove(p, &(scheduler->runners.sched_queues[P7R_SCHED_QUEUE_DYING]), t) {
            list_del(t);
            struct p7r_uthread *uthread_dying = container_of(t, struct p7r_uthread, linkable);
            p7r_uthread_delete(uthread_dying);
        }
    }

    return 0;
}

static
struct p7r_uthread_request sched_cherry_pick(struct p7r_scheduler *scheduler) {
    struct p7r_uthread_request request = { .user_entrance = NULL, .user_argument = NULL };
    if (!list_is_empty(&(scheduler->runners.request_queue))) {
        list_ctl_t *target_link = scheduler->runners.request_queue.next;
        list_del(target_link);
        struct p7r_uthread_request *target_request = container_of(target_link, struct p7r_uthread_request, linkable);
        request = *target_request;
        p7r_uthread_request_delete(target_request);
    }
    return request;
}

static
struct p7r_uthread *sched_uthread_from_request(
        struct p7r_scheduler *scheduler, 
        struct p7r_uthread_request request, 
        uint8_t stack_alloc_policy) {
    return 
        p7r_uthread_new(
                scheduler->index, 
                request.user_entrance, 
                request.user_argument, 
                &(scheduler->runners.stack_allocator),
                stack_alloc_policy);
}

static
struct p7r_uthread *sched_resched_target(struct p7r_scheduler *scheduler) {
    if (list_is_empty(&(scheduler->runners.sched_queues[P7R_SCHED_QUEUE_RUNNING])))
        return NULL;
    list_ctl_t *target_reference = scheduler->runners.sched_queues[P7R_SCHED_QUEUE_RUNNING].next;
    list_del(target_reference);
    return container_of(target_reference, struct p7r_uthread, linkable);
}

static inline
void sched_idle(struct p7r_uthread *uthread, struct p7r_scheduler *scheduler) {
    p7r_context_switch(scheduler->runners.carrier_context, &(uthread->context));
}

static
struct p7r_scheduler *p7r_scheduler_init(
        struct p7r_scheduler *scheduler, 
        uint32_t index, 
        uint32_t n_carriers, 
        struct p7r_context *carrier_context,
        struct p7r_stack_allocator_config config,
        int event_buffer_capacity) {
    __atomic_store_n(&(scheduler->status), P7R_SCHEDULER_BORN, __ATOMIC_RELEASE);

    (scheduler->index = index), (scheduler->n_carriers = n_carriers);
    scheduler->runners.carrier_context = carrier_context;
    p7r_stack_allocator_init(&(scheduler->runners.stack_allocator), config);

    for (uint32_t queue_index = 0; queue_index < sizeof(scheduler->runners.sched_queues) / sizeof(list_ctl_t); queue_index++)
        init_list_head(&(scheduler->runners.sched_queues[queue_index]));
    init_list_head(&(scheduler->runners.request_queue));
    scheduler->runners.running = NULL;

    scheduler->bus.fd_epoll = epoll_create1(EPOLL_CLOEXEC);
    scheduler->bus.fd_notification = eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK);
    scheduler->bus.consumed = 1;
    scheduler->bus.message_boxes = malloc(sizeof(struct p7r_cpbuffer) * n_carriers);
    for (uint32_t index = 0; index < n_carriers; index++)
        cp_buffer_init(&(scheduler->bus.message_boxes[index]));
    p7r_timer_queue_init(&(scheduler->bus.timers));
    scheduler->bus.n_epoll_events = event_buffer_capacity;      // XXX We do not check anything - keep your sanity
    scheduler->bus.epoll_events = malloc(sizeof(struct epoll_event) * event_buffer_capacity);
    (scheduler->bus.notification.fd = scheduler->bus.fd_notification), (scheduler->bus.notification.uthread = NULL);

    __atomic_store_n(&(scheduler->status), P7R_SCHEDULER_ALIVE, __ATOMIC_RELEASE);

    return scheduler;
}

static
struct p7r_scheduler *p7r_scheduler_ruin(struct p7r_scheduler *scheduler) {
    __atomic_store_n(&(scheduler->status), P7R_SCHEDULER_DYING, __ATOMIC_RELEASE);

    // All uthreads will be destroyed with the corresponding stack allocator
    p7r_stack_allocator_ruin(&(scheduler->runners.stack_allocator));

    free(scheduler->bus.epoll_events);
    close(scheduler->bus.fd_epoll);
    close(scheduler->bus.fd_notification);
    {
        list_ctl_t *p, *t;
        for (uint32_t index = 0; index < 1; index++) {
            list_foreach_remove(p, &(scheduler->bus.message_boxes->buffers[index]), t) {
                list_del(t);
                struct p7r_internal_message *message = container_of(t, struct p7r_internal_message, linkable);
                if (message->content_destructor)
                    message->content_destructor(message);
            }
        }
    }
    free(scheduler->bus.message_boxes);

    {
        list_ctl_t *p, *t;
        list_foreach_remove(p, &(scheduler->runners.request_queue), t) {
            list_del(t);
            p7r_uthread_request_delete(container_of(t, struct p7r_uthread_request, linkable));
        }
    }

    return scheduler;
}