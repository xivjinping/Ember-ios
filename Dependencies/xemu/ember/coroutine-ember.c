/*
 * Ember: coroutine backend for iOS.
 *
 * QEMU's block layer runs on coroutines. None of its stock backends works
 * on iOS: there is no ucontext for arm64, and sigaltstack() fails silently
 * there, so the "sigaltstack" backend would run every coroutine on the
 * caller's stack. This backend is the sigaltstack one with the signal trick
 * replaced: a new coroutine gets onto its own stack through three lines of
 * assembly, and from then on switching is sigsetjmp/siglongjmp exactly as
 * before.
 *
 * Copied to util/ by Dependencies/xemu/patch_xemu.py and used in place of
 * coroutine-sigaltstack.c on iOS. Derived from util/coroutine-sigaltstack.c
 * (LGPL-2.1-or-later).
 */

/* XXX Is there a nicer way to disable glibc's stack check for longjmp? */
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0

#include "qemu/osdep.h"
#include <pthread.h>
#include "qemu/coroutine_int.h"

#ifndef __aarch64__
#error "coroutine-ember.c is written for arm64"
#endif

typedef struct {
    Coroutine base;
    void *stack;
    size_t stack_size;
    sigjmp_buf env;
} CoroutineEmber;

typedef struct {
    /** Currently executing coroutine */
    Coroutine *current;

    /** The default coroutine */
    CoroutineEmber leader;
} CoroutineThreadState;

static pthread_key_t thread_state_key;

static CoroutineThreadState *coroutine_get_thread_state(void)
{
    CoroutineThreadState *s = pthread_getspecific(thread_state_key);

    if (!s) {
        s = g_malloc0(sizeof(*s));
        s->current = &s->leader.base;
        pthread_setspecific(thread_state_key, s);
    }
    return s;
}

static void qemu_coroutine_thread_cleanup(void *opaque)
{
    CoroutineThreadState *s = opaque;

    g_free(s);
}

static void __attribute__((constructor)) coroutine_init(void)
{
    int ret;

    ret = pthread_key_create(&thread_state_key, qemu_coroutine_thread_cleanup);
    if (ret != 0) {
        fprintf(stderr, "unable to create leader key: %s\n", strerror(errno));
        abort();
    }
}

/*
 * void ember_call_on_stack(void *stack_top, void (*fn)(void *), void *arg)
 *
 * Makes stack_top the stack pointer and calls fn(arg). fn must not return:
 * there is nothing to return to (frame pointer and link register are zero,
 * which also ends a debugger's backtrace cleanly).
 */
void ember_call_on_stack(void *stack_top, void (*fn)(void *), void *arg)
    __attribute__((noreturn));
__asm__(
    ".text\n"
    ".globl _ember_call_on_stack\n"
    ".p2align 2\n"
    "_ember_call_on_stack:\n"
    "    mov sp, x0\n"
    "    mov x0, x2\n"
    "    mov x29, #0\n"
    "    mov x30, #0\n"
    "    br x1\n");

/* First code to run on a new coroutine's stack. */
static void coroutine_bootstrap(void *opaque)
{
    CoroutineEmber *self = opaque;
    Coroutine *co = &self->base;

    /* Record this context, then go back to qemu_coroutine_new(). */
    if (!sigsetjmp(self->env, 0)) {
        siglongjmp(*(sigjmp_buf *)co->entry_arg, 1);
    }

    while (true) {
        co->entry(co->entry_arg);
        qemu_coroutine_switch(co, co->caller, COROUTINE_TERMINATE);
    }
}

Coroutine *qemu_coroutine_new(void)
{
    CoroutineEmber *co;
    sigjmp_buf old_env;

    co = g_malloc0(sizeof(*co));
    co->stack_size = COROUTINE_STACK_SIZE;
    co->stack = qemu_alloc_stack(&co->stack_size);
    co->base.entry_arg = &old_env; /* stash away our jmp_buf */

    if (!sigsetjmp(old_env, 0)) {
        /* The stack grows down; its top is page aligned, so 16-byte aligned. */
        ember_call_on_stack((char *)co->stack + co->stack_size,
                            coroutine_bootstrap, co);
    }

    /* coroutine_bootstrap() has saved its context and jumped back here. */
    return &co->base;
}

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineEmber *co = DO_UPCAST(CoroutineEmber, base, co_);

    qemu_free_stack(co->stack, co->stack_size);
    g_free(co);
}

CoroutineAction qemu_coroutine_switch(Coroutine *from_, Coroutine *to_,
                                      CoroutineAction action)
{
    CoroutineEmber *from = DO_UPCAST(CoroutineEmber, base, from_);
    CoroutineEmber *to = DO_UPCAST(CoroutineEmber, base, to_);
    CoroutineThreadState *s = coroutine_get_thread_state();
    int ret;

    s->current = to_;

    ret = sigsetjmp(from->env, 0);
    if (ret == 0) {
        siglongjmp(to->env, action);
    }
    return ret;
}

Coroutine *qemu_coroutine_self(void)
{
    CoroutineThreadState *s = coroutine_get_thread_state();

    return s->current;
}

bool qemu_in_coroutine(void)
{
    CoroutineThreadState *s = pthread_getspecific(thread_state_key);

    return s && s->current->caller;
}
