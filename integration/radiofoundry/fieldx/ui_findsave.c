/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus: P25 / DMR search finds saved as channels (findsave.h), the UI task's sequencer. Installed as
 * core/ui/ui_findsave.c.
 *
 * Every write goes through the zone editor's queue (ui_svc_zone_edit, run as a storage job by ui_svc_storage.c, the
 * foreign codeplug question included): ZONE_OP_NEW for the zone, then one ZONE_OP_ADD_NEW a find (the channel
 * editor's "Copy here": the channel written at a free number and put at the zone's end in one job, checked before
 * anything is written; with CONFIG_CHAN_EXTRAS ZONE_OP_ADD_JOIN: the same and into "All channels" too). One step a UI tick, the next only once the queue is empty and the step is seen in the
 * codeplug (the zone found again by its name: a zone index can move). A step that did not land (a refusal, whose
 * own toast said why, a full codeplug, a full zone) ends the save with the count so far.
 *
 * RAM: the state below (14 B) and the toast text (36 B). The zone and channel copies live on the UI task's stack
 * only inside the noinline readers, never together with the zone_op_t of a post. */
#include "config.h"
#if CONFIG_FIND_SAVE
#include <string.h>
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "codeplug/codeplug.h"
#include "codeplug/zone_edit.h"
#include "ui/findsave.h"

enum { FS_IDLE, FS_START, FS_WAIT_NEW, FS_ADD, FS_WAIT_ADD };
enum { WHY_DONE, WHY_ZONE_FULL, WHY_CP_FULL, WHY_FAILED };

static struct {
	uint8_t phase;
	uint8_t kind;		/* FINDSAVE_P25 / FINDSAVE_DMR */
	uint8_t next, end;	/* the next find to save, one past the last */
	uint8_t saved, dup;
	bool one;		/* Save this one (into the newest find zone) */
	uint16_t num;		/* the zone's number: "Bulunanlar <num>" */
	uint16_t zone;		/* its index now */
	uint16_t chan;		/* the channel number the last add wrote */
} fs;
static char fs_msg[36];

typedef struct {
	uint16_t count;		/* channels in the zone */
	bool has;		/* fs.chan is in it */
	bool dup;		/* a channel for the find is in it */
} look_t;

static bool item(uint8_t i, findsave_item_t *f)
{
#if CONFIG_P25_SEARCH
	if (fs.kind == FINDSAVE_P25)
		return ui_p25search_find(i, f);
#endif
#if CONFIG_DMR_SEARCH
	if (fs.kind == FINDSAVE_DMR)
		return ui_dmrsearch_find(i, f);
#endif
	(void)i;
	(void)f;
	return false;
}

/* The highest number of the zones named "Bulunanlar N", and that zone's index (-1: none) */
static uint16_t __attribute__((noinline)) zones_scan(int16_t *at)
{
	zone_t z;
	uint16_t best = 0, n = codeplug_zone_count();
	*at = -1;
	for (uint16_t i = 0; i < n; i++) {
		if (!codeplug_zone(i, &z))
			continue;
		uint16_t k = findsave_zone_number(z.name);
		if (k > best) {
			best = k;
			*at = (int16_t)i;
		}
	}
	return best;
}

/* Our zone's index, found again by its name (an added or deleted zone moves the indexes): false when it is gone */
static bool __attribute__((noinline)) zone_find(void)
{
	zone_t z;
	uint16_t n = codeplug_zone_count();
	for (uint16_t i = 0; i < n; i++) {
		if (codeplug_zone(i, &z) && findsave_zone_number(z.name) == fs.num) {
			fs.zone = i;
			return true;
		}
	}
	return false;
}

/* Our zone's channel at position k (0: none) and its size in *count; false when the zone at fs.zone is not ours. The
 * zone copy lives only in this frame: the channel reads below happen without it on the stack */
static bool __attribute__((noinline)) zone_at(uint16_t k, uint16_t *num, uint16_t *count)
{
	zone_t z;
	if (!codeplug_zone(fs.zone, &z) || findsave_zone_number(z.name) != fs.num)
		return false;
	*count = z.count;
	*num = k < z.count ? z.channels[k] : 0u;
	return true;
}

/* Our zone as it is now: its size, whether fs.chan is in it, whether f (may be NULL) is. False: not our zone */
static bool __attribute__((noinline)) zone_read(const findsave_item_t *f, look_t *out)
{
	channel_t ch;
	uint16_t num, count;
	memset(out, 0, sizeof *out);
	if (!zone_at(0, &num, &count))
		return false;
	out->count = count;
	for (uint16_t k = 0; k < count; k++) {
		if (!zone_at(k, &num, &count))
			return false;
		if (fs.chan != 0 && num == fs.chan)
			out->has = true;
		if (f != NULL && !out->dup && codeplug_channel(num, &ch) && findsave_same(&ch, f))
			out->dup = true;
	}
	return true;
}

static bool zone_look(const findsave_item_t *f, look_t *out)
{
	return zone_read(f, out) || (zone_find() && zone_read(f, out));
}

static bool __attribute__((noinline)) post_new(void)
{
	zone_op_t op;
	memset(&op, 0, sizeof op);
	op.kind = ZONE_OP_NEW;
	findsave_zone_name(fs.num, op.name);
	return ui_svc_zone_edit(&op);
}

static bool __attribute__((noinline)) post_add(uint16_t pos, uint16_t num, const findsave_item_t *f, uint32_t own_id)
{
	zone_op_t op;
	memset(&op, 0, sizeof op);
#if CONFIG_CHAN_EXTRAS
	op.kind = ZONE_OP_ADD_JOIN;	/* the find zone at pos and All channels (zone_join.inc): Home steps through the finds too */
#else
	op.kind = ZONE_OP_ADD_NEW;
#endif
	op.zone = fs.zone;
	op.pos = pos;
	op.channel = num;
	findsave_channel(f, own_id, board_in_band(f->hz, true), &op.ch);
#if CONFIG_P25_CHANNELS
	if (codeplug_mode_p25(op.ch.mode) && !codeplug_p25_valid(&op.ch)) {
		op.ch.rx_only = true;	/* what the codeplug would refuse to transmit with: listen only */
		op.ch.p25_id = 0;
	}
#endif
	return ui_svc_zone_edit(&op);
}

/* The toast: "Bulunanlar 2: 5 saved, 1 there", "Zone full: 3 saved", "Saved to Bulunanlar 2", "Already in ..." */
static void finish(uint8_t why)
{
	char name[CODEPLUG_NAME_LEN + 1];
	sbuf_t b;
	findsave_zone_name(fs.num, name);
	sb_init(&b, fs_msg, sizeof fs_msg);
	if (why == WHY_DONE && fs.one) {
		sb_str(&b, fs.saved ? TR("Saved to ") : TR("Already in "));
		sb_str(&b, name);
	} else {
		if (why == WHY_DONE) {
			sb_str(&b, name);
		} else {
			sb_str(&b, why == WHY_ZONE_FULL ? TR("Zone full") : why == WHY_CP_FULL ? TR("Codeplug full") :
				   TR("Stopped"));
		}
		sb_str(&b, ": ");
		sb_u32(&b, fs.saved, 1);
		sb_str(&b, TR(" saved"));
		if (fs.dup) {
			sb_str(&b, ", ");
			sb_u32(&b, fs.dup, 1);
			sb_str(&b, TR(" there"));
		}
	}
	fs.phase = FS_IDLE;
	ui_toast(fs_msg);
}

/* Begin: the newest find zone for Save this one (unless the find is there already, or it is full), else a new one */
static void __attribute__((noinline)) step_start(void)
{
	int16_t at;
	findsave_item_t f;
	if (!item(fs.next, &f)) {
		fs.phase = FS_IDLE;	/* the list is gone (ui_scratch.h): nothing written, no zone made */
		ui_toast(TR("Stopped"));
		return;
	}
	uint16_t top = zones_scan(&at);
	if (fs.one && at >= 0) {
		look_t l;
		fs.num = top;
		fs.zone = (uint16_t)at;
		if (zone_read(&f, &l)) {
			if (l.dup) {
				fs.dup = 1;
				finish(WHY_DONE);
				return;
			}
			if (l.count < codeplug_zone_capacity()) {
				fs.phase = FS_ADD;
				return;
			}
		}
	}
	if ((codeplug_zone_count() >= CODEPLUG_ZONES_MAX && codeplug_zones_stored()) || top >= FINDSAVE_ZONE_NUM_MAX) {
		fs.phase = FS_IDLE;
		ui_toast(TR("No room for another zone"));
		return;
	}
	fs.num = (uint16_t)(top + 1u);
	if (!post_new())
		return;		/* the queue is full: again at the next tick */
	fs.phase = FS_WAIT_NEW;
}

static void __attribute__((noinline)) step_add(void)
{
	findsave_item_t f;
	look_t l;
	if (fs.next >= fs.end) {
		finish(WHY_DONE);
		return;
	}
	if (!item(fs.next, &f)) {
		finish(WHY_FAILED);	/* the list is gone: another screen took the shared buffer (ui_scratch.h) */
		return;
	}
	if (!zone_look(&f, &l)) {
		finish(WHY_FAILED);
		return;
	}
	if (l.dup) {
		fs.dup++;
		fs.next++;
		return;
	}
	if (l.count >= codeplug_zone_capacity()) {
		finish(WHY_ZONE_FULL);
		return;
	}
	uint16_t num = codeplug_free_channel();
	if (num == 0) {
		finish(WHY_CP_FULL);
		return;
	}
	uint32_t own = 0;
#if CONFIG_P25_CHANNELS
	if (f.kind == FINDSAVE_P25)
		own = codeplug_p25_unit_id(0);	/* the radio's own P25 unit ID (0: none) */
#endif
	fs.chan = num;
	if (post_add(l.count, num, &f, own))
		fs.phase = FS_WAIT_ADD;
}

bool ui_findsave_tick(void)
{
	if (fs.phase == FS_IDLE || ui_svc_zone_pending() || zone_edit_busy())
		return false;
	look_t l;
	switch (fs.phase) {
	case FS_START:
		step_start();
		break;
	case FS_WAIT_NEW:
		if (zone_find())
			fs.phase = FS_ADD;
		else
			finish(WHY_FAILED);	/* refused (its toast said why) or the codeplug question was answered No */
		break;
	case FS_WAIT_ADD:
		if (zone_look(NULL, &l) && l.has) {
			fs.saved++;
			fs.next++;
			fs.chan = 0;
			fs.phase = FS_ADD;
		} else {
			finish(WHY_FAILED);
		}
		break;
	default:
		step_add();
		break;
	}
	return true;
}

bool ui_findsave_start(uint8_t kind, uint8_t first, uint8_t end, bool one)
{
	if (fs.phase != FS_IDLE) {
		ui_toast(TR("Saving: wait"));
		return false;
	}
	if (first >= end) {
		ui_toast(TR("Nothing found to save"));
		return false;
	}
	memset(&fs, 0, sizeof fs);
	fs.kind = kind;
	fs.next = first;
	fs.end = end;
	fs.one = one;
	fs.phase = FS_START;	/* the codeplug is read in the tick: the key's own stack stays shallow */
	return true;
}

bool ui_findsave_busy(void)
{
	return fs.phase != FS_IDLE;
}

bool ui_findsave_line(char *buf, uint8_t len)
{
	if (fs.phase == FS_IDLE)
		return false;
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_str(&b, TR("Saving "));
	uint32_t total = fs.one ? 1u : fs.end, at = (uint32_t)fs.saved + fs.dup + 1u;	/* "all" starts at find 0 */
	sb_u32(&b, at < total ? at : total, 1);
	sb_str(&b, TR(" of "));
	sb_u32(&b, total, 1);
	return true;
}
#endif
