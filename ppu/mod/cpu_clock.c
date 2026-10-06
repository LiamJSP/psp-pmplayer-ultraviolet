#include "cpu_clock.h"
#include "common/ppa_playback_session.h"
#include "../common/ppa_bus_manager.h"
#include "../common/ppa_memory.h"

#include <pspkernel.h>
#include <psppower.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PPA_CPU_GOVERNOR_FAST_DESCENT
#define PPA_CPU_GOVERNOR_FAST_DESCENT 1
#endif

static int cpu_clock_auto_window_is_safe_at_current(unsigned int budget_us,
                                                    unsigned int avg_abs_ms);
static unsigned int cpu_clock_auto_display_interval_avg_us(void);
static unsigned int cpu_clock_auto_display_fps_x1000(void);
static unsigned int cpu_clock_auto_lifetime_display_fps_x1000(void);
static unsigned int cpu_clock_auto_min_displayed_samples(unsigned int budget_us);
static unsigned int cpu_clock_auto_decode_avg_us(void);
static unsigned int cpu_clock_auto_gu_avg_us(void);
static unsigned int cpu_clock_auto_audio_per_display_avg_us(void);
static int cpu_clock_auto_fast_candidate_has_headroom(int target_index,
                                                      unsigned int budget_us);
static int cpu_clock_auto_fast_descent_allowed(unsigned int budget_us,
                                               unsigned int avg_abs_ms);

struct speed_setting_struct current_speed;

/*
 * Aggressive idle/manual default.  The PLL remains at the PSP-safe 222 MHz
 * base while CPU and bus requests drop to 66/33.  Bench builds record both
 * requested and actual clocks because firmware may quantize a low bus value.
 */
static struct speed_setting_struct manual_speed = {66, 222, 33};

static int playback_clock_lock = 0;
static int auto_clock_enabled = 0;
static int extreme_battery_saver = 0;
static int extreme_battery_saved_auto_enabled = 0;
static int extreme_battery_saved_auto_valid = 0;

#define CPU_CLOCK_AUTO_EVAL_US              1000000ULL
#define CPU_CLOCK_AUTO_EVAL_HIGH_US          500000ULL
#define CPU_CLOCK_AUTO_EVAL_MID_US           750000ULL
#define CPU_CLOCK_AUTO_WARMUP_MIN_US         500000ULL
#define CPU_CLOCK_AUTO_WARMUP_DEADLINE_US   1500000ULL
#define CPU_CLOCK_AUTO_SETTLE_US             750000ULL
#define CPU_CLOCK_AUTO_DANGER_HOLD_US       2500000ULL
#define CPU_CLOCK_SAVER_TRANSIENT_US         500000ULL
#define CPU_CLOCK_SAVER_RECOVERY_HOLD_US      750000ULL

/*
 * Playback governor steps, low to high.
 *
 * 111 and 166 are intentionally included only as auto-governor candidates
 * after stable playback has been observed. Playback always starts at 333.
 */
static const int auto_clock_steps[] = {
	66,
	111,
	133,
	166,
	222,
	266,
	333
};

#define CPU_CLOCK_AUTO_STEP_COUNT \
	((int)(sizeof(auto_clock_steps) / sizeof(auto_clock_steps[0])))

#define CPU_CLOCK_AUTO_MIN_INDEX 0
#define CPU_CLOCK_AUTO_MAX_INDEX (CPU_CLOCK_AUTO_STEP_COUNT - 1)

struct cpu_clock_auto_state {
	int current_index;
	int start_cpu;
	int min_cpu;
	int max_cpu;

	uint64_t started_us;
	uint64_t last_eval_us;
	uint64_t last_change_us;
	uint64_t boost_until_us;
	uint64_t optional_work_block_until_us;
	uint64_t warmup_min_until_us;
	uint64_t warmup_deadline_us;
	uint64_t window_started_us;
	uint64_t last_displayed_us;
	uint64_t saver_danger_since_us;
	uint64_t saver_last_skip_us;

	uint64_t clock_us_66;
	uint64_t clock_us_111;
	uint64_t clock_us_133;
	uint64_t clock_us_166;
	uint64_t clock_us_222;
	uint64_t clock_us_266;
	uint64_t clock_us_333;
	uint64_t bus_us_33;
	uint64_t bus_us_55;
	uint64_t bus_us_66;
	uint64_t bus_us_83;
	uint64_t bus_us_111;
	uint64_t bus_us_133;
	uint64_t bus_us_166;

	unsigned int frame_duration_us;
	unsigned int audio_only_blocks, audio_only_max_gap_us, audio_only_empty_count;
	unsigned int audio_only_codec_max_us;
	unsigned int lifetime_audio_only_blocks;
	uint64_t audio_only_last_complete_us;

	unsigned int displayed_count;
	unsigned int skipped_count;

	unsigned int displayed_abs_total_ms;
	unsigned int displayed_abs_max_ms;
	unsigned int display_interval_count;
	unsigned int display_interval_total_us;
	unsigned int display_interval_max_us;

	unsigned int decode_count;
	unsigned int decode_total_us;
	unsigned int decode_max_us;

	unsigned int gu_count;
	unsigned int gu_total_us;
	unsigned int gu_max_us;
	unsigned int postprocess_count;
	unsigned int postprocess_total_us;
	unsigned int postprocess_max_us;
	unsigned int audio_resample_count;
	unsigned int audio_resample_total_us;
	unsigned int audio_resample_max_us;

	unsigned int lifetime_displayed_count;
	unsigned int lifetime_skipped_count;
	unsigned int lifetime_decode_count;
	unsigned int lifetime_gu_count;
	unsigned int lifetime_postprocess_count;
	unsigned int lifetime_audio_resample_count;
	unsigned int lifetime_display_interval_count;
	uint64_t lifetime_display_interval_total_us;
	unsigned int lifetime_display_interval_max_us;

	unsigned int eval_count;
	unsigned int change_count;
	unsigned int downshift_count;
	unsigned int upshift_count;
	unsigned int boost_count;
	unsigned int danger_count;
	unsigned int skip_boost_count;
	unsigned int warmup_block_count;
	unsigned int unstable_block_count;
	unsigned int optional_work_shed_count;
	unsigned int producer_overflow_count;
	unsigned int lifetime_producer_overflow_count;

	unsigned int stable_windows;
	unsigned int warmup_required_presentations;
	unsigned int warmup_complete;

};

/* Each timing domain has one hot-path producer and the completed-presentation
 * owner is its sole consumer.  A small SPSC sample queue gives each observation
 * a single commit point: a context switch can delay a sample, but cannot split
 * its count/total/maximum fields across governor windows.  No polling thread or
 * mutex is introduced. */
#define CPU_CLOCK_PRODUCER_QUEUE_CAPACITY 256U
#define CPU_CLOCK_PRODUCER_QUEUE_MASK \
	(CPU_CLOCK_PRODUCER_QUEUE_CAPACITY - 1U)
#if (CPU_CLOCK_PRODUCER_QUEUE_CAPACITY == 0U) || \
    ((CPU_CLOCK_PRODUCER_QUEUE_CAPACITY & \
      (CPU_CLOCK_PRODUCER_QUEUE_CAPACITY - 1U)) != 0U)
#error "CPU clock producer queue capacity must be a power of two"
#endif

struct cpu_clock_auto_producer {
	volatile unsigned int write_index;
	volatile unsigned int read_index;
	volatile unsigned int overflow_count;
	unsigned int samples[CPU_CLOCK_PRODUCER_QUEUE_CAPACITY];
};

/* One paired commit keeps the codec portion associated with the same whole
 * service sample even when output pre-empts the producer between measurements.
 * Audio-only producer owns writes; blocking audio output owns the drain. */
struct cpu_clock_audio_only_sample { unsigned int total_us, codec_us; };
static struct {
	volatile unsigned int write_index, read_index, overflow_count, discard_pending;
	struct cpu_clock_audio_only_sample samples[CPU_CLOCK_PRODUCER_QUEUE_CAPACITY];
} g_audio_only_producer;

static struct cpu_clock_auto_state auto_state;
static struct cpu_clock_auto_producer g_decode_producer;
static struct cpu_clock_auto_producer g_gu_producer;
static struct cpu_clock_auto_producer g_postprocess_producer;
static struct cpu_clock_auto_producer g_audio_resample_producer;

static void cpu_clock_auto_account_time(uint64_t now);
static void cpu_clock_auto_note_cpu_bounds(void);

static int cpu_clock_auto_window_allows_downshift_to(int target_index,
                                                     unsigned int budget_us,
                                                     unsigned int avg_abs_ms);

static void cpu_clock_auto_publish(struct cpu_clock_auto_producer *producer,
                                   unsigned int elapsed_us)
{
	unsigned int write_index = producer->write_index;
	unsigned int read_index =
		__sync_fetch_and_add(&producer->read_index, 0U);

	if (write_index - read_index >= CPU_CLOCK_PRODUCER_QUEUE_CAPACITY) {
		/* Never block a decode/audio producer.  Overflow is a conservative
		 * governor danger signal and is expected to remain zero in normal use. */
		__sync_fetch_and_add(&producer->overflow_count, 1U);
		return;
	}

	producer->samples[write_index & CPU_CLOCK_PRODUCER_QUEUE_MASK] =
		elapsed_us;
	__sync_synchronize();
	producer->write_index = write_index + 1U;
}

static unsigned int cpu_clock_auto_drain_one(
    struct cpu_clock_auto_producer *producer,
    unsigned int *window_count, unsigned int *window_total_us,
    unsigned int *window_maximum_us, unsigned int *lifetime_count,
    int include_window)
{
	unsigned int read_index = producer->read_index;
	unsigned int write_index =
		__sync_fetch_and_add(&producer->write_index, 0U);
	unsigned int count = 0U;
	unsigned int total_us = 0U;
	unsigned int maximum_us = 0U;
	unsigned int overflow;

	while (read_index != write_index) {
		unsigned int elapsed_us =
			producer->samples[read_index & CPU_CLOCK_PRODUCER_QUEUE_MASK];
		++read_index;
		++count;
		total_us += elapsed_us;
		if (elapsed_us > maximum_us)
			maximum_us = elapsed_us;
	}
	__sync_synchronize();
	producer->read_index = read_index;
	overflow = __sync_lock_test_and_set(&producer->overflow_count, 0U);

	*lifetime_count += count + overflow;
	if (!include_window)
		return overflow;
	*window_count += count;
	*window_total_us += total_us;
	if (maximum_us > *window_maximum_us)
		*window_maximum_us = maximum_us;
	return overflow;
}

static unsigned int cpu_clock_auto_drain_audio_only(int include_window)
{
	unsigned int read_index = g_audio_only_producer.read_index;
	unsigned int write_index = __sync_fetch_and_add(&g_audio_only_producer.write_index, 0U);
	unsigned int overflow;
	while (read_index != write_index) {
		struct cpu_clock_audio_only_sample s =
			g_audio_only_producer.samples[read_index & CPU_CLOCK_PRODUCER_QUEUE_MASK];
		++read_index;
		++auto_state.lifetime_decode_count;
		if (include_window) {
			uint64_t total = (uint64_t)auto_state.decode_total_us + s.total_us;
			++auto_state.decode_count;
			auto_state.decode_total_us = total > 0xffffffffULL ? 0xffffffffU : (unsigned int)total;
			if (s.total_us > auto_state.decode_max_us) auto_state.decode_max_us = s.total_us;
			if (s.codec_us > auto_state.audio_only_codec_max_us)
				auto_state.audio_only_codec_max_us = s.codec_us;
		}
	}
	__sync_synchronize();
	g_audio_only_producer.read_index = read_index;
	overflow = __sync_lock_test_and_set(&g_audio_only_producer.overflow_count, 0U);
	auto_state.lifetime_decode_count += overflow;
	return overflow;
}

static void cpu_clock_auto_collect_producers(int include_window)
{
	unsigned int overflow = 0U;
	/* Controls can request a transition discard, but only audio output may
	 * advance this pair queue's read index while workers are live. It consumes
	 * the request at its next completion, excluding a bounded old-regime tail. */
	if (!include_window)
		__sync_lock_test_and_set(&g_audio_only_producer.discard_pending, 1U);
	overflow += cpu_clock_auto_drain_one(&g_decode_producer,
		&auto_state.decode_count, &auto_state.decode_total_us,
		&auto_state.decode_max_us, &auto_state.lifetime_decode_count,
		include_window);
	overflow += cpu_clock_auto_drain_one(&g_gu_producer,
		&auto_state.gu_count, &auto_state.gu_total_us,
		&auto_state.gu_max_us, &auto_state.lifetime_gu_count,
		include_window);
	overflow += cpu_clock_auto_drain_one(&g_postprocess_producer,
		&auto_state.postprocess_count, &auto_state.postprocess_total_us,
		&auto_state.postprocess_max_us,
		&auto_state.lifetime_postprocess_count, include_window);
	overflow += cpu_clock_auto_drain_one(&g_audio_resample_producer,
		&auto_state.audio_resample_count,
		&auto_state.audio_resample_total_us,
		&auto_state.audio_resample_max_us,
		&auto_state.lifetime_audio_resample_count, include_window);
	auto_state.lifetime_producer_overflow_count += overflow;
	if (include_window)
		auto_state.producer_overflow_count += overflow;
}

static void cpu_clock_auto_discard_one(
    struct cpu_clock_auto_producer *producer)
{
	unsigned int write_index =
		__sync_fetch_and_add(&producer->write_index, 0U);
	__sync_lock_test_and_set(&producer->read_index, write_index);
	(void)__sync_lock_test_and_set(&producer->overflow_count, 0U);
}

static void cpu_clock_auto_discard_pending_producers(void)
{
	unsigned int audio_write = __sync_fetch_and_add(&g_audio_only_producer.write_index, 0U);
	__sync_lock_test_and_set(&g_audio_only_producer.read_index, audio_write);
	(void)__sync_lock_test_and_set(&g_audio_only_producer.overflow_count, 0U);
	(void)__sync_lock_test_and_set(&g_audio_only_producer.discard_pending, 0U);
	cpu_clock_auto_discard_one(&g_decode_producer);
	cpu_clock_auto_discard_one(&g_gu_producer);
	cpu_clock_auto_discard_one(&g_postprocess_producer);
	cpu_clock_auto_discard_one(&g_audio_resample_producer);
}

static uint64_t cpu_clock_auto_eval_interval_us(void)
{
	int cpu = auto_clock_steps[auto_state.current_index];
	if (extreme_battery_saver || cpu >= 222)
		return CPU_CLOCK_AUTO_EVAL_HIGH_US;
	if (cpu >= 111)
		return CPU_CLOCK_AUTO_EVAL_MID_US;
	return CPU_CLOCK_AUTO_EVAL_US;
}

uint64_t cpu_clock_auto_now_us(void) {
	return (uint64_t)sceKernelGetSystemTimeWide();
}

static void cpu_clock_auto_evaluate(void);

static void cpu_clock_make_speed(int cpu, struct speed_setting_struct *speed) {
	if (cpu >= 333) {
		speed->cpu = 333;
		speed->ram = 333;
		speed->bus = 166;
	}
	else if (cpu >= 266) {
		speed->cpu = 266;
		speed->ram = 266;
		speed->bus = 133;
	}
	else if (cpu >= 222) {
		speed->cpu = 222;
		speed->ram = 222;
		speed->bus = 111;
	}
	else if (cpu >= 166) {
		speed->cpu = 166;
		speed->ram = 222;
		speed->bus = 83;
	}
	else if (cpu >= 133) {
		speed->cpu = 133;
		speed->ram = 222;
		speed->bus = 66;
	}
	else if (cpu >= 111) {
		speed->cpu = 111;
		speed->ram = 222;
		speed->bus = 55;
	}
	else {
		/* 66 MHz is the tested floor for this application. */
		speed->cpu = 66;
		speed->ram = 222;
		speed->bus = 33;
	}
}

static int cpu_clock_auto_window_is_safe_at_current(unsigned int budget_us,
                                                    unsigned int avg_abs_ms) {
	int current_cpu;
	unsigned int max_abs_limit_ms;
	unsigned int avg_abs_limit_ms;
	unsigned int decode_limit_pct;
	unsigned int gu_limit_pct;

	if (auto_state.current_index < CPU_CLOCK_AUTO_MIN_INDEX)
		current_cpu = auto_clock_steps[CPU_CLOCK_AUTO_MIN_INDEX];
	else if (auto_state.current_index > CPU_CLOCK_AUTO_MAX_INDEX)
		current_cpu = auto_clock_steps[CPU_CLOCK_AUTO_MAX_INDEX];
	else
		current_cpu = auto_clock_steps[auto_state.current_index];

	/*
	 * Stay-safety thresholds.
	 *
	 * These are slightly more permissive than downshift thresholds. The goal
	 * is not to bounce clocks on one harmless transient, but to recover if
	 * the current clock is no longer keeping A/V sync and workload budget
	 * healthy over the polling window.
	 *
	 * Low tiers request a half-rate bus.  Bench telemetry compares the request
	 * with the value reported by the active CFW instead of assuming it stuck.
	 */
	if (current_cpu <= 66) {
		max_abs_limit_ms = 105;
		avg_abs_limit_ms = 88;
		decode_limit_pct = 78;
		gu_limit_pct = 90;
	}
	else if (current_cpu <= 111) {
		max_abs_limit_ms = 100;
		avg_abs_limit_ms = 85;
		decode_limit_pct = 75;
		gu_limit_pct = 90;
	}
	else if (current_cpu <= 133) {
		max_abs_limit_ms = 100;
		avg_abs_limit_ms = 85;
		decode_limit_pct = 75;
		gu_limit_pct = 90;
	}
	else if (current_cpu <= 166) {
		max_abs_limit_ms = 100;
		avg_abs_limit_ms = 85;
		decode_limit_pct = 80;
		gu_limit_pct = 90;
	}
	else {
		max_abs_limit_ms = 105;
		avg_abs_limit_ms = 90;
		decode_limit_pct = 85;
		gu_limit_pct = 95;
	}

	if (auto_state.producer_overflow_count != 0U) {
		return 0;
	}

	/*
	 * If the window has no displayed frames but decode/GU activity happened,
	 * treat that as suspicious after warmup/settle. It may mean the current
	 * clock is failing to present frames even before the skip counter catches
	 * up.
	 */
	if (auto_state.displayed_count == 0) {
		if (auto_state.decode_count > 0 || auto_state.gu_count > 0 ||
		    auto_state.postprocess_count > 0) {
			return 0;
		}

		return 1;
	}

	/*
	 * A small displayed sample is not enough to prove trouble. Do not upshift
	 * just because the window is short.
	 */
	if (auto_state.displayed_count <
	    cpu_clock_auto_min_displayed_samples(budget_us))
		return 1;

	/* A/V delta alone can look healthy while every frame is late. Check the
	 * rolling presentation cadence as a separate FPS/frametime guard. */
	if (auto_state.display_interval_count >= 8U) {
		unsigned int interval_avg = cpu_clock_auto_display_interval_avg_us();
		if (interval_avg > (budget_us * 135U) / 100U ||
		    auto_state.display_interval_max_us > (budget_us * 220U) / 100U) {
			return 0;
		}
	}

	if (auto_state.displayed_abs_max_ms > max_abs_limit_ms) {
		return 0;
	}

	if (avg_abs_ms > avg_abs_limit_ms) {
		return 0;
	}

	if (auto_state.decode_count > 0 &&
	    auto_state.decode_max_us > (budget_us * decode_limit_pct) / 100U) {
		return 0;
	}

	if ((auto_state.gu_count > 0 || auto_state.postprocess_count > 0) &&
	    (cpu_clock_auto_gu_avg_us() +
	     (auto_state.postprocess_count == 0 ? 0U :
	      auto_state.postprocess_total_us / auto_state.postprocess_count)) >
	        (budget_us * gu_limit_pct) / 100U) {
		return 0;
	}

	return 1;
}

static void cpu_clock_auto_account_time(uint64_t now) {
	uint64_t elapsed;

	if (auto_state.last_change_us == 0)
		return;

	if (now < auto_state.last_change_us)
		return;

	elapsed = now - auto_state.last_change_us;

	{
		int actual_cpu = scePowerGetCpuClockFrequencyInt();
		int actual_bus = scePowerGetBusClockFrequencyInt();

		if (actual_cpu >= 333)
			auto_state.clock_us_333 += elapsed;
		else if (actual_cpu >= 266)
			auto_state.clock_us_266 += elapsed;
		else if (actual_cpu >= 222)
			auto_state.clock_us_222 += elapsed;
		else if (actual_cpu >= 166)
			auto_state.clock_us_166 += elapsed;
		else if (actual_cpu >= 133)
			auto_state.clock_us_133 += elapsed;
		else if (actual_cpu >= 111)
			auto_state.clock_us_111 += elapsed;
		else
			auto_state.clock_us_66 += elapsed;

		if (actual_bus >= 166)
			auto_state.bus_us_166 += elapsed;
		else if (actual_bus >= 133)
			auto_state.bus_us_133 += elapsed;
		else if (actual_bus >= 111)
			auto_state.bus_us_111 += elapsed;
		else if (actual_bus >= 83)
			auto_state.bus_us_83 += elapsed;
		else if (actual_bus >= 66)
			auto_state.bus_us_66 += elapsed;
		else if (actual_bus >= 55)
			auto_state.bus_us_55 += elapsed;
		else
			auto_state.bus_us_33 += elapsed;
	}

	auto_state.last_change_us = now;
}

static void cpu_clock_auto_note_cpu_bounds(void) {
	if (auto_state.min_cpu == 0 || current_speed.cpu < auto_state.min_cpu)
		auto_state.min_cpu = current_speed.cpu;

	if (current_speed.cpu > auto_state.max_cpu)
		auto_state.max_cpu = current_speed.cpu;
}

static void cpu_clock_apply_speed(const struct speed_setting_struct *speed) {
	int r0;
	int r1;
	int r2;

	if (speed == 0)
		return;
	if (current_speed.cpu == speed->cpu &&
	    current_speed.ram == speed->ram &&
	    current_speed.bus == speed->bus)
		return;

	current_speed = *speed;

	r0 = 0;
	r1 = 0;
	r2 = 0;

	if (current_speed.cpu > 222) {
		/*
		 * API order:
		 *   scePowerSetClockFrequency(pll, cpu, bus)
		 *
		 * In this codebase current_speed.ram is being used as the PLL/base
		 * clock field.
		 */
		r0 = scePowerSetClockFrequency(current_speed.ram,
		                                current_speed.cpu,
		                                current_speed.bus);
	}
	else {
		/*
		 * For sub-222 MHz playback, first establish a safe known
		 * 222/222/111 base, then lower CPU and bus individually.
		 *
		 * The subsequent bench log compares requested and reported values; CFWs
		 * are permitted to quantize unsupported clock combinations.
		 */
		r0 = scePowerSetClockFrequency(222, 222, 111);
		r1 = scePowerSetCpuClockFrequency(current_speed.cpu);
		r2 = scePowerSetBusClockFrequency(current_speed.bus);
	}

	(void)r0;
	(void)r1;
	(void)r2;
}

static void cpu_clock_apply_cpu(int cpu) {
	struct speed_setting_struct speed;

	cpu_clock_make_speed(cpu, &speed);
	cpu_clock_apply_speed(&speed);
}

static int cpu_clock_auto_active(void) {
	return auto_clock_enabled && playback_clock_lock;
}

static unsigned int cpu_clock_auto_avg_abs_ms(void) {
	if (auto_state.displayed_count == 0)
		return 0;

	return auto_state.displayed_abs_total_ms / auto_state.displayed_count;
}

static unsigned int cpu_clock_auto_display_interval_avg_us(void)
{
	return auto_state.display_interval_count == 0 ? 0U :
		auto_state.display_interval_total_us / auto_state.display_interval_count;
}

static unsigned int cpu_clock_auto_display_fps_x1000(void)
{
	if (auto_state.display_interval_count == 0 ||
	    auto_state.display_interval_total_us == 0)
		return 0U;
	return (unsigned int)(((uint64_t)auto_state.display_interval_count *
	                       1000000000ULL) /
	                      auto_state.display_interval_total_us);
}

static unsigned int cpu_clock_auto_lifetime_display_fps_x1000(void)
{
	if (auto_state.lifetime_display_interval_count == 0 ||
	    auto_state.lifetime_display_interval_total_us == 0)
		return 0U;

	/* Discontinuities longer than 500 ms are excluded when samples are added,
	 * so pause/seek time cannot masquerade as poor presentation throughput. */
	return (unsigned int)(((uint64_t)auto_state.lifetime_display_interval_count *
	                       1000000000ULL) /
	                      auto_state.lifetime_display_interval_total_us);
}

static void cpu_clock_auto_clear_window(void) {
	/* Producer samples committed after the evaluation snapshot belong to the
	 * next window.  Do not drain them here: doing so would silently discard work
	 * whenever a producer pre-empts the presentation owner during evaluation. */
	auto_state.window_started_us = cpu_clock_auto_now_us();
	auto_state.audio_only_blocks = auto_state.audio_only_max_gap_us = 0U;
	auto_state.audio_only_empty_count = auto_state.audio_only_codec_max_us = 0U;
	auto_state.displayed_count = 0;
	auto_state.skipped_count = 0;

	auto_state.displayed_abs_total_ms = 0;
	auto_state.displayed_abs_max_ms = 0;
	auto_state.display_interval_count = 0;
	auto_state.display_interval_total_us = 0;
	auto_state.display_interval_max_us = 0;

	auto_state.decode_count = 0;
	auto_state.decode_total_us = 0;
	auto_state.decode_max_us = 0;

	auto_state.gu_count = 0;
	auto_state.gu_total_us = 0;
	auto_state.gu_max_us = 0;
	auto_state.postprocess_count = 0;
	auto_state.postprocess_total_us = 0;
	auto_state.postprocess_max_us = 0;
	auto_state.audio_resample_count = 0;
	auto_state.audio_resample_total_us = 0;
	auto_state.audio_resample_max_us = 0;
	auto_state.producer_overflow_count = 0;
}

static void cpu_clock_auto_set_index(int index, const char *reason) {
	uint64_t now;
	int old_index;

	if (index < CPU_CLOCK_AUTO_MIN_INDEX)
		index = CPU_CLOCK_AUTO_MIN_INDEX;

	if (index > CPU_CLOCK_AUTO_MAX_INDEX)
		index = CPU_CLOCK_AUTO_MAX_INDEX;

	if (auto_state.current_index == index)
		return;

	now = cpu_clock_auto_now_us();
	cpu_clock_auto_account_time(now);

	old_index = auto_state.current_index;
	auto_state.current_index = index;

	cpu_clock_apply_cpu(auto_clock_steps[auto_state.current_index]);

	auto_state.change_count++;

	if (index < old_index)
		auto_state.downshift_count++;
	else
		auto_state.upshift_count++;

	cpu_clock_auto_note_cpu_bounds();
}

static unsigned int cpu_clock_auto_required_stable_windows(int target_index) {
	int target_cpu;

	if (extreme_battery_saver)
		return 1U;

	if (target_index < CPU_CLOCK_AUTO_MIN_INDEX)
		target_index = CPU_CLOCK_AUTO_MIN_INDEX;
	if (target_index > CPU_CLOCK_AUTO_MAX_INDEX)
		target_index = CPU_CLOCK_AUTO_MAX_INDEX;

	target_cpu = auto_clock_steps[target_index];
	return target_cpu <= 66 ? 2U : 1U;
}

static unsigned int cpu_clock_auto_decode_avg_us(void) {
	return auto_state.decode_count == 0 ? 0U :
		auto_state.decode_total_us / auto_state.decode_count;
}

static unsigned int cpu_clock_auto_gu_avg_us(void) {
	return auto_state.gu_count == 0 ? 0U :
		auto_state.gu_total_us / auto_state.gu_count;
}

static unsigned int cpu_clock_auto_audio_per_display_avg_us(void) {
	if (auto_state.audio_resample_count == 0U)
		return 0U;
	/* Audio block cadence differs from video cadence. Charge the measured
	 * producer duty across displayed frames rather than adding one whole audio
	 * block to every video decode sample. */
	if (auto_state.displayed_count != 0U)
		return auto_state.audio_resample_total_us /
		       auto_state.displayed_count;
	return auto_state.audio_resample_total_us /
	       auto_state.audio_resample_count;
}

/* A native/light stream should not spend half of a short movie walking through
 * every intermediate clock.  This is deliberately stricter than the ordinary
 * one-step downshift test: it is used only to skip tiers after one clean,
 * representative window and leaves the existing danger path in charge of an
 * immediate recovery if the workload changes. */
static int cpu_clock_auto_fast_descent_allowed(unsigned int budget_us,
                                               unsigned int avg_abs_ms)
{
	unsigned int interval_avg;
	unsigned int output_avg;

#if !PPA_CPU_GOVERNOR_FAST_DESCENT
	(void)budget_us;
	(void)avg_abs_ms;
	return 0;
#endif
	if (auto_state.producer_overflow_count != 0U)
		return 0;
	if (auto_clock_steps[auto_state.current_index] < 222)
		/* Below 222 MHz, use normal calibration. */
		return 0;
	if (auto_state.skipped_count != 0 || auto_state.postprocess_count != 0)
		return 0;
	if (auto_state.displayed_count <
	    cpu_clock_auto_min_displayed_samples(budget_us) ||
	    auto_state.decode_count == 0)
		return 0;
	if (ppa_bus_cpu_pressure_hint_q8() >= 96U)
		return 0;
	if (avg_abs_ms > 45U || auto_state.displayed_abs_max_ms > 65U)
		return 0;
	if (auto_state.display_interval_count < 6U)
		return 0;
	interval_avg = cpu_clock_auto_display_interval_avg_us();
	if (interval_avg > (budget_us * 108U) / 100U ||
	    auto_state.display_interval_max_us > (budget_us * 150U) / 100U)
		return 0;
	if (auto_state.decode_max_us > (budget_us * 55U) / 100U)
		return 0;
	output_avg = cpu_clock_auto_gu_avg_us();
	if (output_avg > (budget_us * 45U) / 100U)
		return 0;
	return 1;
}

static int cpu_clock_auto_fast_candidate_has_headroom(int target_index,
                                                      unsigned int budget_us)
{
	unsigned int current_cpu;
	unsigned int target_cpu;
	unsigned int decode_avg;
	unsigned int gu_avg;
	unsigned int projected_decode_avg;
	unsigned int projected_decode_max;
	unsigned int projected_gu_avg;
	unsigned int projected_gu_max;
	unsigned int audio_avg;
	unsigned int projected_audio_avg;
	unsigned int projected_audio_max;

	if (target_index < CPU_CLOCK_AUTO_MIN_INDEX ||
	    target_index >= auto_state.current_index)
		return 0;
	current_cpu = (unsigned int)auto_clock_steps[auto_state.current_index];
	target_cpu = (unsigned int)auto_clock_steps[target_index];
	if (target_cpu == 0U)
		return 0;
	decode_avg = cpu_clock_auto_decode_avg_us();
	gu_avg = cpu_clock_auto_gu_avg_us();
	audio_avg = cpu_clock_auto_audio_per_display_avg_us();
	projected_decode_avg = (unsigned int)(
		((uint64_t)decode_avg * current_cpu + target_cpu - 1U) /
		target_cpu);
	projected_decode_max = (unsigned int)(
		((uint64_t)auto_state.decode_max_us * current_cpu +
		 target_cpu - 1U) / target_cpu);
	projected_gu_avg = (unsigned int)(
		((uint64_t)gu_avg * current_cpu + target_cpu - 1U) /
		target_cpu);
	projected_gu_max = (unsigned int)(
		((uint64_t)auto_state.gu_max_us * current_cpu +
		 target_cpu - 1U) / target_cpu);
	projected_audio_avg = (unsigned int)(
		((uint64_t)audio_avg * current_cpu + target_cpu - 1U) /
		 target_cpu);
	projected_audio_max = (unsigned int)(
		((uint64_t)auto_state.audio_resample_max_us * current_cpu +
		 target_cpu - 1U) / target_cpu);

	/* Measured resampling duty is charged explicitly. Keep the remaining broad
	 * reserve for demux, interrupts and scheduling costs that are not represented
	 * by the decode/GU/audio timers. */
	return projected_decode_avg <= (budget_us * 40U) / 100U &&
	       projected_audio_avg <= (budget_us * 22U) / 100U &&
	       projected_decode_avg + projected_audio_avg <=
	           (budget_us * 50U) / 100U &&
	       projected_gu_avg <= (budget_us * 55U) / 100U &&
	       projected_decode_max <= (budget_us * 80U) / 100U &&
	       projected_audio_max <= (budget_us * 45U) / 100U &&
	       projected_gu_max <= (budget_us * 90U) / 100U;
}

/* Require a representative fraction of one evaluation window, rather than a
 * fixed 30 frames.  A fixed threshold prevented 23.976/24/25 fps content from
 * ever downshifting and was too weak for 50/60 fps content. */
static unsigned int cpu_clock_auto_min_displayed_samples(unsigned int budget_us) {
	uint64_t now;
	uint64_t elapsed_us;
	unsigned int expected;
	unsigned int required;

	if (budget_us == 0)
		budget_us = 33366U;
	now = cpu_clock_auto_now_us();
	elapsed_us = now >= auto_state.window_started_us ?
		now - auto_state.window_started_us : CPU_CLOCK_AUTO_EVAL_US;
	if (elapsed_us < 250000ULL)
		elapsed_us = 250000ULL;
	if (elapsed_us > 2000000ULL)
		elapsed_us = 2000000ULL;
	expected = (unsigned int)(elapsed_us / budget_us);
	if (expected < 6U)
		expected = 6U;
	if (expected > 120U)
		expected = 120U;
	required = extreme_battery_saver ?
		(expected + 2U) / 3U : (expected * 2U + 2U) / 3U;
	if (required < (extreme_battery_saver ? 4U : 6U))
		required = extreme_battery_saver ? 4U : 6U;
	return required;
}

/* Saver accepts one additional evaluation window of soft pressure. This does
 * not hide the measurement or change synchronization: repeated misses still
 * recover, and callers must bypass the grace period for queue starvation.
 * There is no timer/thread; the existing presentation owner advances it. */
static int cpu_clock_saver_defer_danger(uint64_t now)
{
	if (!extreme_battery_saver)
		return 0;
	if (auto_state.saver_danger_since_us == 0U) {
		auto_state.saver_danger_since_us = now;
		return 1;
	}
	return now >= auto_state.saver_danger_since_us &&
	       now - auto_state.saver_danger_since_us <
	           CPU_CLOCK_SAVER_TRANSIENT_US;
}

static int cpu_clock_auto_window_allows_downshift_to(int target_index,
                                                     unsigned int budget_us,
                                                     unsigned int avg_abs_ms) {
	int target_cpu;
	unsigned int decode_limit_pct;
	unsigned int gu_limit_pct;
	unsigned int max_abs_limit_ms;
	unsigned int avg_abs_limit_ms;
	unsigned int min_displayed;
	unsigned int decode_avg;
	unsigned int gu_avg;
	unsigned int projected_decode_avg;
	unsigned int projected_decode_max;
	unsigned int projected_gu_avg;
	unsigned int projected_gu_max;
	unsigned int postprocess_avg;
	unsigned int projected_postprocess_avg;
	unsigned int projected_postprocess_max;
	unsigned int projected_output_avg;
	unsigned int projected_output_peak;
	unsigned int audio_per_display_avg;
	unsigned int projected_audio_avg;
	unsigned int projected_audio_max;
	unsigned int projected_producer_avg;
	int current_cpu;

	if (target_index < CPU_CLOCK_AUTO_MIN_INDEX)
		target_index = CPU_CLOCK_AUTO_MIN_INDEX;

	if (target_index > CPU_CLOCK_AUTO_MAX_INDEX)
		target_index = CPU_CLOCK_AUTO_MAX_INDEX;

	target_cpu = auto_clock_steps[target_index];
	current_cpu = auto_clock_steps[auto_state.current_index];
	min_displayed = cpu_clock_auto_min_displayed_samples(budget_us);
	decode_avg = cpu_clock_auto_decode_avg_us();
	gu_avg = cpu_clock_auto_gu_avg_us();
	postprocess_avg = auto_state.postprocess_count == 0 ? 0U :
		auto_state.postprocess_total_us / auto_state.postprocess_count;
	audio_per_display_avg = cpu_clock_auto_audio_per_display_avg_us();
	projected_decode_avg = (unsigned int)(
		((uint64_t)decode_avg * (unsigned int)current_cpu +
		 (unsigned int)target_cpu - 1U) / (unsigned int)target_cpu);
	projected_decode_max = (unsigned int)(
		((uint64_t)auto_state.decode_max_us * (unsigned int)current_cpu +
		 (unsigned int)target_cpu - 1U) / (unsigned int)target_cpu);
	projected_gu_avg = (unsigned int)(
		((uint64_t)gu_avg * (unsigned int)current_cpu +
		 (unsigned int)target_cpu - 1U) / (unsigned int)target_cpu);
	projected_gu_max = (unsigned int)(
		((uint64_t)auto_state.gu_max_us * (unsigned int)current_cpu +
		 (unsigned int)target_cpu - 1U) / (unsigned int)target_cpu);
	projected_postprocess_avg = (unsigned int)(
		((uint64_t)postprocess_avg * (unsigned int)current_cpu +
		 (unsigned int)target_cpu - 1U) / (unsigned int)target_cpu);
	projected_postprocess_max = (unsigned int)(
		((uint64_t)auto_state.postprocess_max_us * (unsigned int)current_cpu +
		 (unsigned int)target_cpu - 1U) / (unsigned int)target_cpu);
	projected_audio_avg = (unsigned int)(
		((uint64_t)audio_per_display_avg * (unsigned int)current_cpu +
		 (unsigned int)target_cpu - 1U) / (unsigned int)target_cpu);
	projected_audio_max = (unsigned int)(
		((uint64_t)auto_state.audio_resample_max_us *
		 (unsigned int)current_cpu + (unsigned int)target_cpu - 1U) /
		 (unsigned int)target_cpu);
	projected_producer_avg = projected_decode_avg + projected_audio_avg;
	projected_output_avg = projected_gu_avg + projected_postprocess_avg;
	/* GU and post-processing are sequential but their independent maxima may
	 * occur on different frames. Pair each peak with the other stage's average
	 * instead of summing two unrelated worst cases. */
	projected_output_peak = projected_gu_max + projected_postprocess_avg;
	if (projected_postprocess_max + projected_gu_avg > projected_output_peak)
		projected_output_peak = projected_postprocess_max + projected_gu_avg;

	/*
	 * Aggressive lower-clock exploration:
	 *
	 * These limits are intentionally permissive so the production governor can
	 * to allow the governor to reach the experimental low CPU tiers and let
	 * the real playback metrics decide whether they are usable.
	 *
	 * Each low tier requests a matching half-rate bus.  Actual CPU/bus values
	 * are measured in bench builds so a CFW clamp cannot make results deceptive.
	 */
	if (target_cpu <= 66) {
		decode_limit_pct = 55;
		gu_limit_pct = 70;
		max_abs_limit_ms = 100;
		avg_abs_limit_ms = 85;
	}
	else if (target_cpu <= 111) {
		decode_limit_pct = 45;
		gu_limit_pct = 65;
		max_abs_limit_ms = 95;
		avg_abs_limit_ms = 80;
	}
	else if (target_cpu <= 133) {
		decode_limit_pct = 45;
		gu_limit_pct = 65;
		max_abs_limit_ms = 95;
		avg_abs_limit_ms = 80;
	}
	else if (target_cpu <= 166) {
		decode_limit_pct = 50;
		gu_limit_pct = 70;
		max_abs_limit_ms = 95;
		avg_abs_limit_ms = 80;
	}
	else if (target_cpu <= 222) {
		decode_limit_pct = 55;
		gu_limit_pct = 75;
		max_abs_limit_ms = 95;
		avg_abs_limit_ms = 80;
	}
	else {
		decode_limit_pct = 60;
		gu_limit_pct = 80;
		max_abs_limit_ms = 100;
		avg_abs_limit_ms = 85;
	}

	if (auto_state.producer_overflow_count != 0U) {
		return 0;
	}

	/* This hint is sampled postprocess/cache work plus cache traffic and late
	 * frames, not a hardware-utilization percentage or an I/O-wait timer. */
	if (ppa_bus_cpu_pressure_hint_q8() >= 176U) {
		return 0;
	}

	if (auto_state.displayed_count < min_displayed) {
		return 0;
	}

	if (auto_state.decode_count == 0) {
		return 0;
	}

	/* Valid direct-display paths may not report GU work every window. Zero GU
	 * samples mean zero projected optional cost, not a permanent downshift veto. */
	if (auto_state.display_interval_count >= 4U) {
		unsigned int interval_avg = cpu_clock_auto_display_interval_avg_us();
		unsigned int fps_x1000 = cpu_clock_auto_display_fps_x1000();
		unsigned int target_fps_x1000 = budget_us == 0 ? 0U :
			(unsigned int)(1000000000ULL / budget_us);
		if (interval_avg > (budget_us * 118U) / 100U ||
		    auto_state.display_interval_max_us > (budget_us * 185U) / 100U ||
		    (target_fps_x1000 != 0U &&
		     fps_x1000 * 100U < target_fps_x1000 * 90U)) {
			return 0;
		}
	}

	if (auto_state.displayed_abs_max_ms > max_abs_limit_ms) {
		return 0;
	}

	if (avg_abs_ms > avg_abs_limit_ms) {
		return 0;
	}

	if (projected_producer_avg >
	    (budget_us * decode_limit_pct) / 100U) {
		return 0;
	}

	if ((auto_state.gu_count > 0 || auto_state.postprocess_count > 0) &&
	    projected_output_avg >
	    (budget_us * gu_limit_pct) / 100U) {
		return 0;
	}

	/* A single operation consuming an entire frame is still unsafe even when
	 * the window average is low; displayed-frame skips remain the hard guard. */
	if (projected_decode_max + projected_audio_avg > budget_us ||
	    projected_audio_max > (budget_us * 75U) / 100U ||
	    ((auto_state.gu_count > 0 || auto_state.postprocess_count > 0) &&
	     projected_output_peak > budget_us)) {
		return 0;
	}

	return 1;
}

void cpu_clock_auto_reset(void) {
	uint64_t now = cpu_clock_auto_now_us();

	cpu_clock_auto_discard_pending_producers();
	memset(&auto_state, 0, sizeof(auto_state));

	auto_state.current_index = CPU_CLOCK_AUTO_MAX_INDEX;
	auto_state.started_us = now;
	auto_state.last_eval_us = now;
	auto_state.last_change_us = now;
	auto_state.boost_until_us = now;
	auto_state.warmup_min_until_us = now +
		(extreme_battery_saver ? 250000ULL : CPU_CLOCK_AUTO_WARMUP_MIN_US);
	auto_state.warmup_deadline_us = now +
		(extreme_battery_saver ? 750000ULL : CPU_CLOCK_AUTO_WARMUP_DEADLINE_US);
	auto_state.warmup_required_presentations =
		extreme_battery_saver ? 6U : 10U;
	auto_state.window_started_us = now;
	auto_state.frame_duration_us = 33366;

	auto_state.start_cpu = 333;
	auto_state.min_cpu = 333;
	auto_state.max_cpu = 333;

	if (cpu_clock_auto_active()) {
		cpu_clock_apply_cpu(auto_clock_steps[auto_state.current_index]);

	}
}

static uint64_t cpu_clock_auto_boost_hold_us(cpu_clock_auto_boost_reason reason)
{
	uint64_t normal_us;
	switch (reason) {
	case CPU_CLOCK_BOOST_SUBTITLE:
	case CPU_CLOCK_BOOST_VISUAL_CHANGE:
		normal_us = 500000ULL;
		break;
	case CPU_CLOCK_BOOST_RESUME:
	case CPU_CLOCK_BOOST_SEEK:
	case CPU_CLOCK_BOOST_AUDIO_RECONFIG:
	case CPU_CLOCK_BOOST_OUTPUT_MODE:
		normal_us = 1500000ULL;
		break;
	case CPU_CLOCK_BOOST_TRICKPLAY_LEAVE:
		normal_us = 750000ULL;
		break;
	case CPU_CLOCK_BOOST_OPEN:
	case CPU_CLOCK_BOOST_RESET:
	case CPU_CLOCK_BOOST_TRICKPLAY_ENTER:
	case CPU_CLOCK_BOOST_GENERIC:
	default:
		normal_us = 2500000ULL;
		break;
	}
	return extreme_battery_saver ? (normal_us + 1ULL) / 2ULL : normal_us;
}

void cpu_clock_auto_boost_for_reason(cpu_clock_auto_boost_reason reason) {
	uint64_t now;
	uint64_t new_until;

	if (!cpu_clock_auto_active())
		return;
	/* A subtitle/color option must not pre-emptively pin saver playback to
	 * 333 MHz. Measure its actual cost; mandatory seek/reset/output events
	 * retain their established immediate recovery path. */
	if (extreme_battery_saver &&
	    (reason == CPU_CLOCK_BOOST_SUBTITLE ||
	     reason == CPU_CLOCK_BOOST_VISUAL_CHANGE))
		return;

	now = cpu_clock_auto_now_us();
	new_until = now + cpu_clock_auto_boost_hold_us(reason);

	/* Callers publish typed, edge-triggered events.  Even at 333 MHz the hold is
	 * refreshed so a seek or decoder reset cannot inherit an expiring old hold. */
	auto_state.boost_count++;
	if (new_until > auto_state.boost_until_us)
		auto_state.boost_until_us = new_until;

	auto_state.last_displayed_us = 0;
	auto_state.audio_only_last_complete_us = 0;
	auto_state.saver_danger_since_us = 0;
	auto_state.saver_last_skip_us = 0;
	auto_state.stable_windows = 0;
	/* Work committed before this explicit workload transition belongs to the old
	 * regime.  Retain lifetime counts but exclude it from the new calibration. */
	cpu_clock_auto_collect_producers(0);
	if (auto_state.current_index != CPU_CLOCK_AUTO_MAX_INDEX)
		cpu_clock_auto_set_index(CPU_CLOCK_AUTO_MAX_INDEX, "typed_boost");
	cpu_clock_auto_clear_window();
}

void cpu_clock_auto_boost(void) {
	cpu_clock_auto_boost_for_reason(CPU_CLOCK_BOOST_GENERIC);
}

void cpu_clock_auto_pause(void)
{
	auto_state.audio_only_last_complete_us = 0;
	auto_state.saver_danger_since_us = 0;
	auto_state.saver_last_skip_us = 0;
	auto_state.audio_only_blocks = auto_state.audio_only_max_gap_us = 0U;
	if (!cpu_clock_auto_active())
		return;

	/* Playback workers park on kernel waits while paused. Holding the last
	 * decode clock wastes battery with no cadence benefit, so move directly to
	 * the floor; resume/seek events restore 333 MHz before work restarts. */
	cpu_clock_auto_collect_producers(0);
	cpu_clock_auto_set_index(CPU_CLOCK_AUTO_MIN_INDEX, "pause");
	auto_state.last_displayed_us = 0;
	auto_state.stable_windows = 0;
	cpu_clock_auto_clear_window();
}

static int cpu_clock_auto_danger_target_index(void) {
	int current_cpu;
	int target_index;

	current_cpu = auto_clock_steps[auto_state.current_index];
	target_index = auto_state.current_index + 1;

	/*
	 * Recover faster from very low clocks. A single danger condition at
	 * 111/133 probably means the clock is too low for the current section,
	 * so jump back to 222 rather than creeping one step at a time.
	 *
	 * Actual skipped frames still force max clock in cpu_clock_auto_evaluate().
	 */
	if (current_cpu <= 133) {
		int i;

		for (i = auto_state.current_index + 1; i <= CPU_CLOCK_AUTO_MAX_INDEX; i++) {
			if (auto_clock_steps[i] >= 222) {
				target_index = i;
				break;
			}
		}
	}

	if (target_index > CPU_CLOCK_AUTO_MAX_INDEX)
		target_index = CPU_CLOCK_AUTO_MAX_INDEX;

	return target_index;
}

static void cpu_clock_auto_evaluate(void) {
	uint64_t now;
	unsigned int budget_us;
	unsigned int avg_abs_ms;
	unsigned int required_stable_windows;
	int fast_target_index;
	int target_index;
	int danger;

	if (!cpu_clock_auto_active())
		return;

	now = cpu_clock_auto_now_us();

	if (now - auto_state.last_eval_us < cpu_clock_auto_eval_interval_us())
		return;

	/* Presentation is the sole governor owner.  Producer observations are
	 * snapshotted only when an evaluation is actually due, avoiding a polling
	 * thread and avoiding per-frame cross-thread clearing. */
	cpu_clock_auto_collect_producers(1);
	auto_state.eval_count++;

	budget_us = auto_state.frame_duration_us;
	if (budget_us == 0)
		budget_us = 33366;

	avg_abs_ms = cpu_clock_auto_avg_abs_ms();

	/* Startup warmup is work-based rather than a fixed high-clock sleep.  Once
	 * enough successful presentations and decode work exist, descent can begin
	 * after 0.5 s; the deadline prevents a sparse stream from waiting forever. */
	if (!auto_state.warmup_complete) {
		int representative =
			now >= auto_state.warmup_min_until_us &&
			auto_state.lifetime_displayed_count >=
				auto_state.warmup_required_presentations &&
			auto_state.lifetime_decode_count != 0U;
		if (!representative && now < auto_state.warmup_deadline_us) {
			auto_state.warmup_block_count++;
			auto_state.stable_windows = 0;
			cpu_clock_auto_clear_window();
			auto_state.last_eval_us = now;
			return;
		}
		auto_state.warmup_complete = 1U;
	}

	/* Typed recovery events retain a bounded settle hold without forcing the
	 * governor itself to wake on a timer. */
	if (now < auto_state.boost_until_us) {
		auto_state.warmup_block_count++;
		auto_state.stable_windows = 0;
		cpu_clock_auto_clear_window();
		auto_state.last_eval_us = now;
		return;
	}

	danger = 0;

	/*
	 * Skipped frames are a hard failure. Keep this separate from softer
	 * sync/budget danger so it cannot be accidentally downgraded from
	 * danger=2 to danger=1 by a later condition.
	 */
	if (auto_state.skipped_count > 0) {
		danger = 2;
	}
	else if (ppa_bus_cpu_pressure_hint_q8() >= 232U) {
		danger = 1;
	}
	else if (!cpu_clock_auto_window_is_safe_at_current(budget_us,
	                                                   avg_abs_ms)) {
		danger = 1;
	}

	if (danger != 0) {
		auto_state.danger_count++;
		if (now >= auto_state.optional_work_block_until_us)
			auto_state.optional_work_shed_count++;
		if (now + 2000000ULL > auto_state.optional_work_block_until_us)
			auto_state.optional_work_block_until_us = now + 2000000ULL;

		/* One late frame or one high decode sample is not sustained demand.
		 * Two skips in the same window or lost producer samples are hard
		 * evidence and bypass this saver-only grace period. */
		if (extreme_battery_saver &&
		    auto_state.skipped_count < 2U &&
		    auto_state.producer_overflow_count == 0U &&
		    cpu_clock_saver_defer_danger(now)) {
			auto_state.stable_windows = 0;
			cpu_clock_auto_clear_window();
			auto_state.last_eval_us = now;
			return;
		}
		auto_state.saver_danger_since_us = 0;

		if (danger >= 2) {
			cpu_clock_auto_set_index(CPU_CLOCK_AUTO_MAX_INDEX,
			                         "danger_skip");
		}
		else if (auto_state.current_index < CPU_CLOCK_AUTO_MAX_INDEX) {
			cpu_clock_auto_set_index(cpu_clock_auto_danger_target_index(),
			                         "danger");
		}

		auto_state.boost_until_us = now + (extreme_battery_saver ?
			CPU_CLOCK_SAVER_RECOVERY_HOLD_US : CPU_CLOCK_AUTO_DANGER_HOLD_US);
		auto_state.stable_windows = 0;
		cpu_clock_auto_clear_window();
		auto_state.last_eval_us = now;
		return;
	}
	auto_state.saver_danger_since_us = 0;

	if (auto_state.current_index > CPU_CLOCK_AUTO_MIN_INDEX) {
		/* On a clean native/no-postprocess stream, use the current high-clock
		 * measurements to prove a deeper tier has ample headroom. This avoids
		 * burning several seconds at 333/266/222 merely to visit each step. */
		fast_target_index = auto_state.current_index;
		if (cpu_clock_auto_fast_descent_allowed(budget_us, avg_abs_ms)) {
			int candidate;
			for (candidate = CPU_CLOCK_AUTO_MIN_INDEX;
			     candidate < auto_state.current_index - 1;
			     ++candidate) {
				if (!cpu_clock_auto_fast_candidate_has_headroom(candidate,
				                                                  budget_us))
					continue;
				if (cpu_clock_auto_window_allows_downshift_to(candidate,
				                                               budget_us,
				                                               avg_abs_ms)) {
					fast_target_index = candidate;
					break;
				}
			}
		}
		if (fast_target_index < auto_state.current_index - 1) {
			cpu_clock_auto_set_index(fast_target_index,
			                         "fast_downshift");
			auto_state.boost_until_us = now +
				(extreme_battery_saver ? 250000ULL : 750000ULL);
			auto_state.stable_windows = 0;
			cpu_clock_auto_clear_window();
			auto_state.last_eval_us = now;
			return;
		}

		target_index = auto_state.current_index - 1;

		if (cpu_clock_auto_window_allows_downshift_to(target_index,
		                                               budget_us,
		                                               avg_abs_ms)) {
			auto_state.stable_windows++;
		}
		else {
			auto_state.unstable_block_count++;
			auto_state.stable_windows = 0;
		}

		required_stable_windows =
			cpu_clock_auto_required_stable_windows(target_index);

		if (auto_state.stable_windows >= required_stable_windows) {
			cpu_clock_auto_set_index(target_index,
			                         "downshift");
			auto_state.boost_until_us = now + (extreme_battery_saver ? 250000ULL : CPU_CLOCK_AUTO_SETTLE_US);
			auto_state.stable_windows = 0;
		}
	}
	else {
		/*
		 * Already at the minimum step. Stay there unless a future danger
		 * condition triggers an upshift.
		 */
		if (auto_state.displayed_count <
		        cpu_clock_auto_min_displayed_samples(budget_us) ||
		    auto_state.decode_count == 0) {
			auto_state.unstable_block_count++;
		}

		auto_state.stable_windows = 0;
	}

	cpu_clock_auto_clear_window();
	auto_state.last_eval_us = now;
}

void cpu_clock_auto_on_present_complete(int audio_timestamp_ms,
                                        int video_timestamp_ms,
                                        int video_frame_duration_ms) {
	int64_t delta;
	unsigned int abs_delta;
	uint64_t now;

	if (video_frame_duration_ms > 0)
		auto_state.frame_duration_us = (unsigned int)video_frame_duration_ms * 1000U;

	if (!cpu_clock_auto_active())
		return;

	now = cpu_clock_auto_now_us();
	/* Malformed or very distant timestamps must not overflow a signed int
	 * before the governor sees the lag. Each input is int-sized, so the
	 * magnitude of the widened difference always fits unsigned int. */
	delta = (int64_t)audio_timestamp_ms - (int64_t)video_timestamp_ms;
	abs_delta = (unsigned int)(delta < 0 ? -delta : delta);

	if (auto_state.last_displayed_us != 0 &&
	    now >= auto_state.last_displayed_us &&
	    now - auto_state.last_displayed_us <= 500000ULL) {
		unsigned int interval =
			(unsigned int)(now - auto_state.last_displayed_us);
		auto_state.display_interval_count++;
		auto_state.display_interval_total_us += interval;
		if (interval > auto_state.display_interval_max_us)
			auto_state.display_interval_max_us = interval;

		auto_state.lifetime_display_interval_count++;
		auto_state.lifetime_display_interval_total_us += interval;
		if (interval > auto_state.lifetime_display_interval_max_us)
			auto_state.lifetime_display_interval_max_us = interval;
	}
	auto_state.last_displayed_us = now;

	auto_state.displayed_count++;
	auto_state.lifetime_displayed_count++;

	if (abs_delta > 0xffffffffU - auto_state.displayed_abs_total_ms)
		auto_state.displayed_abs_total_ms = 0xffffffffU;
	else
		auto_state.displayed_abs_total_ms += abs_delta;

	if (abs_delta > auto_state.displayed_abs_max_ms)
		auto_state.displayed_abs_max_ms = abs_delta;

	cpu_clock_auto_evaluate();
}

void cpu_clock_auto_on_displayed_frame(int audio_timestamp_ms,
                                       int video_timestamp_ms,
                                       int video_frame_duration_ms)
{
	cpu_clock_auto_on_present_complete(audio_timestamp_ms,
	                                   video_timestamp_ms,
	                                   video_frame_duration_ms);
}

void cpu_clock_auto_on_audio_only_produced_us(unsigned int total_us,
                                             unsigned int codec_us,
                                             int block_duration_ms)
{
    unsigned int write_index, read_index;
    ppa_session_note_decode(total_us, block_duration_ms);
    if (!cpu_clock_auto_active() || !ppa_session_audio_only()) return;
    if (codec_us > total_us) codec_us = total_us;
    write_index = g_audio_only_producer.write_index;
    read_index = __sync_fetch_and_add(&g_audio_only_producer.read_index, 0U);
    if (write_index - read_index >= CPU_CLOCK_PRODUCER_QUEUE_CAPACITY) {
        __sync_fetch_and_add(&g_audio_only_producer.overflow_count, 1U);
        return;
    }
    g_audio_only_producer.samples[write_index & CPU_CLOCK_PRODUCER_QUEUE_MASK].total_us = total_us;
    g_audio_only_producer.samples[write_index & CPU_CLOCK_PRODUCER_QUEUE_MASK].codec_us = codec_us;
    __sync_synchronize();
    g_audio_only_producer.write_index = write_index + 1U;
}

void cpu_clock_auto_on_audio_only_complete(int block_duration_ms)
{
    uint64_t now;
    unsigned int budget, current_cpu, codec_peak, service_avg, overflow;
    int target, hard_pressure, soft_pressure;
    if (!cpu_clock_auto_active() || !ppa_session_audio_only()) return;
    now = cpu_clock_auto_now_us();
    if (__sync_lock_test_and_set(&g_audio_only_producer.discard_pending, 0U)) {
        overflow = cpu_clock_auto_drain_audio_only(0);
        auto_state.lifetime_producer_overflow_count += overflow;
    }
    budget = (unsigned int)(block_duration_ms > 0 ? block_duration_ms : 23) * 1000U;
    auto_state.frame_duration_us = budget;
    if (auto_state.audio_only_last_complete_us && now >= auto_state.audio_only_last_complete_us) {
        uint64_t gap = now - auto_state.audio_only_last_complete_us;
        if (gap > 0xffffffffULL) gap = 0xffffffffULL;
        if (gap > auto_state.audio_only_max_gap_us)
            auto_state.audio_only_max_gap_us = (unsigned int)gap;
    }
    auto_state.audio_only_last_complete_us = now;
    auto_state.audio_only_blocks++;
    auto_state.lifetime_audio_only_blocks++;
    if (!ppa_session_audio_only_queued_blocks()) auto_state.audio_only_empty_count++;
    if (now - auto_state.last_eval_us < cpu_clock_auto_eval_interval_us()) return;
    overflow = cpu_clock_auto_drain_audio_only(1);
    auto_state.lifetime_producer_overflow_count += overflow;
    auto_state.producer_overflow_count += overflow;
    cpu_clock_auto_collect_producers(1);
    auto_state.eval_count++;
    codec_peak = auto_state.audio_only_codec_max_us;
    service_avg = cpu_clock_auto_decode_avg_us();
    current_cpu = (unsigned int)auto_clock_steps[auto_state.current_index];
    /* A read burst can exceed one block's duration while the PCM ring still
     * covers it. Keep whole-service throughput and actual output/queue pressure
     * as safety guards. Only the codec/PCM peak is a one-block cost constraint;
     * scale the whole-service average conservatively because demux CPU work
     * and storage/bus throughput can also worsen at a lower tier. Native codec
     * waits remain included; neither measurement is CPU utilization. */
    hard_pressure = auto_state.producer_overflow_count ||
        auto_state.audio_only_max_gap_us > budget * 2U ||
        (auto_state.audio_only_blocks >= 6U && auto_state.decode_count &&
         auto_state.audio_only_empty_count >= 2U && service_avg > budget * 3U / 4U);
    soft_pressure = auto_state.audio_only_blocks >= 6U && auto_state.decode_count &&
        codec_peak > budget;
    if (hard_pressure || soft_pressure) {
        if (!hard_pressure && cpu_clock_saver_defer_danger(now)) {
            auto_state.stable_windows = 0;
            cpu_clock_auto_clear_window();
            auto_state.last_eval_us = now;
            return;
        }
        auto_state.saver_danger_since_us = 0;
        cpu_clock_auto_set_index(CPU_CLOCK_AUTO_MAX_INDEX, "audio_starvation");
        auto_state.boost_until_us = now + (extreme_battery_saver ?
            CPU_CLOCK_SAVER_RECOVERY_HOLD_US : CPU_CLOCK_AUTO_DANGER_HOLD_US);
        auto_state.stable_windows = 0;
    } else if (auto_state.audio_only_blocks >= 6U && auto_state.decode_count &&
               now >= auto_state.boost_until_us && now >= auto_state.warmup_min_until_us &&
               auto_state.current_index > CPU_CLOCK_AUTO_MIN_INDEX) {
        uint64_t projected_peak, projected_service;
        target = auto_state.current_index - 1;
        projected_peak = (uint64_t)codec_peak * current_cpu / (unsigned int)auto_clock_steps[target];
        projected_service = (uint64_t)service_avg * current_cpu / (unsigned int)auto_clock_steps[target];
        if (projected_peak + 1000U <= budget / 2U && projected_service <= budget * 3U / 4U &&
            !auto_state.audio_only_empty_count &&
            auto_state.audio_only_max_gap_us <= budget + budget / 3U)
            auto_state.stable_windows++;
        else auto_state.stable_windows = 0;
        if (auto_state.stable_windows >= (extreme_battery_saver ? 1U : 2U)) {
            cpu_clock_auto_set_index(target, "audio_downshift");
            auto_state.boost_until_us = now + (extreme_battery_saver ?
                250000ULL : CPU_CLOCK_AUTO_SETTLE_US);
            auto_state.stable_windows = 0;
        }
    } else {
        auto_state.stable_windows = 0;
    }
    if (!hard_pressure && !soft_pressure)
        auto_state.saver_danger_since_us = 0;
    cpu_clock_auto_clear_window();
    auto_state.last_eval_us = now;
}

void cpu_clock_auto_on_frame_skipped(void) {
	ppa_session_note_skip();
	uint64_t now;

	if (!cpu_clock_auto_active())
		return;

	now = cpu_clock_auto_now_us();

	auto_state.skipped_count++;
	auto_state.lifetime_skipped_count++;
	if (now >= auto_state.optional_work_block_until_us)
		auto_state.optional_work_shed_count++;
	if (now + 2000000ULL > auto_state.optional_work_block_until_us)
		auto_state.optional_work_block_until_us = now + 2000000ULL;

	auto_state.stable_windows = 0;
	if (extreme_battery_saver) {
		uint64_t previous_skip = auto_state.saver_last_skip_us;
		auto_state.saver_last_skip_us = now;
		/* Isolated frame drops are the deliberate saver quality concession.
		 * Repeated drops recover immediately, including during a settle hold. */
		if (previous_skip == 0U || now < previous_skip ||
		    now - previous_skip > CPU_CLOCK_SAVER_TRANSIENT_US)
			return;
		auto_state.saver_danger_since_us = 0;
	}
	auto_state.boost_until_us = now + (extreme_battery_saver ?
		CPU_CLOCK_SAVER_RECOVERY_HOLD_US : CPU_CLOCK_AUTO_DANGER_HOLD_US);

	auto_state.skip_boost_count++;
	cpu_clock_auto_set_index(CPU_CLOCK_AUTO_MAX_INDEX, "skip");
}

void cpu_clock_auto_on_decode_us(unsigned int elapsed_us,
                                 int video_frame_duration_ms) {
	ppa_session_note_decode(elapsed_us, video_frame_duration_ms);
	if (video_frame_duration_ms > 0)
		auto_state.frame_duration_us = (unsigned int)video_frame_duration_ms * 1000U;

	if (!cpu_clock_auto_active())
		return;

	cpu_clock_auto_publish(&g_decode_producer, elapsed_us);

	/* The completed-presentation callback is the sole decision owner. */
}

void cpu_clock_auto_on_gu_draw_us(unsigned int elapsed_us) {
	if (!cpu_clock_auto_active())
		return;

	cpu_clock_auto_publish(&g_gu_producer, elapsed_us);
}

void cpu_clock_auto_on_postprocess_us(unsigned int elapsed_us) {
	if (!cpu_clock_auto_active())
		return;

	/* Keep this separate from GU timing. Averaging both kinds of samples in one
	 * counter under-reports their sequential per-frame cost; downshift projection
	 * combines the two averages explicitly. */
	cpu_clock_auto_publish(&g_postprocess_producer, elapsed_us);
}

void cpu_clock_auto_on_audio_resample_us(unsigned int elapsed_us) {
	if (!cpu_clock_auto_active())
		return;
	cpu_clock_auto_publish(&g_audio_resample_producer, elapsed_us);
}

int cpu_clock_auto_optional_work_allowed(void)
{
	if (!cpu_clock_auto_active())
		return 1;
	return cpu_clock_auto_now_us() >= auto_state.optional_work_block_until_us;
}

void cpu_clock_set_auto_enabled(int enabled) {
	auto_clock_enabled = extreme_battery_saver ? 1 : (enabled ? 1 : 0);

	if (playback_clock_lock) {
		if (auto_clock_enabled) {
			cpu_clock_auto_reset();
		}
		else {
			cpu_clock_apply_cpu(333);
		}
	}
	else {
		cpu_clock_apply_speed(&manual_speed);
	}
}

int cpu_clock_get_auto_enabled(void) {
	return auto_clock_enabled;
}

void cpu_clock_set_extreme_battery_saver(int enabled) {
	int next = enabled ? 1 : 0;
	if (extreme_battery_saver == next) {
		if (next && !auto_clock_enabled) {
			auto_clock_enabled = 1;
			if (playback_clock_lock)
				cpu_clock_auto_reset();
			else
				cpu_clock_apply_speed(&manual_speed);
		}
		return;
	}

	if (next) {
		extreme_battery_saved_auto_enabled = auto_clock_enabled;
		extreme_battery_saved_auto_valid = 1;
		extreme_battery_saver = 1;
		auto_clock_enabled = 1;
	}
	else {
		extreme_battery_saver = 0;
		if (extreme_battery_saved_auto_valid)
			auto_clock_enabled = extreme_battery_saved_auto_enabled;
		extreme_battery_saved_auto_valid = 0;
	}

	if (playback_clock_lock) {
		if (auto_clock_enabled)
			cpu_clock_auto_reset();
		else
			cpu_clock_apply_cpu(333);
	}
}

int cpu_clock_get_extreme_battery_saver(void) {
	return extreme_battery_saver;
}

int cpu_clock_get_current_cpu(void) {
	return current_speed.cpu;
}

void cpu_clock_set_cpu_speed(int cpu) {
	cpu_clock_make_speed(cpu, &manual_speed);

	if (!playback_clock_lock || !auto_clock_enabled)
		cpu_clock_apply_speed(&manual_speed);
}

void cpu_clock_set_speed(struct speed_setting_struct *speed) {
	if (speed == 0)
		return;

	manual_speed = *speed;

	if (!playback_clock_lock || !auto_clock_enabled)
		cpu_clock_apply_speed(&manual_speed);
}

void cpu_clock_set_maximum(void) {
	/* Decoder startup and recovery callers share the same clock owner.  A raw
	 * apply while auto mode is active would desynchronise current_index and
	 * residence accounting, so route it through the governor instead. */
	if (cpu_clock_auto_active()) {
		cpu_clock_auto_boost();
		return;
	}

	cpu_clock_apply_cpu(333);
}

void cpu_clock_set_minimum(void) {
	if (playback_clock_lock) {
		if (auto_clock_enabled) {
			cpu_clock_apply_cpu(auto_clock_steps[auto_state.current_index]);
		}
		else {
			cpu_clock_apply_cpu(333);
		}

		return;
	}

	cpu_clock_apply_speed(&manual_speed);
}

void cpu_clock_enter_playback(void) {
	playback_clock_lock = 1;
	ppa_bus_manager_reset();

	if (auto_clock_enabled) {
		cpu_clock_auto_reset();
	}
	else {
		cpu_clock_apply_cpu(333);
	}
}

void cpu_clock_leave_playback(void) {
	
	playback_clock_lock = 0;
	cpu_clock_auto_collect_producers(0);
	cpu_clock_auto_clear_window();
	cpu_clock_apply_speed(&manual_speed);
}

void cpu_clock_auto_finish_session(void) {
	uint64_t now;

	/* Drain the final producer samples before leaving automatic playback. */

	if (!auto_clock_enabled)
		return;

	now = cpu_clock_auto_now_us();
	cpu_clock_auto_collect_producers(1);
	cpu_clock_auto_account_time(now);

}
