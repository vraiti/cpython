/* SIG_DFL interception.
 *
 * A terminate-class signal whose disposition is SIG_DFL ends the process
 * inside the kernel: no user-space code runs, so nothing could flush the
 * trace. While tracing is active D3G substitutes its own C handler for
 * SIG_DFL on such signals, without touching the signal module's Python-level
 * bookkeeping (signal.getsignal() still reports SIG_DFL). On delivery the
 * handler only records the signal and pokes the eval breaker; the main thread
 * then flushes the trace, restores SIG_DFL and re-raises, so the process dies
 * by the very signal it received (status 128+n, WIFSIGNALED) exactly as it
 * would have without D3G.
 *
 * The substitution is applied at install() to every candidate signal found
 * at SIG_DFL, and again whenever the application sets SIG_DFL through
 * PyOS_setsig() (signal.signal and every path built on it). Installing a
 * Python handler or SIG_IGN overwrites it, as it should. */

#include "Python.h"
#include "pycore_ceval.h"        /* _PyEval_SignalReceived */
#include "tracer_hooks.h"
#include <signal.h>
#include <errno.h>

static const int candidates[] = {
    SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGUSR1, SIGUSR2, SIGALRM, SIGPIPE,
#ifdef SIGXCPU
    SIGXCPU,
#endif
};
#define NCAND ((int)(sizeof(candidates) / sizeof(candidates[0])))

static int active;                               /* install() .. flush */
static char substituted[NSIG];                   /* kernel has our handler */
static volatile sig_atomic_t pending[NSIG];      /* delivered, not yet acted on */
static volatile sig_atomic_t any_pending;

static int substitutable(int sig) {
    for (int i = 0; i < NCAND; i++)
        if (candidates[i] == sig) return 1;
    return 0;
}

/* Perform the genuine default action. Async-signal-safe. Does not return
 * for terminate-class signals. */
static void die_by(int sig) {
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    raise(sig);
}

static void dfl_handler(int sig) {
    int save_errno = errno;
    if (pending[sig]) {
        /* Second delivery while the first is still waiting for the main
         * thread: do not keep the sender waiting, die now as SIG_DFL would. */
        die_by(sig);
        return;
    }
    pending[sig] = 1;
    any_pending = 1;
    _PyEval_SignalReceived();
    errno = save_errno;
}

/* Called from PyOS_setsig before the sigaction() call. */
PyOS_sighandler_t d3g_setsig_substitute(int sig, PyOS_sighandler_t handler) {
    if (sig <= 0 || sig >= NSIG) return handler;
    if (active && handler == SIG_DFL && substitutable(sig)) {
        substituted[sig] = 1;
        return dfl_handler;
    }
    substituted[sig] = 0;
    return handler;
}

/* Called from PyOS_setsig on the handler it is about to return. */
PyOS_sighandler_t d3g_setsig_report(PyOS_sighandler_t old) {
    return old == dfl_handler ? SIG_DFL : old;
}

/* Main thread, GIL held: act on signals the substitute handler recorded. */
void d3g_check_pending_signals(void) {
    if (!any_pending) return;
    any_pending = 0;
    for (int i = 0; i < NCAND; i++) {
        int sig = candidates[i];
        if (!pending[sig]) continue;
        d3g_flush_trace();
        die_by(sig);
        pending[sig] = 0;   /* only reached if the signal did not terminate us */
    }
}

void d3g_signals_install(void) {
    active = 1;
    for (int i = 0; i < NCAND; i++) {
        int sig = candidates[i];
        struct sigaction cur;
        if (sigaction(sig, NULL, &cur) == -1) continue;
        if (cur.sa_handler != SIG_DFL) continue;   /* SIG_IGN or a handler: leave it */
        struct sigaction sa;
        sa.sa_handler = dfl_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_ONSTACK;
        if (sigaction(sig, &sa, NULL) == 0)
            substituted[sig] = 1;
    }
}

void d3g_signals_uninstall(void) {
    active = 0;
    for (int i = 0; i < NCAND; i++) {
        int sig = candidates[i];
        if (!substituted[sig]) continue;
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(sig, &sa, NULL);
        substituted[sig] = 0;
    }
}
