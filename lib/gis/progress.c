#include <assert.h>
#include <stdio.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include <grass/gis.h>

#define LOG_CAPACITY       1024
#define LOG_MSG_SIZE       128
#define TIME_RATE_LIMIT_MS 100

typedef enum { EV_LOG, EV_PROGRESS } event_type_t;

typedef struct {
    event_type_t type;
    size_t completed;
    size_t total;
    char message[LOG_MSG_SIZE];
    atomic_bool ready;
} event_t;

typedef struct {
    event_t buffer[LOG_CAPACITY];
    atomic_size_t write_index;
    size_t read_index;
    atomic_size_t completed;
    size_t total;
    int info_format;
    atomic_long last_progress_ns;
    long interval_ns;
    size_t percent_step;
    atomic_size_t next_percent_threshold;
    atomic_bool stop;
    GProgressSink
        sink; // optional sink; if callbacks are NULL, fall back to info_format
} telemetry_t;

typedef void (*context_progress_fn)(telemetry_t *, size_t);

struct GProgressContext {
    telemetry_t telemetry;
    context_progress_fn report_progress;
    GProgressSink sink; // per-context override (optional)
    atomic_bool initialized;
    pthread_t consumer_thread;
    atomic_bool consumer_started;
};

static telemetry_t g_percent_telemetry;
static atomic_bool g_percent_initialized = false;
static atomic_bool g_percent_consumer_started = false;
static GProgressSink g_percent_sink = {0};

static GProgressContext *context_create(size_t, size_t, long);
static bool telemetry_has_pending_events(telemetry_t *);
static void telemetry_init_time(telemetry_t *, size_t, long);
static void telemetry_init_percent(telemetry_t *, size_t, size_t);
static void enqueue_event(telemetry_t *, event_t *);
static void telemetry_enqueue_final_progress(telemetry_t *);
static void telemetry_log(telemetry_t *, const char *);
static void telemetry_set_info_format(telemetry_t *);
static void telemetry_progress(telemetry_t *, size_t);
static void context_progress_percent(telemetry_t *, size_t);
static void context_progress_time(telemetry_t *, size_t);
static void *telemetry_consumer(void *);
static void start_global_percent(size_t, size_t);
static bool output_is_silenced(void);
static long now_ns(void);

// Legacy compatibility: adapter for void (*fn)(int)
static void legacy_percent_adapter(const GProgressEvent *e, void *ud)
{
    void (*fn)(int) = (void (*)(int))ud;
    if (fn) {
        int pct = (int)(e->percent);
        fn(pct);
    }
}

/// Creates an isolated progress-reporting context for concurrent work.
///
/// The returned context tracks progress for `total_num_elements` items and
/// emits progress updates whenever completion advances by at least
/// `percent_step` percentage points. `total_num_elements` must match the
/// actual number of work units that will be reported through
/// `G_progress_update()`. In particular, callers should pass a completed-work
/// count, not a raw loop index or a larger container size, otherwise the
/// terminal `100%` update may never be reached. If output is enabled by the
/// current runtime configuration, this function also starts the background
/// consumer thread used to flush queued telemetry events.
///
/// \param total_num_elements Total number of elements to process.
/// \param step Minimum percentage increment that triggers a
///   progress event.
/// \return A newly allocated `GPercentContext`, or `NULL` if output
///   is silenced by environment variable `GRASS_MESSAGE_FORMAT` or
///   verbosity level is below `1`.
GProgressContext *G_progress_context_create(size_t total_num_elements,
                                            size_t step)
{
    return context_create(total_num_elements, step,
                          (step == 0 ? TIME_RATE_LIMIT_MS : 0));
}

GProgressContext *G_progress_context_create_time(size_t total_num_elements,
                                                 long interval_ms)
{
    return context_create(total_num_elements, 0, interval_ms);
}

/// Destroys a `GPercentContext` and releases any resources it owns.
///
/// This function stops the context's background telemetry consumer, waits for
/// the consumer thread to finish when it was started, marks the context as no
/// longer initialized, and frees the context memory. Passing `NULL` is safe and
/// has no effect.
///
/// \param ctx The progress-reporting context previously created by
///   `G_percent_context_create()`, or `NULL`.
void G_progress_context_destroy(GProgressContext *ctx)
{
    if (!ctx) {
        return;
    }

    if (!atomic_load_explicit(&ctx->initialized, memory_order_acquire)) {
        G_free(ctx);
        return;
    }

    if (atomic_load_explicit(&ctx->telemetry.completed, memory_order_acquire) >=
            ctx->telemetry.total &&
        atomic_load_explicit(&ctx->telemetry.next_percent_threshold,
                             memory_order_acquire) <= 100) {
        telemetry_enqueue_final_progress(&ctx->telemetry);
    }

    atomic_store_explicit(&ctx->telemetry.stop, true, memory_order_release);

    if (atomic_exchange_explicit(&ctx->consumer_started, false,
                                 memory_order_acq_rel)) {
        pthread_join(ctx->consumer_thread, NULL);
    }

    atomic_store_explicit(&ctx->initialized, false, memory_order_release);
    G_free(ctx);
}

void G_progress_context_set_sink(GProgressContext *ctx,
                                 const GProgressSink *sink)
{
    if (!ctx)
        return;
    if (sink) {
        ctx->sink = *sink;
    }
    else {
        ctx->sink.on_progress = NULL;
        ctx->sink.on_log = NULL;
        ctx->sink.user_data = NULL;
    }
    // update telemetry copy; safe because sink is read-only by consumer after
    // set
    ctx->telemetry.sink = ctx->sink;
}

void G_percent_set_sink(const GProgressSink *sink)
{
    if (sink) {
        g_percent_sink = *sink;
    }
    else {
        g_percent_sink.on_progress = NULL;
        g_percent_sink.on_log = NULL;
        g_percent_sink.user_data = NULL;
    }
    // apply immediately to global telemetry if initialized
    g_percent_telemetry.sink = g_percent_sink;
}

/// Reports progress for an isolated `GPercentContext` instance.
///
/// This re-entrant variant of `G_percent` is intended for concurrent or
/// context-specific work. It validates that `ctx` is initialized, clamps
/// `current_element` to the valid `0...total` range, and enqueues a progress
/// event only when the computed percentage reaches the next configured
/// threshold for the context.
///
/// Callers typically create the context with `G_percent_context_create()`, call
/// this function as work advances, and later release resources with
/// `G_percent_context_destroy()`.
///
/// \param ctx The progress-reporting context created by
///   `G_percent_context_create()`.
/// \param completed: The current completed element index or count.
void G_progress_update(GProgressContext *ctx, size_t completed)
{
    if (!ctx)
        return;
    if (!atomic_load_explicit(&ctx->initialized, memory_order_acquire))
        return;

    telemetry_t *t = &ctx->telemetry;
    if (t->total == 0)
        return;

    size_t total = t->total;
    if (completed > total)
        completed = total;

    atomic_store_explicit(&t->completed, completed, memory_order_release);
    ctx->report_progress(t, completed);
}

// Compatibility layer for legacy percent routine API
void G_set_percent_routine(int (*fn)(int))
{
    // The historical signature in gis.h declares int (*)(int), but actual
    // implementers often used void(*)(int). We accept int-returning and ignore
    // the return value.
    if (!fn) {
        // Reset to default behavior
        G_percent_set_sink(NULL);
        return;
    }
    // Wrap the legacy function pointer in a sink that casts and calls with
    // percent
    GProgressSink s = {0};
    s.on_progress = legacy_percent_adapter;
    // Store the function pointer in user_data with a cast that preserves
    // address
    s.user_data = (void *)fn;
    G_percent_set_sink(&s);
}

void G_unset_percent_routine(void)
{
    // Reset to default (env-driven G_info_format output)
    G_percent_set_sink(NULL);
}

/// Reports global progress when completion crosses the next percentage step.
///
/// This function initializes the shared global telemetry stream on first use,
/// clamps `current_element` into the valid `0...total_num_elements` range, and
/// enqueues a progress update only when the computed percentage reaches the
/// next configured threshold. When progress reaches the total, a terminal
/// `100%` event is always queued and the background consumer is asked to stop
/// after pending events have been flushed.
///
///  \param current_element The current completed element index or count.
///  \param total_num_elements The total number of elements to process. Values
///    less than or equal to `0` disable reporting.
///  \param percent_step The minimum percentage increment required before a new
///    progress event is emitted.
void G_percent(long current_element, long total_num_elements, int percent_step)
{
    if (total_num_elements <= 0 || output_is_silenced())
        return;

    start_global_percent((size_t)total_num_elements, (size_t)percent_step);

    // If someone initialized with different totals/steps, we keep the first
    // ones for simplicity.

    size_t total = (size_t)total_num_elements;
    size_t completed = (current_element < 0) ? 0 : (size_t)current_element;
    if (completed > total)
        completed = total;

    if (g_percent_telemetry.percent_step == 0)
        return; // not configured

    atomic_store_explicit(&g_percent_telemetry.completed, completed,
                          memory_order_release);

    if (completed == total) {
        telemetry_enqueue_final_progress(&g_percent_telemetry);
        atomic_store_explicit(&g_percent_telemetry.stop, true,
                              memory_order_release);
        return;
    }

    size_t current_pct = (size_t)((completed * 100) / total);
    size_t expected = atomic_load_explicit(
        &g_percent_telemetry.next_percent_threshold, memory_order_relaxed);
    while (current_pct >= expected && expected <= 100) {
        size_t next = expected + g_percent_telemetry.percent_step;
        if (expected < 100 && next > 100)
            next = 100;
        else if (next > 100)
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

static GProgressContext *context_create(size_t total_num_elements, size_t step,
                                        long interval_ms)
{
    if (output_is_silenced())
        return NULL;

    GProgressContext *ctx = G_calloc(1, sizeof(*ctx));

    atomic_init(&ctx->initialized, true);

    assert(step <= 100);

    if (step == 0) {
        assert(interval_ms > 0);
        telemetry_init_time(&ctx->telemetry, total_num_elements, interval_ms);
        ctx->report_progress = context_progress_time;
    }
    else {
        telemetry_init_percent(&ctx->telemetry, total_num_elements, step);
        ctx->report_progress = context_progress_percent;
    }

    ctx->sink.on_progress = NULL;
    ctx->sink.on_log = NULL;
    ctx->sink.user_data = NULL;

    // propagate context sink to telemetry by default
    ctx->telemetry.sink = ctx->sink;

    atomic_init(&ctx->consumer_started, false);

    bool expected_started = false;
    if (atomic_compare_exchange_strong_explicit(
            &ctx->consumer_started, &expected_started, true,
            memory_order_acq_rel, memory_order_relaxed)) {
        pthread_create(&ctx->consumer_thread, NULL, telemetry_consumer,
                       &ctx->telemetry);
    }

    return ctx;
}

static void context_progress_percent(telemetry_t *t, size_t completed)
{
    size_t total = t->total;

    if (completed == total) {
        telemetry_enqueue_final_progress(t);
        return;
    }

    size_t current_pct = (size_t)((completed * 100) / total);
    size_t expected =
        atomic_load_explicit(&t->next_percent_threshold, memory_order_relaxed);
    while (current_pct >= expected && expected <= 100) {
        size_t next = expected + t->percent_step;
        if (expected < 100 && next > 100)
            next = 100;
        else if (next > 100)
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

static void context_progress_time(telemetry_t *t, size_t completed)
{
    if (completed == t->total) {
        telemetry_enqueue_final_progress(t);
        return;
    }

    long now = now_ns();
    long last =
        atomic_load_explicit(&t->last_progress_ns, memory_order_relaxed);

    if (now - last < t->interval_ns) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(&t->last_progress_ns, &last,
                                                 now, memory_order_acq_rel,
                                                 memory_order_relaxed)) {
        return;
    }

    event_t ev = {0};
    ev.type = EV_PROGRESS;
    ev.completed = completed;
    ev.total = t->total;
    enqueue_event(t, &ev);
}

/// Consumes queued telemetry events and emits log or progress output until
/// shutdown is requested and the event buffer has been drained.
///
/// \param arg Pointer to the `telemetry_t` instance whose ring buffer and
///   formatting settings should be consumed.
/// \return `NULL` after the consumer loop exits and any global consumer state
///   has been reset.
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
            if (t->sink.on_log) {
                t->sink.on_log(ev->message, t->sink.user_data);
            }
            else {
                // default logging
                printf("[LOG] %s\n", ev->message);
            }
        }
        else if (ev->type == EV_PROGRESS) {
            double pct = (ev->total > 0)
                             ? (double)ev->completed * 100.0 / (double)ev->total
                             : 0.0;
            bool is_terminal = (ev->total > 0 && ev->completed >= ev->total);

            if (t->sink.on_progress) {
                GProgressEvent pe = {
                    .completed = ev->completed,
                    .total = ev->total,
                    .percent = pct,
                    .is_terminal = is_terminal,
                };
                t->sink.on_progress(&pe, t->sink.user_data);
            }
            else {
                // Default rendering honors info_format
                switch (t->info_format) {
                case G_INFO_FORMAT_STANDARD:
                    fprintf(stderr, "%4d%%\b\b\b\b\b", (int)pct);
                    if ((int)pct == 100)
                        fprintf(stderr, "\n");
                    break;
                case G_INFO_FORMAT_GUI:
                    fprintf(stderr, "GRASS_INFO_PERCENT: %d\n", (int)pct);
                    fflush(stderr);
                    break;
                case G_INFO_FORMAT_PLAIN:
                    fprintf(stderr, "%d%s", (int)pct,
                            ((int)pct == 100 ? "\n" : ".."));
                    break;
                default:
                    break;
                }
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
        // keep g_percent_sink as-is; no change needed on shutdown
    }

    return NULL;
}

static void telemetry_init_time(telemetry_t *t, size_t total, long interval_ms)
{
    atomic_init(&t->write_index, 0);
    t->read_index = 0;

    for (size_t i = 0; i < LOG_CAPACITY; ++i) {
        atomic_init(&t->buffer[i].ready, false);
    }

    atomic_init(&t->completed, 0);
    t->total = total;
    telemetry_set_info_format(t);

    atomic_init(&t->last_progress_ns, 0);
    t->interval_ns = interval_ms * 1000000L;

    t->percent_step = 0; // 0 => disabled, use time-based if interval_ns > 0
    atomic_init(&t->next_percent_threshold, 0);

    atomic_init(&t->stop, false);

    // default: no custom sink; callbacks NULL imply fallback to info_format
    t->sink.on_progress = NULL;
    t->sink.on_log = NULL;
    t->sink.user_data = NULL;
}

/// Initializes telemetry state for percentage-based progress reporting.
///
/// Resets the telemetry ring buffer and counters, disables time-based
/// throttling, and configures the next progress event to be emitted when the
/// completed work reaches the first `percent_step` threshold.
///
/// \param t The telemetry instance to reset and configure.
/// \param total The total number of work units expected for the tracked
///   operation.
/// \param percent_step The percentage increment that controls when
///   progress updates are emitted. A value of `0` disables percentage-based
///   thresholds.
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

    // disable time-based gating
    atomic_init(&t->last_progress_ns, 0);
    t->interval_ns = 0;

    // enable percentage-based gating
    t->percent_step = percent_step;
    size_t first = percent_step > 0 ? percent_step : 0;
    atomic_init(&t->next_percent_threshold, first);

    atomic_init(&t->stop, false);

    // default: no custom sink; callbacks NULL imply fallback to info_format
    t->sink.on_progress = NULL;
    t->sink.on_log = NULL;
    t->sink.user_data = NULL;
}

/// Queues a telemetry event into the ring buffer for later consumption.
///
/// Waits until the destination slot becomes available, copies the event payload
/// into that slot, and then marks the slot as ready using release semantics so
/// readers can safely observe the published event.
///
/// \param t The telemetry instance that owns the event buffer.
/// \param src The event payload to enqueue.
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

/// Queues a terminal `100%` progress event for a telemetry stream.
///
/// This helper records the stream as fully completed, disables further
/// percentage-threshold reporting, and enqueues one last progress event with
/// `completed == total` so the consumer can emit the final `100%` update.
///
/// \param t The telemetry instance to finalize.
static void telemetry_enqueue_final_progress(telemetry_t *t)
{
    event_t ev = {0};

    atomic_store_explicit(&t->completed, t->total, memory_order_release);
    atomic_store_explicit(&t->next_percent_threshold, 101,
                          memory_order_release);

    ev.type = EV_PROGRESS;
    ev.completed = t->total;
    ev.total = t->total;
    enqueue_event(t, &ev);
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

/// Captures the current GRASS info output format for subsequent telemetry.
///
/// Reads the process-wide info formatting mode and stores it on the telemetry
/// instance so later progress and log events can format output consistently.
///
/// \param t The telemetry state that caches the active info format.
static void telemetry_set_info_format(telemetry_t *t)
{
    t->info_format = G_info_format();
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
/// \param t The telemetry state to update and publish through.
/// \param step The number of newly completed units of work to add.
static void telemetry_progress(telemetry_t *t, size_t step)
{
    size_t new_completed =
        atomic_fetch_add_explicit(&t->completed, step, memory_order_relaxed) +
        step;

    if (t->percent_step > 0 && t->total > 0) {
        if (new_completed >= t->total) {
            telemetry_enqueue_final_progress(t);
            return;
        }

        size_t current_pct = (size_t)((new_completed * 100) / t->total);
        size_t expected = atomic_load_explicit(&t->next_percent_threshold,
                                               memory_order_relaxed);
        while (current_pct >= expected && expected <= 100) {
            size_t next = expected + t->percent_step;
            if (expected < 100 && next > 100)
                next = 100;
            else if (next > 100)
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
/// \param total_num_elements The total number of elements used to compute
///   progress percentages.
/// \param percent_step The percentage increment that controls when
///   progress updates are emitted.
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

    // attach current global sink (may be empty for default behavior)
    g_percent_telemetry.sink = g_percent_sink;

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

static bool output_is_silenced(void)
{
    return (G_info_format() == G_INFO_FORMAT_SILENT || G_verbose() < 1);
}

/// Returns the current UTC time in nanoseconds.
static long now_ns(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}
