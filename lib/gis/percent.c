/*!
   \file lib/gis/percent.c

   \brief GIS Library - percentage progress functions.

   (C) 2001-2009, 2011 by the GRASS Development Team

   This program is free software under the GNU General Public License
   (>=v2). Read the file COPYING that comes with GRASS for details.

   \author GRASS Development Team
 */

#include <stdio.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include <grass/gis.h>

#define LOG_CAPACITY 1024
#define LOG_MSG_SIZE 128

typedef enum { EV_LOG, EV_PROGRESS } event_type_t;

typedef struct {
    atomic_bool ready;
    event_type_t type;
    size_t completed;
    size_t total;
    char message[LOG_MSG_SIZE];
} event_t;

typedef struct {
    event_t buffer[LOG_CAPACITY];
    atomic_size_t write_index;
    size_t read_index;
    atomic_size_t completed;
    size_t total;
    int info_format;
    bool output_enabled;
    atomic_long last_progress_ns;
    long interval_ns;
    size_t percent_step;
    atomic_size_t next_percent_threshold;
    atomic_bool stop;
} telemetry_t;

struct GPercentContext {
    telemetry_t telemetry;
    atomic_bool initialized;
    pthread_t consumer_thread;
    atomic_bool consumer_started;
};

static telemetry_t g_percent_telemetry;
static atomic_bool g_percent_initialized = false;
static atomic_bool g_percent_consumer_started = false;

static long now_ns(void);
static bool telemetry_has_pending_events(telemetry_t *);
static void telemetry_init(telemetry_t *, size_t, long);
static void telemetry_init_percent(telemetry_t *, size_t, size_t);
static void enqueue_event(telemetry_t *, event_t *);
static void telemetry_log(telemetry_t *, const char *);
static void telemetry_set_info_format(telemetry_t *t);
static void telemetry_set_output_enabled(telemetry_t *t);
static void telemetry_progress(telemetry_t *, size_t);
static void *telemetry_consumer(void *);
static void start_global_percent(size_t, size_t);

/// Creates an isolated progress-reporting context for concurrent work.
///
/// The returned context tracks progress for `total_num_elements` items and
/// emits progress updates whenever completion advances by at least
/// `percent_step` percentage points. If output is enabled by the current
/// runtime configuration, this function also starts the background consumer
/// thread used to flush queued telemetry events.
///
/// - Parameter total_num_elements: Total number of elements to process.
/// - Parameter percent_step: Minimum percentage increment that triggers a
///   progress event.
/// - Returns: A newly allocated `GPercentContext`, or `NULL` if allocation
///   fails.
GPercentContext *G_percent_context_create(size_t total_num_elements,
                                          size_t percent_step)
{
    GPercentContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    atomic_init(&ctx->initialized, true);
    telemetry_init_percent(&ctx->telemetry,
                           ((total_num_elements > 0) ? total_num_elements : 0),
                           ((percent_step > 0) ? percent_step : 0));
    atomic_init(&ctx->consumer_started, false);

    if (ctx->telemetry.output_enabled) {
        bool expected_started = false;
        if (atomic_compare_exchange_strong_explicit(
                &ctx->consumer_started, &expected_started, true,
                memory_order_acq_rel, memory_order_relaxed)) {
            pthread_create(&ctx->consumer_thread, NULL, telemetry_consumer,
                           &ctx->telemetry);
        }
    }

    return ctx;
}

void G_percent_context_destroy(GPercentContext *ctx)
{
    if (!ctx) {
        return;
    }

    if (!atomic_load_explicit(&ctx->initialized, memory_order_acquire)) {
        free(ctx);
        return;
    }

    atomic_store_explicit(&ctx->telemetry.stop, true, memory_order_release);

    if (ctx->telemetry.output_enabled &&
        atomic_exchange_explicit(&ctx->consumer_started, false,
                                 memory_order_acq_rel)) {
        pthread_join(ctx->consumer_thread, NULL);
    }

    atomic_store_explicit(&ctx->initialized, false, memory_order_release);
    free(ctx);
}

void G_percent_r(GPercentContext *ctx, size_t current_element)
{
    if (!ctx)
        return;
    if (!atomic_load_explicit(&ctx->initialized, memory_order_acquire))
        return;

    telemetry_t *t = &ctx->telemetry;
    if (t->total == 0 || t->percent_step == 0 || !t->output_enabled)
        return;

    size_t total = t->total;
    size_t completed = (current_element < 0) ? 0 : current_element;
    if (completed > total)
        completed = total;

    size_t current_pct = (size_t)((completed * 100) / total);
    size_t expected =
        atomic_load_explicit(&t->next_percent_threshold, memory_order_relaxed);
    while (current_pct >= expected && expected <= 100) {
        size_t next = expected + t->percent_step;
        if (next > 100)
            next = 101;
        if (atomic_compare_exchange_strong_explicit(
                &t->next_percent_threshold, &expected, next,
                memory_order_acq_rel, memory_order_relaxed)) {
            event_t ev = {0};
            ev.type = EV_PROGRESS;
            ev.completed = completed;
            ev.total = total;
            enqueue_event(t, &ev);
            return;
        }
    }
}

void G_percent(long current_element, long total_num_elements, int percent_step)
{
    if (total_num_elements <= 0)
        return;

    start_global_percent((size_t)total_num_elements, (size_t)percent_step);

    // If someone initialized with different totals/steps, we keep the first
    // ones for simplicity.

    size_t total = (size_t)total_num_elements;
    size_t completed = (current_element < 0) ? 0 : (size_t)current_element;
    if (completed > total)
        completed = total;

    if (g_percent_telemetry.percent_step == 0 ||
        !g_percent_telemetry.output_enabled)
        return; // not configured

    size_t current_pct = (size_t)((completed * 100) / total);
    size_t expected = atomic_load_explicit(
        &g_percent_telemetry.next_percent_threshold, memory_order_relaxed);
    while (current_pct >= expected && expected <= 100) {
        size_t next = expected + g_percent_telemetry.percent_step;
        if (next > 100)
            next = 101;
        if (atomic_compare_exchange_strong_explicit(
                &g_percent_telemetry.next_percent_threshold, &expected, next,
                memory_order_acq_rel, memory_order_relaxed)) {
            event_t ev = {0};
            ev.type = EV_PROGRESS;
            ev.completed = completed;
            ev.total = total;
            enqueue_event(&g_percent_telemetry, &ev);
            if (completed == total) {
                atomic_store_explicit(&g_percent_telemetry.stop, true,
                                      memory_order_release);
            }
            return;
        }
        // CAS failed; expected updated, loop continues
    }
}

static void *telemetry_consumer(void *arg)
{
    telemetry_t *t = arg;

    while (true) {
        if (atomic_load_explicit(&t->stop, memory_order_acquire) &&
            !telemetry_has_pending_events(t)) {
            break;
        }

        event_t *ev = &t->buffer[t->read_index % LOG_CAPACITY];

        if (!atomic_load_explicit(&ev->ready, memory_order_acquire)) {
            sched_yield();
            continue;
        }

        // handle event
        if (ev->type == EV_LOG) {
            printf("[LOG] %s\n", ev->message);
        }
        else if (ev->type == EV_PROGRESS) {
            double pct = (ev->total > 0)
                             ? (double)ev->completed * 100.0 / (double)ev->total
                             : 0.0;

            switch (t->info_format) {
            case G_INFO_FORMAT_STANDARD:
                fprintf(stderr, "%4d%%\b\b\b\b\b", (int)pct);
                break;
            case G_INFO_FORMAT_GUI:
                fprintf(stderr, "GRASS_INFO_PERCENT: %d", (int)pct);
                fflush(stderr);
                break;
            case G_INFO_FORMAT_PLAIN:
                fprintf(stderr, "%d%s", (int)pct,
                        ((int)pct == 100 ? "" : ".."));
                break;
            default:
                break;
            }
        }

        // mark slot free
        atomic_store_explicit(&ev->ready, false, memory_order_release);
        t->read_index++;
    }

    if (t == &g_percent_telemetry) {
        atomic_store_explicit(&g_percent_consumer_started, false,
                              memory_order_release);
        atomic_store_explicit(&g_percent_initialized, false,
                              memory_order_release);
    }

    return NULL;
}

static void telemetry_init(telemetry_t *t, size_t total, long interval_ms)
{
    atomic_init(&t->write_index, 0);
    t->read_index = 0;

    for (size_t i = 0; i < LOG_CAPACITY; ++i) {
        atomic_init(&t->buffer[i].ready, false);
    }

    atomic_init(&t->completed, 0);
    t->total = total;
    telemetry_set_info_format(t);
    telemetry_set_output_enabled(t);

    atomic_init(&t->last_progress_ns, 0);
    t->interval_ns = interval_ms * 1000000L;

    t->percent_step = 0; // 0 => disabled, use time-based if interval_ns > 0
    atomic_init(&t->next_percent_threshold, 0);

    atomic_init(&t->stop, false);
}

static void telemetry_init_percent(telemetry_t *t, size_t total,
                                   size_t percent_step)
{
    atomic_init(&t->write_index, 0);
    t->read_index = 0;
    for (size_t i = 0; i < LOG_CAPACITY; ++i) {
        atomic_init(&t->buffer[i].ready, false);
    }
    atomic_init(&t->completed, 0);
    t->total = total;
    telemetry_set_info_format(t);
    telemetry_set_output_enabled(t);

    // disable time-based gating
    atomic_init(&t->last_progress_ns, 0);
    t->interval_ns = 0;

    // enable percentage-based gating
    t->percent_step = percent_step;
    size_t first = percent_step > 0 ? percent_step : 0;
    atomic_init(&t->next_percent_threshold, first);

    atomic_init(&t->stop, false);
}

static void enqueue_event(telemetry_t *t, event_t *src)
{
    size_t idx =
        atomic_fetch_add_explicit(&t->write_index, 1, memory_order_relaxed);

    event_t *dst = &t->buffer[idx % LOG_CAPACITY];

    // wait until slot is free (bounded spin)
    while (atomic_load_explicit(&dst->ready, memory_order_acquire)) {
        sched_yield();
    }

    // copy payload
    *dst = *src;

    // publish
    atomic_store_explicit(&dst->ready, true, memory_order_release);
}

static bool telemetry_has_pending_events(telemetry_t *t)
{
    if (t->read_index !=
        atomic_load_explicit(&t->write_index, memory_order_acquire)) {
        return true;
    }

    event_t *ev = &t->buffer[t->read_index % LOG_CAPACITY];
    return atomic_load_explicit(&ev->ready, memory_order_acquire);
}

static void telemetry_log(telemetry_t *t, const char *msg)
{
    event_t ev = {0};
    ev.type = EV_LOG;
    snprintf(ev.message, LOG_MSG_SIZE, "%s", msg);

    enqueue_event(t, &ev);
}

static void telemetry_set_info_format(telemetry_t *t)
{
    t->info_format = G_info_format();
}

static void telemetry_set_output_enabled(telemetry_t *t)
{
    t->output_enabled =
        t->info_format != G_INFO_FORMAT_SILENT && G_verbose() >= 1;
}

/// Records completed work and enqueues a progress event when the next
/// reportable threshold is reached.
///
/// The function atomically increments the telemetry's completed counter by
/// `step`, then decides whether to emit a progress event using one of two
/// modes: percent-based reporting when `percent_step` and `total` are
/// configured, or time-based throttling when they are not. Atomic
/// compare-and-swap operations ensure that only one caller emits an event for a
/// given threshold or interval.
///
/// - Parameter t: The telemetry state to update and publish through.
/// - Parameter step: The number of newly completed units of work to add.
static void telemetry_progress(telemetry_t *t, size_t step)
{
    size_t new_completed =
        atomic_fetch_add_explicit(&t->completed, step, memory_order_relaxed) +
        step;

    if (t->percent_step > 0 && t->total > 0) {
        size_t current_pct = (size_t)((new_completed * 100) / t->total);
        size_t expected = atomic_load_explicit(&t->next_percent_threshold,
                                               memory_order_relaxed);
        while (current_pct >= expected && expected <= 100) {
            size_t next = expected + t->percent_step;
            if (next > 100)
                next = 101; // sentinel beyond 100 to stop further emits
            if (atomic_compare_exchange_strong_explicit(
                    &t->next_percent_threshold, &expected, next,
                    memory_order_acq_rel, memory_order_relaxed)) {
                // we won the right to emit at this threshold
                break;
            }
            // CAS failed, expected now contains the latest value; loop to
            // re-check
        }
        // If we didn't advance, nothing to emit
        if (current_pct < expected || expected > 100) {
            return;
        }
    }
    else {
        long now = now_ns();
        long last =
            atomic_load_explicit(&t->last_progress_ns, memory_order_relaxed);
        if (now - last < t->interval_ns) {
            return;
        }
        if (!atomic_compare_exchange_strong_explicit(
                &t->last_progress_ns, &last, now, memory_order_acq_rel,
                memory_order_relaxed)) {
            return;
        }
    }

    event_t ev = {0};
    ev.type = EV_PROGRESS;
    ev.completed = new_completed;
    ev.total = t->total;

    enqueue_event(t, &ev);
}

/// Initializes shared percent-based telemetry and starts the detached consumer
/// thread once.
///
/// This function performs one-time global setup for percent progress reporting.
/// Repeated calls return immediately after the initialization state has been
/// set. If output is disabled or the consumer thread cannot be created, no
/// further progress consumer setup is performed.
///
/// - Parameter total_num_elements: The total number of elements used to compute
/// progress percentages.
/// - Parameter percent_step: The percentage increment that controls when
/// progress updates are emitted.
static void start_global_percent(size_t total_num_elements, size_t percent_step)
{
    bool expected_init = false;
    if (!atomic_compare_exchange_strong_explicit(
            &g_percent_initialized, &expected_init, true, memory_order_acq_rel,
            memory_order_relaxed)) {
        return;
    }

    telemetry_init_percent(&g_percent_telemetry,
                           ((total_num_elements > 0) ? total_num_elements : 0),
                           ((percent_step > 0) ? percent_step : 0));

    if (!g_percent_telemetry.output_enabled) {
        return;
    }

    bool expected_started = false;
    if (atomic_compare_exchange_strong_explicit(
            &g_percent_consumer_started, &expected_started, true,
            memory_order_acq_rel, memory_order_relaxed)) {
        pthread_t consumer_thread;
        pthread_attr_t attr;

        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&consumer_thread, &attr, telemetry_consumer,
                           &g_percent_telemetry) != 0) {
            atomic_store_explicit(&g_percent_consumer_started, false,
                                  memory_order_release);
            atomic_store_explicit(&g_percent_initialized, false,
                                  memory_order_release);
        }
        pthread_attr_destroy(&attr);
    }
}

/// Returns the current UTC time in nanoseconds.
static long now_ns(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

// ----------------------------------------------------------------------------

static struct state {
    int prev;
    int first;
} state = {-1, 1};

static struct state *st = &state;
static int (*ext_percent)(int);

/*!
   \brief Print percent complete messages.

   This routine prints a percentage complete message to stderr. The
   percentage complete is <i>(<b>n</b>/<b>d</b>)*100</i>, and these are
   printed only for each <b>s</b> percentage. This is perhaps best
   explained by example:
   \code
   #include <stdio.h>
   #include <grass/gis.h>
   int row;
   int nrows;
   nrows = 1352; // 1352 is not a special value - example only

   G_message(_("Percent complete..."));
   for (row = 0; row < nrows; row++)
   {
   G_percent(row, nrows, 10);
   do_calculation(row);
   }
   G_percent(1, 1, 1);
   \endcode

   This example code will print completion messages at 10% increments;
   i.e., 0%, 10%, 20%, 30%, etc., up to 100%. Each message does not appear
   on a new line, but rather erases the previous message.

   Note that to prevent the illusion of the module stalling, the G_percent()
   call is placed before the time consuming part of the for loop, and an
   additional call is generally needed after the loop to "finish it off"
   at 100%.

   \param n current element
   \param d total number of elements
   \param s increment size
 */
void G_percent_old(long n, long d, int s)
{
    int x, format;

    format = G_info_format();

    x = (d <= 0 || s <= 0) ? 100 : (int)(100 * n / d);

    /* be verbose only 1> */
    if (format == G_INFO_FORMAT_SILENT || G_verbose() < 1)
        return;

    if (n <= 0 || n >= d || x > st->prev + s) {
        st->prev = x;

        if (ext_percent) {
            ext_percent(x);
        }
        else {
            if (format == G_INFO_FORMAT_STANDARD) {
                fprintf(stderr, "%4d%%\b\b\b\b\b", x);
            }
            else {
                if (format == G_INFO_FORMAT_PLAIN) {
                    if (x == 100)
                        fprintf(stderr, "%d\n", x);
                    else
                        fprintf(stderr, "%d..", x);
                }
                else { /* GUI */
                    if (st->first) {
                        fprintf(stderr, "\n");
                    }
                    fprintf(stderr, "GRASS_INFO_PERCENT: %d\n", x);
                    fflush(stderr);
                    st->first = 0;
                }
            }
        }
    }

    if (x >= 100) {
        if (ext_percent) {
            ext_percent(100);
        }
        else if (format == G_INFO_FORMAT_STANDARD) {
            fprintf(stderr, "\n");
        }
        st->prev = -1;
        st->first = 1;
    }
}

/*!
   \brief Reset G_percent() to 0%; do not add newline.
 */
void G_percent_reset(void)
{
    st->prev = -1;
    st->first = 1;
}

/*!
   \brief Print progress info messages

   Use G_percent() when number of elements is defined.

   This routine prints a progress info message to stderr. The value
   <b>n</b> is printed only for each <b>s</b>. This is perhaps best
   explained by example:
   \code
   #include <grass/vector.h>

   int line;

   G_message(_("Reading features..."));
   line = 0;
   while(TRUE)
   {
   if (Vect_read_next_line(Map, Points, Cats) < 0)
   break;
   line++;
   G_progress(line, 1e3);
   }
   G_progress(1, 1);
   \endcode

   This example code will print progress in messages at 1000
   increments; i.e., 1000, 2000, 3000, 4000, etc., up to number of
   features for given vector map. Each message does not appear on a new
   line, but rather erases the previous message.

   \param n current element
   \param s increment size

   \return always returns 0
 */
void G_progress(long n, int s)
{
    int format;

    format = G_info_format();

    /* be verbose only 1> */
    if (format == G_INFO_FORMAT_SILENT || G_verbose() < 1)
        return;

    if (n == s && n == 1) {
        if (format == G_INFO_FORMAT_PLAIN)
            fprintf(stderr, "\n");
        else if (format != G_INFO_FORMAT_GUI)
            fprintf(stderr, "\r");
        return;
    }

    if (n % s == 0) {
        if (format == G_INFO_FORMAT_PLAIN)
            fprintf(stderr, "%ld..", n);
        else if (format == G_INFO_FORMAT_GUI)
            fprintf(stderr, "GRASS_INFO_PROGRESS: %ld\n", n);
        else
            fprintf(stderr, "%10ld\b\b\b\b\b\b\b\b\b\b", n);
    }
}

/*!
   \brief Establishes percent_routine as the routine that will handle
   the printing of percentage progress messages.

   \param percent_routine routine will be called like this: percent_routine(x)
 */
void G_set_percent_routine(int (*percent_routine)(int))
{
    ext_percent = percent_routine;
}

/*!
   \brief After this call subsequent percentage progress messages will
   be handled in the default method.

   Percentage progress messages are printed directly to stderr.
 */
void G_unset_percent_routine(void)
{
    ext_percent = NULL;
}
