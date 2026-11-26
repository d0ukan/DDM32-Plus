// SPDX-License-Identifier: GPL-3.0-or-later
//! The map pack (type 3, docs/mappack-design.md): layout, the firmware's structural validation (C.2), the slot
//! choice (R3), the flags carry (S-A), user-built packs on update (S-B), the RadioID 192 KB limit (N2) and the
//! manifest's `ref_pack` object (E.2). Synthetic packs only, no port.
mod common;
use common::*;
use ddm32_core::datapacks::datapack::{self as dp, RADIO_ID, REF};
use ddm32_core::datapacks::files;
use ddm32_core::datapacks::refpack::{self as rp, Rules, Running, SlotChoice, SlotVerdict, UpdateDecision, Verdict};
use ddm32_core::releases::signed_manifest as sm;
use serde_json::json;

fn rules() -> Rules {
    Rules::new(Some(COORD))
}

fn good() -> Vec<u8> {
    ref_payload(COORD, &two_rpts(), "own+RadioID.net")
}

fn set16(p: &mut [u8], o: usize, v: u16) {
    p[o..o + 2].copy_from_slice(&v.to_le_bytes());
}
fn set32(p: &mut [u8], o: usize, v: u32) {
    p[o..o + 4].copy_from_slice(&v.to_le_bytes());
}

/// The payload offset of a section's first byte and its directory entry offset.
fn sec(p: &[u8], id: u16) -> (usize, usize) {
    let d = rp::directory(p).unwrap();
    let i = d.entries.iter().position(|e| e.id == id).unwrap();
    (d.entries[i].off as usize, 16 + i * 12)
}

fn is_bad(v: Verdict) -> bool {
    matches!(v, Verdict::Bad(_))
}
fn is_other(v: Verdict) -> bool {
    matches!(v, Verdict::Other(_))
}

#[test]
fn layout_two_slot_table() {
    assert_eq!(0x30000, dp::slot_size(RADIO_ID).unwrap());
    assert_eq!(0x10000, dp::slot_size(REF).unwrap());
    assert_eq!(0x4000, dp::slot_size(dp::AIR).unwrap());
    let slots: Vec<(u64, u64)> = dp::TYPES
        .iter()
        .flat_map(|t| (0..2).map(move |s| (dp::slot_address(*t, s).unwrap(), dp::slot_size(*t).unwrap() as u64)))
        .collect();
    assert_eq!(
        vec![0xE70000, 0xEB0000, 0xEA0000, 0xEE0000, 0xEF0000, 0xEF4000],
        slots.iter().map(|s| s.0).collect::<Vec<_>>()
    );
    for (i, a) in slots.iter().enumerate() {
        assert!(dp::in_region(a.0, a.1) && a.0 % 4096 == 0);
        for b in &slots[i + 1..] {
            assert!(a.0 + a.1 <= b.0 || b.0 + b.1 <= a.0, "{a:?} overlaps {b:?}");
        }
    }
    assert_eq!(("radioid", "ref", "air"), (dp::type_name(1), dp::type_name(3), dp::type_name(2)));
    let plan = dp::plan_for_slot(REF, 1, &vec![0; 0x10000]).unwrap();
    assert_eq!(16, plan.len());
    assert_eq!(0xEE0000, plan.last().unwrap().address);
    assert!(plan.iter().all(|s| (0xEE0000..0xEF0000).contains(&s.address)));
    assert!(dp::plan_for_slot(REF, 0, &vec![0; 0x10001]).is_err());
}

#[test]
fn a_well_formed_pack_is_valid_with_its_features() {
    assert_eq!(Verdict::Valid(rp::Features { map: true, rpt: true, air: true }), rp::validate(&good(), 0, &rules()));
    // without the repeater sections: still valid, the repeaters off (H.5, C.2 "absent is not a failure")
    let s: Vec<_> = ref_sections(&[], "").into_iter().filter(|s| !(10..=12).contains(&s.0)).collect();
    let p = rp::assemble(COORD, &s).unwrap();
    assert_eq!(Verdict::Valid(rp::Features { map: true, rpt: false, air: true }), rp::validate(&p, 0, &rules()));
    // an unknown section id is ignored, even misplaced (the firmware skips it entirely)
    let mut s = ref_sections(&two_rpts(), "x");
    s.push((99, 3, vec![1, 2, 3]));
    let mut p = rp::assemble(COORD, &s).unwrap();
    assert!(rp::validate(&p, 0, &rules()).is_valid());
    let e = 16 + 15 * 12;
    p[e + 4..e + 8].copy_from_slice(&3u32.to_le_bytes());
    p[e + 8..e + 12].copy_from_slice(&u32::MAX.to_le_bytes());
    assert!(rp::validate(&p, 0, &rules()).is_valid());
}

#[test]
fn every_structural_rule_broken_once_is_bad() {
    let g = good();
    let mut cases: Vec<(&str, Vec<u8>)> = Vec::new();
    let mut p = g.clone();
    p[0] = b'X';
    cases.push(("magic", p));
    let mut p = g.clone();
    p[5] = 17;
    cases.push(("17 sections", p));
    let (_, e) = sec(&g, rp::STEPS);
    let mut p = g.clone();
    set32(&mut p, e + 4, (g.len() - 100) as u32 & !3);
    cases.push(("section past the payload", p));
    let mut p = g.clone();
    set32(&mut p, e + 8, u32::MAX);
    cases.push(("off + rec * count overflowing u32", p));
    let mut p = g.clone();
    set32(&mut p, e + 4, 2 + rp::directory(&g).unwrap().get(rp::STEPS).unwrap().off);
    cases.push(("not aligned", p));
    let (provs_off, _) = sec(&g, rp::PROVINCES);
    let mut p = g.clone();
    set32(&mut p, e + 4, provs_off as u32);
    cases.push(("overlapping sections", p));
    let (rings, _) = sec(&g, rp::RINGS);
    let mut p = g.clone();
    set16(&mut p, rings + 4, 80);
    set16(&mut p, rings + 6, 5);
    cases.push(("ring past the steps", p));
    let mut p = g.clone();
    set16(&mut p, rings + 4, 0xFFFF);
    set16(&mut p, rings + 6, 2);
    cases.push(("ring first + count wrapping 16 bits", p));
    let mut p = g.clone();
    set16(&mut p, rings + 6, 0);
    cases.push(("ring of no step", p));
    // ranges must follow each other (the firmware's rule against crafted overlaps: one frame taking minutes)
    let mut p = g.clone();
    set16(&mut p, rings + 8 + 4, 0);
    cases.push(("two rings sharing the same step range", p));
    let mut p = g.clone();
    set16(&mut p, rings + 4, 1);
    set16(&mut p, rings + 8 + 4, 0);
    cases.push(("two rings with swapped step ranges", p));
    let mut p = g.clone();
    set16(&mut p, provs_off, 81);
    cases.push(("province ring past the rings", p));
    let mut p = g.clone();
    set16(&mut p, provs_off + 18, 0);
    cases.push(("a province reusing another's rings", p));
    let mut p = g.clone();
    p[provs_off + 2] = 2;
    cases.push(("a province's rings running into the next one's", p));
    let mut p = g.clone();
    set16(&mut p, provs_off + 4, 5000);
    cases.push(("province name past its blob", p));
    let mut p = g.clone();
    set16(&mut p, provs_off + 10, 20);
    cases.push(("province min > max", p));
    let (df, _) = sec(&g, rp::DIST_FIRST);
    let mut p = g.clone();
    set16(&mut p, df + 4, 0);
    cases.push(("DIST_FIRST decreasing", p));
    let mut p = g.clone();
    set16(&mut p, df + 162, 80);
    cases.push(("first[81] != DIST_POS count", p));
    let (nf, _) = sec(&g, rp::DIST_NAME_FIRST);
    let mut p = g.clone();
    set16(&mut p, nf + 160, 9000);
    cases.push(("district name offset past the blob", p));
    let (dn, _) = sec(&g, rp::DIST_NAMES);
    let dn_len = rp::directory(&g).unwrap().count(rp::DIST_NAMES) as usize;
    let mut p = g.clone();
    // the last province's only name loses its NUL: the blob's final NUL goes too
    p[dn + dn_len - 1] = b'x';
    cases.push(("blob without its final NUL", p));
    let (pn, _) = sec(&g, rp::PROV_NAMES);
    let pn_len = rp::directory(&g).unwrap().count(rp::PROV_NAMES) as usize;
    let mut p = g.clone();
    p[pn + pn_len - 1] = b'x';
    cases.push(("province blob without its final NUL", p));
    let (_, ke) = sec(&g, rp::RPT_KEY);
    let mut p = g.clone();
    set32(&mut p, ke + 8, 1);
    cases.push(("RPT / RPT_KEY counts", p));
    let (k, _) = sec(&g, rp::RPT_KEY);
    let mut p = g.clone();
    p[k + 8] ^= 1;
    cases.push(("a key differing from its record", p));
    let (air, _) = sec(&g, rp::AIR);
    let mut p = g.clone();
    set16(&mut p, air + 44 + 8, 2);
    p[air + 44 + 10] = 2;
    cases.push(("air first + count past the freqs", p));
    let mut p = g.clone();
    p[air + 10] = 65;
    cases.push(("air count > 64", p));
    let (akey, _) = sec(&g, rp::AIR_KEY);
    for (off, what) in [(0usize, "airfield key position"), (8, "airfield key prov"), (9, "airfield key kind")] {
        let mut p = g.clone();
        p[akey + off] ^= 1;
        cases.push((what, p));
    }
    let mut p = g.clone();
    p[air + 43] = 7;
    cases.push(("airfield prov differing from its key", p));
    let mut p = g.clone();
    set32(&mut p, e + 4, 16);
    cases.push(("a section inside the directory", p));
    let (_, nfe) = sec(&g, rp::DIST_NAME_FIRST);
    let mut p = g.clone();
    set16(&mut p, nfe, rp::DIST_FIRST); // same record size (2): a duplicate, not another version
    cases.push(("a known id twice", p));
    let (meta, _) = sec(&g, rp::MAP_META);
    let mut p = g.clone();
    set16(&mut p, meta + rp::META_RINGS, 80);
    cases.push(("MAP_META count", p));
    for (what, p) in cases {
        assert!(is_bad(rp::validate(&p, 0, &rules())), "{what}");
    }
    // incomplete groups
    let s: Vec<_> = ref_sections(&two_rpts(), "x").into_iter().filter(|s| s.0 != rp::RPT_KEY).collect();
    assert!(is_bad(rp::validate(&rp::assemble(COORD, &s).unwrap(), 0, &rules())));
    let s: Vec<_> = ref_sections(&two_rpts(), "x").into_iter().filter(|s| s.0 != rp::DIST_NAMES).collect();
    assert!(is_bad(rp::validate(&rp::assemble(COORD, &s).unwrap(), 0, &rules())));
}

#[test]
fn version_items_are_another_version_and_flags_zero_is_no_hint() {
    let g = good();
    let mut p = g.clone();
    p[4] = 2;
    assert!(is_other(rp::validate(&p, 0, &rules())), "directory version 2");
    let (_, e) = sec(&g, rp::RPT);
    let mut p = g.clone();
    set16(&mut p, e + 2, 48);
    assert!(is_other(rp::validate(&p, 0, &rules())), "a known id with another record size");
    assert!(is_other(rp::validate(&g, 0, &Rules::new(Some(COORD ^ 1)))), "the coordinate id");
    // flags: 0 = no hint (3.1 and datapack.py write 0); the right hint is valid; a disagreeing one is OTHER
    assert!(rp::validate(&g, 0, &rules()).is_valid());
    assert_eq!(0x4D01, rp::hint(COORD));
    assert!(rp::validate(&g, rp::hint(COORD), &rules()).is_valid());
    assert!(is_other(rp::validate(&g, 0x4E01, &rules())));
    assert!(is_other(rp::validate(&g, 0x4D02, &rules())));
    // version items are checked before the structure: a pack for another version that is also broken says OTHER
    let mut p = g.clone();
    p[4] = 2;
    p[5] = 17;
    assert!(is_other(rp::validate(&p, 0, &rules())));
    // firmware 1.1's hint: 0x1001, little-endian bytes 0x01, 0x10
    assert_eq!(0x1001, rp::hint(rp::COORD_1_1));
    assert_eq!([0x01, 0x10], rp::hint(rp::COORD_1_1).to_le_bytes());
    // the head's version items come before the directory's size: a pack for another id with 17 sections is OTHER
    let mut p = g.clone();
    p[5] = 17;
    assert!(is_other(rp::validate(&p, 0, &Rules::new(Some(COORD ^ 1)))));
    // a known id with another record size after a misplaced entry: still OTHER (the firmware returns at once)
    let mut p = g.clone();
    let (_, e_steps) = sec(&g, rp::STEPS);
    set32(&mut p, e_steps + 4, 2);
    let (_, e_rpt) = sec(&g, rp::RPT);
    set16(&mut p, e_rpt + 2, 48);
    assert!(is_other(rp::validate(&p, 0, &rules())));
    // a .dpk whose firmware is not known: its own coordinate id stands in
    assert!(rp::validate(&g, rp::hint(COORD), &Rules::new(None)).is_valid());
}

fn slot(payload: &[u8], seq: u32, tag: &str) -> Vec<u8> {
    ref_image(payload, seq, tag, 0)
}

fn empty() -> Vec<u8> {
    vec![0xFF; 64]
}

#[test]
fn judging_slots_as_the_firmware_does() {
    assert_eq!(SlotVerdict::Empty, rp::judge(&empty(), &rules()));
    // old RadioID bytes in a map tail: no DPK1, so NONE, not BAD (B.2)
    assert_eq!(SlotVerdict::Empty, rp::judge(&[0x12; 64], &rules()));
    let ok = slot(&good(), 3, "build 1.1.0");
    assert_eq!(SlotVerdict::Valid, rp::judge(&ok, &rules()));
    let mut crc = ok.clone();
    *crc.last_mut().unwrap() ^= 1;
    assert!(matches!(rp::judge(&crc, &rules()), SlotVerdict::Bad(_)));
    let mut hdr = ok.clone();
    hdr[30] ^= 1;
    assert!(matches!(rp::judge(&hdr, &rules()), SlotVerdict::Bad(_)));
    let rid = dp::image(RADIO_ID, &[0; 8], "20261119", "x", 1, "t", 0).unwrap();
    assert!(matches!(rp::judge(&rid, &rules()), SlotVerdict::Bad(_)));
    assert!(matches!(rp::judge(&ok, &Rules::new(Some(1))), SlotVerdict::Other(_)));
    // both rejected, one OTHER: the type says OTHER
    assert_eq!("other", rp::type_state(&[crc.clone(), ok.clone()], &Rules::new(Some(1))));
    assert_eq!("bad", rp::type_state(&[crc, empty()], &rules()));
    assert_eq!("none", rp::type_state(&[empty(), empty()], &rules()));
    assert_eq!("valid", rp::type_state(&[empty(), ok], &rules()));
}

#[test]
fn slot_choice_follows_r3() {
    let p = good();
    let other_payload = ref_payload(COORD, &two_rpts()[..1], "RadioID.net");
    let target = rules();
    let w = |slot, seq| Ok(SlotChoice::Write { slot, seq });
    // both empty: slot A, sequence 1
    assert_eq!(w(0, 1), rp::choose_slot(&[empty(), empty()], &Running::NoMapPack, &target, &p, &[], false));
    // A valid in force: B, above it
    let a5 = slot(&other_payload, 5, "build 1.1.0");
    assert_eq!(w(1, 6), rp::choose_slot(&[a5.clone(), empty()], &Running::Unknown, &target, &p, &[], false));
    // newer B for another version (OTHER, seq 7) next to the valid older A: B is overwritten, sequence above both
    let b7_other = slot(&ref_payload(COORD ^ 0x100, &two_rpts(), "x"), 7, "build 1.2.0");
    assert_eq!(w(1, 8), rp::choose_slot(&[a5.clone(), b7_other.clone()], &Running::NoMapPack, &target, &p, &[], false));
    // newer B damaged (payload CRC): overwritten, its header still counts for the sequence
    let mut b9_bad = slot(&other_payload, 9, "x");
    *b9_bad.last_mut().unwrap() ^= 1;
    assert_eq!(w(1, 10), rp::choose_slot(&[a5.clone(), b9_bad], &Running::NoMapPack, &target, &p, &[], false));
    // both valid for the target: the older one
    let b6 = slot(&other_payload, 6, "build 1.1.0");
    assert_eq!(w(0, 7), rp::choose_slot(&[a5.clone(), b6.clone()], &Running::NoMapPack, &target, &p, &[], false));
    // across versions: the running firmware (coordinate X) uses A; the target (Z) rejects both: A is kept
    let x = 0x0BAD_F00D;
    let ax = slot(&ref_payload(x, &two_rpts(), "x"), 4, "build 1.1.0");
    let bz_bad = slot(&ref_payload(x, &two_rpts(), "x"), 5, "build 1.1.0");
    let mut bz_bad = bz_bad;
    *bz_bad.last_mut().unwrap() ^= 1;
    assert_eq!(
        w(1, 6),
        rp::choose_slot(
            &[ax.clone(), bz_bad.clone()],
            &Running::MapPack(vec![x], vec![]),
            &Rules::new(Some(COORD)),
            &p,
            &[],
            false
        )
    );
    // ... even when the target would pick A (A older and B valid for the target): the running slot stays
    let bt = slot(&p, 3, "build 1.2.0");
    assert_eq!(
        Ok(SlotChoice::AlreadyThere { slot: 1 }),
        rp::choose_slot(&[ax.clone(), bt.clone()], &Running::MapPack(vec![x], vec![]), &target, &p, &[], false)
    );
    // the only free slot holds another target pack: it is replaced (never the running firmware's)
    // (the target's id is known as another version's: a pack of that id is not the running firmware's)
    assert_eq!(
        w(1, 5),
        rp::choose_slot(
            &[ax.clone(), bt],
            &Running::MapPack(vec![x], vec![COORD]),
            &target,
            &other_payload,
            &[],
            false
        )
    );
    // running version not known: the higher CRC-checked sequence is kept
    assert_eq!(w(0, 8), rp::choose_slot(&[a5.clone(), b7_other.clone()], &Running::Unknown, &target, &p, &[], false));
    // a map pack firmware whose coordinate id is not known: every id a slot holds is possible; two packs of two ids
    // may each be the running one: nothing is written
    assert!(rp::choose_slot(
        &[ax.clone(), slot(&p, 6, "b")],
        &Running::MapPack(vec![], vec![]),
        &target,
        &other_payload,
        &[],
        false
    )
    .is_err());
    // with the id known it goes ahead
    assert_eq!(
        w(1, 7),
        rp::choose_slot(
            &[ax, slot(&p, 6, "b")],
            &Running::MapPack(vec![x], vec![COORD]),
            &target,
            &other_payload,
            &[],
            false
        )
    );
    assert!(rp::choose_slot(&[empty()], &Running::NoMapPack, &target, &p, &[], false).is_err());
}

#[test]
fn the_sequence_counts_every_checked_header() {
    let mut bad = slot(&good(), 41, "x");
    *bad.last_mut().unwrap() ^= 1;
    assert_eq!(Ok(42), rp::next_seq(&[slot(&good(), 3, "x"), bad.clone()]));
    // the RadioID / air rule (datapack.next_seq) counts only CRC-valid packs, as before
    assert_eq!(Ok(4), dp::next_seq(&[slot(&good(), 3, "x"), bad]));
    assert!(rp::next_seq(&[slot(&good(), u32::MAX, "x")]).is_err());
}

#[test]
fn flags_are_carried_into_the_rebuilt_header() {
    let img = ref_image(&good(), 0, "build 1.1.0", rp::hint(COORD));
    let h = dp::parse(&img).0.unwrap();
    assert_eq!(rp::hint(COORD), h.flags);
    assert_eq!(img[6..8], rp::hint(COORD).to_le_bytes());
    let re = dp::image(REF, &img[64..], &h.date, &h.name, 9, &h.tag, h.flags).unwrap();
    assert_eq!(re[6..8], img[6..8]);
    assert_eq!(re[64..], img[64..]);
    // types 1 and 2 keep 0
    let rid = dp::image(RADIO_ID, &[0; 8], "20261119", "x", 1, "t", 0).unwrap();
    assert_eq!(0, dp::parse(&rid).0.unwrap().flags);
}

#[test]
fn release_built_is_the_header_tag_only() {
    assert!(rp::is_release_built("build 1.1.0"));
    assert!(rp::is_release_built("build 1.2.0-beta.1"));
    assert!(!rp::is_release_built("DDM32Plus-rs 1"));
    assert!(!rp::is_release_built("refpack.py own"));
    assert!(!rp::is_release_built("build "));
    assert!(!rp::is_release_built("builder 1"));
}

#[test]
fn update_step_never_replaces_a_user_built_pack_silently() {
    let release = ref_image(&good(), 0, "build 1.1.0", rp::hint(COORD));
    let target = rules();
    // nothing on the radio: write
    assert_eq!(
        UpdateDecision::WriteRelease,
        rp::update_decision(&[empty(), empty()], &Running::NoMapPack, &release, &target)
    );
    // the same pack: skipped
    let same = ref_image(&good(), 4, "build 1.1.0", rp::hint(COORD));
    assert_eq!(UpdateDecision::Same, rp::update_decision(&[same, empty()], &Running::Unknown, &release, &target));
    // an older release pack, even with the TRAC rows ("own+RadioID.net", owner decision 2026-11-26): replaced without asking
    let old = slot(&ref_payload(COORD, &two_rpts()[..1], "own+RadioID.net"), 2, "build 1.0.9");
    assert_eq!(
        UpdateDecision::WriteRelease,
        rp::update_decision(&[old, empty()], &Running::Unknown, &release, &target)
    );
    // a user-built pack (tag not "build ..."): asked, keep offered (valid for the target), rebuild offered
    let mine =
        slot(&ref_payload(COORD, &[rpt_rec(0, 0, 430_100_000, 0, "TA9OWN", 1)], "own list"), 3, "refpack.py own");
    match rp::update_decision(&[empty(), mine.clone()], &Running::Unknown, &release, &target) {
        UpdateDecision::AskUserBuilt { keep_ok, rebuild_ok, tag, .. } => {
            assert!(keep_ok && rebuild_ok);
            assert_eq!("refpack.py own", tag);
        }
        d => panic!("{d:?}"),
    }
    // a user-built pack for another version: keep not offered (it would show "for another version")
    let mine_old = slot(&ref_payload(COORD ^ 0x55, &[rpt_rec(0, 0, 430_100_000, 0, "TA9OWN", 1)], "own"), 3, "mine");
    match rp::update_decision(&[mine_old, empty()], &Running::Unknown, &release, &target) {
        UpdateDecision::AskUserBuilt { keep_ok, rebuild_ok, .. } => assert!(!keep_ok && rebuild_ok),
        d => panic!("{d:?}"),
    }
}

#[test]
fn rebuild_takes_the_release_sections_and_both_repeater_lists() {
    let release = good();
    let own = rpt_rec(0, 0, 430_100_000, 0, "TA9OWN", 1);
    let dup = two_rpts()[1].clone(); // also in the release
    let mine = ref_payload(COORD ^ 0x55, &[own.clone(), dup], "own list");
    let out = rp::rebuild(&release, rp::hint(COORD), &mine, &rules()).unwrap();
    assert!(rp::validate(&out, rp::hint(COORD), &rules()).is_valid());
    let d = rp::directory(&out).unwrap();
    assert_eq!(COORD, d.coord);
    assert_eq!(3, d.count(rp::RPT));
    let recs = d.bytes(&out, rp::RPT);
    let rx: Vec<u32> = recs.chunks(44).map(|r| u32::from_le_bytes([r[8], r[9], r[10], r[11]])).collect();
    assert_eq!(vec![145_600_000, 430_100_000, 439_000_000], rx);
    assert!(recs.chunks(44).any(|r| r == own.as_slice()));
    let info = rp::rpt_info(&out).unwrap();
    assert_eq!((3, "own list".to_string()), (info.count, info.source));
    // the map / airfield sections are the release's, byte for byte
    let rd = rp::directory(&release).unwrap();
    for id in [rp::STEPS, rp::PROVINCES, rp::DIST_NAMES, rp::AIR] {
        assert_eq!(rd.bytes(&release, id), d.bytes(&out, id), "section {id}");
    }
    // a user pack whose repeater records are of another version cannot be rebuilt
    let mut odd = mine.clone();
    let (_, e) = sec(&mine, rp::RPT);
    set16(&mut odd, e + 2, 48);
    assert!(rp::rebuild(&release, 0, &odd, &rules()).is_err());
}

#[test]
fn radioid_packs_over_192_kb_are_refused() {
    let t = tmp();
    // a 3.1-built pack of 196,545 bytes (fits 1.0.6's 256 KB slot): refused for every firmware (N2)
    let mut h = dp::header(RADIO_ID, &[], "20261119", "big", 0, "t", 0).unwrap();
    let payload = vec![0u8; dp::RADIO_ID_PAYLOAD_MAX + 1];
    h[12..16].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    let crc = ddm32_core::radio::monitor::crc32(&payload, 0);
    h[16..20].copy_from_slice(&crc.to_le_bytes());
    let hc = ddm32_core::radio::monitor::crc32(&h[..60], 0);
    h[60..64].copy_from_slice(&hc.to_le_bytes());
    let img = [h, payload].concat();
    std::fs::write(t.path().join("big.dpk"), &img).unwrap();
    let e = files::load(&t.path().join("big.dpk")).unwrap_err();
    assert!(e.0.contains("192 KB"), "{}", e.0);
    // exactly 196,544 bytes is taken
    let ok = dp::image(RADIO_ID, &vec![0u8; dp::RADIO_ID_PAYLOAD_MAX], "20261119", "max", 0, "t", 0).unwrap();
    assert!(files::check_image(&ok, "max.dpk").is_ok());
    assert!(dp::image(RADIO_ID, &vec![0u8; dp::RADIO_ID_PAYLOAD_MAX + 1], "20261119", "x", 0, "t", 0).is_err());
}

#[test]
fn a_map_dpk_is_loaded_only_when_it_checks() {
    let t = tmp();
    let img = ref_image(&good(), 0, "build 1.1.0", rp::hint(COORD));
    std::fs::write(t.path().join("ref.dpk"), &img).unwrap();
    let pk = files::load(&t.path().join("ref.dpk")).unwrap();
    assert_eq!(REF, pk.ptype);
    let mut bad = good();
    let (k, _) = sec(&bad, rp::RPT_KEY);
    bad[k] ^= 1;
    let e = files::check_image(&ref_image(&bad, 0, "x", 0), "bad.dpk").unwrap_err();
    assert!(e.0.contains("does not check"), "{}", e.0);
    let mut other = good();
    other[4] = 2;
    let e = files::check_image(&ref_image(&other, 0, "x", 0), "o.dpk").unwrap_err();
    assert!(e.0.contains("another firmware version"), "{}", e.0);
    assert!(dp::info(&img).contains("map pack: directory 1, coordinate id 0x1A2B3C4D, hint 0x4D01, 2 repeaters"));
}

// --- the manifest's ref_pack object (E.2) --------------------------------------------------------------------------

fn ref_entry(release: &str, data: &[u8]) -> serde_json::Value {
    json!({"schema": 1, "version": "1.1.0", "file": "ddm32-ref-1.1.0.dpk", "url": url(release, "ddm32-ref-1.1.0.dpk"),
           "sha256": sm::sha256_hex(data), "size": data.len(), "pack_type": 3, "pack_format": 1, "dir_version": 1,
           "coord_crc": format!("0x{COORD:08x}"), "date": "20261119", "min_firmware": "1.1.0"})
}

fn fw_with(rp_obj: Option<serde_json::Value>) -> serde_json::Value {
    let app = release_app("1.1.0");
    let mut e = firmware_entry(&app, "1.1.0", "stable", Some("v1.1.0"), None);
    if let Some(o) = rp_obj {
        e["ref_pack"] = o;
    }
    manifest(vec![e], "v1.1.0")
}

#[test]
fn manifest_ref_pack_is_parsed_and_checked() {
    let data = ref_image(&good(), 0, "build 1.1.0", rp::hint(COORD));
    let m = sm::check_manifest(&fw_with(Some(ref_entry("v1.1.0", &data))), &ddm32_core::releases::CURRENT).unwrap();
    let r = m.firmware[0].ref_pack.as_ref().unwrap();
    assert_eq!((3, 1, 1, COORD), (r.pack_type, r.pack_format, r.dir_version, r.coord_crc));
    assert_eq!(("1.1.0", "20261119", data.len() as u64), (r.min_firmware.as_str(), r.date.as_str(), r.size));
    assert!(!m.firmware[0].ref_pack_newer);
    // without the object: as before
    let m = sm::check_manifest(&fw_with(None), &ddm32_core::releases::CURRENT).unwrap();
    assert!(m.firmware[0].ref_pack.is_none());
    // a newer schema: ignored (the firmware still installs), "update the programmer"
    let mut newer = ref_entry("v1.1.0", &data);
    newer["schema"] = json!(2);
    newer["pack_type"] = json!(4);
    let m = sm::check_manifest(&fw_with(Some(newer)), &ddm32_core::releases::CURRENT).unwrap();
    assert!(m.firmware[0].ref_pack.is_none() && m.firmware[0].ref_pack_newer);
    // malformed schema-1 objects are refused
    for (k, v) in [
        ("pack_type", json!(2)),
        ("size", json!(0x10001)),
        ("sha256", json!("xyz")),
        ("file", json!("ddm32-ref.zip")),
        ("url", json!("https://example.com/ddm32-ref-1.1.0.dpk")),
        ("coord_crc", json!("1A2B3C4D")),
        ("coord_crc", json!("0x1A2B3C4D")),
        ("coord_crc", json!("0x1a2b3c4")),
        ("pack_format", json!(2)),
        ("file", json!("ddm32-ref-latest.dpk")),
        ("file", json!("ref-1.1.0.dpk")),
        ("dir_version", json!(0)),
        ("dir_version", json!(256)),
        ("coord_crc", json!(12)),
        ("date", json!("2026-11-19")),
        ("min_firmware", json!("one")),
        ("schema", json!(0)),
        ("schema", json!("1")),
    ] {
        let mut o = ref_entry("v1.1.0", &data);
        o[k] = v.clone();
        assert!(sm::check_manifest(&fw_with(Some(o)), &ddm32_core::releases::CURRENT).is_err(), "{k} = {v}");
    }
    assert!(sm::check_manifest(&fw_with(Some(json!("x"))), &ddm32_core::releases::CURRENT).is_err());
    // a pack of another version than its firmware entry: refused (parity with signed_manifest.py)
    let mut o = ref_entry("v1.1.0", &data);
    o["version"] = json!("1.1.1");
    o["file"] = json!("ddm32-ref-1.1.1.dpk");
    o["url"] = json!(url("v1.1.0", "ddm32-ref-1.1.1.dpk"));
    let e = sm::check_manifest(&fw_with(Some(o)), &ddm32_core::releases::CURRENT).unwrap_err();
    assert!(e.0.contains("ref_pack.version"), "{}", e.0);
    // a newer schema is still ignored, whatever its version says
    let mut o = ref_entry("v1.1.0", &data);
    o["schema"] = json!(2);
    o["version"] = json!("9.9.9");
    assert!(sm::check_manifest(&fw_with(Some(o)), &ddm32_core::releases::CURRENT).unwrap().firmware[0].ref_pack_newer);
}

#[test]
fn the_nested_object_leaves_the_older_fields_unchanged() {
    // The 3.1.0 parser reads only the keys it knows inside a firmware entry; the entry with the object gives the same
    // fields as the one without it (the 3.1.0 binary itself was checked against such a manifest by hand, see the
    // job report).
    let data = ref_image(&good(), 0, "build 1.1.0", 0);
    let a = sm::check_manifest(&fw_with(Some(ref_entry("v1.1.0", &data))), &ddm32_core::releases::CURRENT).unwrap();
    let b = sm::check_manifest(&fw_with(None), &ddm32_core::releases::CURRENT).unwrap();
    let (mut fa, fb) = (a.firmware[0].clone(), b.firmware[0].clone());
    fa.ref_pack = None;
    assert_eq!(fa, fb);
    // and the signature covers the object: one changed digit fails the signature
    let (bytes, sig) = signed(&fw_with(Some(ref_entry("v1.1.0", &data))), None);
    let key = test_public_hex();
    assert!(sm::verify_bytes(&bytes, &sig, Some(&key), None).is_ok());
    let text = String::from_utf8(bytes).unwrap().replacen("\"date\": \"20261119\"", "\"date\": \"20261118\"", 1);
    assert!(sm::verify_bytes(text.as_bytes(), &sig, Some(&key), None).is_err());
}

#[test]
fn the_release_map_pack_is_downloaded_and_checked() {
    use ddm32_core::releases::client;
    let t = tmp();
    let data = ref_image(&good(), 0, "build 1.1.0", rp::hint(COORD));
    let m = fw_with(Some(ref_entry("v1.1.0", &data)));
    let dir = t.path().join("cache");
    write_signed(&dir, &m, None);
    let manifest = sm::verify_files(&dir, Some(&test_public_hex()), None).unwrap();
    let mut cat = ddm32_core::releases::Catalog::new("test");
    cat.add(&manifest.release, &manifest, Some(dir.clone()));
    let fv = cat.firmware[0].clone();
    let net = FakeNet::default();
    net.files.borrow_mut().insert(url("v1.1.0", "ddm32-ref-1.1.0.dpk"), data.clone());
    let path = client::download_ref_pack(&fv, &t.path().join("releases"), &net, None).unwrap();
    assert_eq!(data, std::fs::read(&path).unwrap());
    assert!(
        path.ends_with("refpacks/1.1.0/ddm32-ref-1.1.0.dpk") || path.ends_with(r"refpacks\1.1.0\ddm32-ref-1.1.0.dpk")
    );
    // a second call uses the checked file
    client::download_ref_pack(&fv, &t.path().join("releases"), &net, None).unwrap();
    assert_eq!(1, net.downloads.borrow().len());
    // other bytes than signed: refused, nothing left behind
    let t2 = tmp();
    *net.payload.borrow_mut() = Some(ref_image(&good(), 0, "build 1.1.1", 0));
    let e = client::download_ref_pack(&fv, t2.path(), &net, None).unwrap_err();
    assert_eq!(Some(ddm32_core::releases::VerifyFailure::HashMismatch), e.kind());
    assert!(!t2.path().join("refpacks/1.1.0/ddm32-ref-1.1.0.dpk").exists());
}

#[test]
fn firmware_1_1_meta_must_be_its_compiled_origin() {
    // with firmware 1.1's coordinate id MAP_META must carry its origin, unit and bounds
    let mut s = ref_sections(&two_rpts(), "x");
    let mut meta = s[0].2.clone();
    let (ints, shorts) = rp::META_1_1;
    for (i, v) in ints.iter().enumerate() {
        meta[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
    }
    for (i, v) in shorts.iter().enumerate() {
        meta[12 + i * 2..14 + i * 2].copy_from_slice(&v.to_le_bytes());
    }
    s[0].2 = meta.clone();
    let r11 = Rules::new(Some(rp::COORD_1_1));
    assert!(rp::validate(&rp::assemble(rp::COORD_1_1, &s).unwrap(), 0x1001, &r11).is_valid());
    meta[16] ^= 1; // XMAX
    s[0].2 = meta;
    assert!(is_bad(rp::validate(&rp::assemble(rp::COORD_1_1, &s).unwrap(), 0x1001, &r11)));
    // a zeroed origin passes only for another coordinate id (its compiled values are not known here)
    assert!(rp::validate(&good(), 0, &rules()).is_valid());
}

/// The real public 1.1 map pack built by tools/refpack.py (job mappack-fw). Not committed (its repeater rows come
/// from RadioID.net, docs/mappack-design.md E.5); checked when the file is there (DDM32_REF_PACK or the job's path).
#[test]
fn the_real_public_pack_is_valid_when_present() {
    let path = std::env::var("DDM32_REF_PACK")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|_| std::path::PathBuf::from("E:/GIT-REPO/ddm32-wt/mp-scripts/ddm32-ref-public.dpk"));
    let Ok(data) = std::fs::read(&path) else {
        eprintln!("{} not here: skipped", path.display());
        return;
    };
    let pk = files::check_image(&data, "ddm32-ref-public.dpk").unwrap();
    assert_eq!(REF, pk.ptype);
    assert_eq!(0x1001, pk.header.flags);
    let payload = &data[64..];
    let d = rp::directory(payload).unwrap();
    assert_eq!(rp::COORD_1_1, d.coord);
    assert_eq!(
        Verdict::Valid(rp::Features { map: true, rpt: true, air: true }),
        rp::validate(payload, pk.header.flags, &Rules::new(Some(rp::COORD_1_1)))
    );
    assert_eq!(SlotVerdict::Valid, rp::judge(&ref_image_seq(&data, 5), &Rules::new(Some(rp::COORD_1_1))));
    assert!(rp::is_release_built(&pk.header.tag), "{}", pk.header.tag);
    // a 1.2 with another id calls it another version
    assert!(is_other(rp::validate(payload, pk.header.flags, &Rules::new(Some(0x1234_5678)))));
}

/// The pack with another sequence, its flags, date, name and tag carried (as the writer rebuilds it).
fn ref_image_seq(data: &[u8], seq: u32) -> Vec<u8> {
    let h = dp::parse(data).0.unwrap();
    dp::image(REF, &data[64..], &h.date, &h.name, seq, &h.tag, h.flags).unwrap()
}

#[test]
fn verifier_findings_on_the_slot_choice() {
    let target = rules();
    let release = good();
    let mine = ref_payload(COORD, &[rpt_rec(0, 0, 430_100_000, 0, "TA9OWN", 1)], "own list");
    let w = |slot, seq| Ok(SlotChoice::Write { slot, seq });
    let run11 = Running::MapPack(vec![COORD], vec![]);
    // SF1: the release pack in the slot NOT in force (A: the user's pack, seq 5, in force) is written again into B
    // with a sequence above A, never "already there"
    let images = [slot(&mine, 5, "user"), slot(&release, 4, "build 1.1.0")];
    assert_eq!(w(1, 6), rp::choose_slot(&images, &run11, &target, &release, &[], false));
    assert_eq!(w(1, 6), rp::choose_slot(&images, &Running::NoMapPack, &target, &release, &[], false));
    // ... and "already there" only when the identical pack is the one in force
    let images = [slot(&mine, 3, "user"), slot(&release, 4, "build 1.1.0")];
    assert_eq!(
        Ok(SlotChoice::AlreadyThere { slot: 1 }),
        rp::choose_slot(&images, &run11, &target, &release, &[], false)
    );
    // SF3: a 1.0.x radio with a RadioID pack over 192 KB in slot A: map A (its tail) is avoided
    assert_eq!(w(1, 1), rp::choose_slot(&[empty(), empty()], &Running::NoMapPack, &target, &release, &[0], false));
    // both partners big: refused unless allowed
    let e = rp::choose_slot(&[empty(), empty()], &Running::NoMapPack, &target, &release, &[0, 1], false).unwrap_err();
    assert!(e.starts_with(rp::RADIOID_LOSS), "{e}");
    assert_eq!(w(0, 1), rp::choose_slot(&[empty(), empty()], &Running::NoMapPack, &target, &release, &[0, 1], true));
    // the only slot the rules leave is avoided (the other is the target's only valid pack): refused, then allowed
    let images = [slot(&mine, 5, "user"), empty()];
    assert!(rp::choose_slot(&images, &run11, &target, &release, &[1], false).is_err());
    assert_eq!(w(1, 6), rp::choose_slot(&images, &run11, &target, &release, &[1], true));
    // SF4: a 1.1.x patch build whose id the program does not know: the slot valid under its own id is kept
    let patch = 0x0BAD_F00D;
    let images = [slot(&ref_payload(patch, &two_rpts(), "x"), 7, "build 1.1.1"), slot(&release, 3, "build 1.1.0")];
    assert_eq!(Err(rp::BOTH_IN_USE.to_string()), rp::choose_slot(&images, &run11, &target, &mine, &[], false));
    // when that id is known to be another version's, it is not protected
    let run11b = Running::MapPack(vec![COORD], vec![patch]);
    assert_eq!(w(0, 8), rp::choose_slot(&images, &run11b, &target, &mine, &[], false));
}

#[test]
fn reverification_findings() {
    let target = rules();
    // SF-A: the rings cover exactly the steps, the provinces exactly the rings
    let mut s = ref_sections(&two_rpts(), "x");
    s[3].2.extend([1, 1]); // one step more than the rings use
    s[0].2[rp::META_STEPS..rp::META_STEPS + 2].copy_from_slice(&82u16.to_le_bytes());
    assert!(is_bad(rp::validate(&rp::assemble(COORD, &s).unwrap(), 0, &target)), "a step no ring uses");
    let mut s = ref_sections(&two_rpts(), "x");
    s[1].2[80 * 18 + 2] = 0; // the last province's ring count 1 -> 0
    assert!(is_bad(rp::validate(&rp::assemble(COORD, &s).unwrap(), 0, &target)), "a ring no province uses");
    // SF-B: a 1.3 radio whose own id the program does not know: ids of other versions may still be in force
    let x = 0x0BAD_F00D;
    let a5 = slot(&ref_payload(x, &two_rpts(), "x"), 5, "build 1.1.0");
    let mut b9 = slot(&good(), 9, "build 1.2.0");
    *b9.last_mut().unwrap() ^= 1;
    let w = |slot, seq| Ok(SlotChoice::Write { slot, seq });
    assert_eq!(
        w(1, 10),
        rp::choose_slot(
            &[a5.clone(), b9.clone()],
            &Running::MapPack(vec![], vec![x, COORD]),
            &target,
            &good(),
            &[],
            false
        )
    );
    // with the version's own id known, other versions' ids are not the running one
    assert_eq!(
        w(0, 10),
        rp::choose_slot(&[a5, b9], &Running::MapPack(vec![0x5555], vec![x, COORD]), &target, &good(), &[], false)
    );
    // N-a: the refusal says what to do
    assert!(rp::BOTH_IN_USE.contains("check for updates online"));
}

/// SF-A on the real public pack (when present): its last ring or province shortened is BAD, as the firmware says.
#[test]
fn the_real_pack_shortened_is_bad() {
    let path = std::env::var("DDM32_REF_PACK")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|_| std::path::PathBuf::from("E:/GIT-REPO/ddm32-wt/mp-scripts/ddm32-ref-public.dpk"));
    let Ok(data) = std::fs::read(&path) else { return };
    let payload = data[64..].to_vec();
    let r = Rules::new(Some(rp::COORD_1_1));
    assert!(rp::validate(&payload, 0x1001, &r).is_valid());
    let d = rp::directory(&payload).unwrap();
    let rings = d.get(rp::RINGS).unwrap();
    let last_ring = rings.off as usize + 8 * (rings.count as usize - 1);
    let mut p = payload.clone();
    let n = u16::from_le_bytes([p[last_ring + 6], p[last_ring + 7]]);
    p[last_ring + 6..last_ring + 8].copy_from_slice(&(n - 1).to_le_bytes());
    assert!(is_bad(rp::validate(&p, 0x1001, &r)), "last ring {n} -> {}", n - 1);
    let provs = d.get(rp::PROVINCES).unwrap();
    let last_prov = provs.off as usize + 18 * 80;
    let mut p = payload;
    p[last_prov + 2] -= 1;
    assert!(is_bad(rp::validate(&p, 0x1001, &r)), "last province one ring less");
}
