/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_LAB_H
#define NATIVE_LAB_H
#include "radio/radio_sm.h"
#include "native_test.h"
void native_lab_init(void);
void native_lab_service(radio_sm_t *, uint32_t now_us);
int native_lab_reserved(void);
/* Radio-task wait cap: 0 unreserved, 2 ms raw bench, 5 ms internal RX audio.
 * Hardware capture/lease interrupts remain independent of this task wait. */
uint32_t native_lab_wait_ms(void);
int native_lab_owns(void);
/* On-device P25 worker only. RX fixed bounds, no TX token or persistent writes.
 * The radio task still performs all prepare/restore operations. */
uint32_t native_lab_voice_start(uint32_t frequency_hz,uint32_t duration_ms);
int native_lab_voice_poll(uint32_t session,uint8_t *state,uint8_t *reason);
int native_lab_voice_read(uint32_t session,nt_record *record);
void native_lab_voice_stop(uint32_t session);
int native_lab_voice_release(uint32_t session);
/* Ordinary radio requests interrupt a test. Internal RX-audio consumes PTT
 * edges (including a shared programming-jack transition) without aborting;
 * no PTT request may turn an interrupted test into normal FM transmit. */
/* WAIT retains the same request in the radio task until RF/worker release. */
enum { NATIVE_REQUEST_WAIT=2 };
int native_lab_request(radio_sm_t *, const radio_req_t *);
size_t native_lab_command(const uint8_t *, uint16_t, uint8_t *, size_t);
#if defined(CONFIG_P25_SCAN) && CONFIG_P25_SCAN
/* DM32 Plus, P25 in a zone scan (docs/plus.md): the radio core's scan on a P25 channel, every poll past the settle.
 * stop: the scan's stop count (a new value is a new stop); carrier: something is on the frequency. Counted for the
 * debug link (0x21 [12]). True while the channel at zone position pos is in its cooldown after a take that heard
 * nothing of ours: the scan does not stop there for now */
bool native_lab_scan_stop(uint16_t pos, uint8_t stop, bool carrier);
/* The channel the radio is on as the cooldown names it: its zone position, or 0xFFF0 + the VFO's index (DDM32 Plus
 * faster ring: a ring may hold VFOs) */
uint16_t native_lab_scan_key(const radio_sm_t *sm);
/* DDM32 Plus v1.0.3: the last take that was given back heard no frame of the channel's kind at all (a carrier that is
 * no P25, or nothing: the strength read high); false once a take hears frames */
bool native_lab_scan_quiet(void);
#endif
/* Local mic/PTT owner. Cable ARM cannot select this audio mode. */
#define NATIVE_PHYSICAL_PTT_DOWN 3
#define NATIVE_PHYSICAL_PTT_UP 4
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
int native_lab_live_poll(uint32_t session, uint8_t *state, uint8_t *reason, uint8_t *queued);
int native_lab_live_block(uint32_t session, const uint8_t raw[36], int last);
int native_lab_live_go(uint32_t session);
void native_lab_live_summary(uint32_t out[5]);
#endif
#endif
