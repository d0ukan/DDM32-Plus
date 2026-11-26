#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus: Menu > Channels slow to open and to move (owner's report on v1.2.0; docs/plus.md "Channels list speed").

Every codeplug_kv_get (each channel load: its tag, a P25 channel's mode parameters and the radio-wide P25 unit ID)
checked the whole settings chain again (chain_check: every page's CRC in 16-byte reads and two entry walks), then
raw_next checked each page once more; the Channels screen loads every channel on entry and 8 per move, so the cost was
channels x settings size. Measured on the host fixture (tests/plus/test_kv_cache.c): 27 channels / 13 P25 tags 5,557
data-chip reads to open the list, 1,304 per move; 60 channels / 30 tags 24,707 and 2,864.

- codeplug_kv.c (vendored, written by tools/classic_standard.py, so patched here with exact-match asserts): the chain
  is checked whole once per codeplug_kv_generation() and remembered (kv_ok); while unchanged a lookup reads the root
  header (compared with the CRC of the one checked) and walks the entries with each page's header only.
  codeplug_kv_generation() moves on every key write and sector commit (codeplug_kv_write / codeplug_kv_commit, also
  when they fail or are cut), every codeplug_serial() change (reload, CPS / TFTP / DOTA end, foreign, accept, start
  new) and every flash write or erase through the storage layer (storage.c clears storage_untouched in wr(), er(),
  the replace path and storage_init: the journal, records, sectors, replay at power on). A corrupt area is still
  found by the first lookup after any of those, exactly as before; the debug link's codeplug_kv_info and every write
  path keep the full check.
- classic_standard.inc (ours): codeplug_p25_unit_id caches the radio-wide P25 unit ID per generation.
- ui_menus.c: chlist_current() (a zone read) once per list count() instead of once per row.
"""

KV = 'core/codeplug/codeplug_kv.c'
KV_H = 'core/codeplug/codeplug_kv.h'


def patch_vendored(dest, write, relative, replacements):
    """Exact-match replacements in a file tools/classic_standard.py wrote from third_party (not in the upstream pin,
    so not through transform)"""
    path = dest / relative
    content = path.read_text()
    for old, new in replacements:
        if content.count(old) != 1:
            raise SystemExit(f"Pinned patch mismatch: {relative}: {old}")
        content = content.replace(old, new)
    write(path, content.encode())


def prepare(transform, dest, write):
    patch_vendored(dest, write, KV, [
        ('#define KV_CHAIN_PAGES 33u\n',
         '#define KV_CHAIN_PAGES 33u\n'
         '/* DDM32 Plus (tools/plus_chlag.py): the chain checked whole once per codeplug_kv_generation(), not at every\n'
         ' * lookup; all under storage_lock */\n'
         'static uint32_t kv_gen;\t\t// moved by key writes, sector commits and storage writes (codeplug_kv_generation)\n'
         'static uint32_t kv_ok_gen;\t// the generation the chain was found valid at (kv_ok bit 0)\n'
         'static uint32_t kv_ok_sig;\t// the CRC of its root header then: a raw change of the header is seen at once\n'
         'static uint8_t kv_ok;\t\t// bit 0: valid at kv_ok_gen; bit 1: codeplug_kv_next walks it (page_check trusts)\n'),
        ('*next=get32(b+12);uint32_t sum=0;',
         '*next=get32(b+12);if(kv_ok&2u)return CODEPLUG_KV_VALID;uint32_t sum=0;'),
        ('  if(!c->started){c->first=codeplug_kv_serial();if(chain_check(&len,&entries,&banks)!=CODEPLUG_KV_VALID)'
         'c->done=true;else{uint8_t h[16];uint32_t a;if(c->first!=codeplug_kv_serial()||!area_addr(&a)||'
         '!board.flash->read(a,h,sizeof h))c->done=true;else{c->root_crc=get32(h+8);c->root_next=get32(h+12);}}}\n',
         '  if(!c->started){c->first=codeplug_kv_serial();uint32_t g=codeplug_kv_generation(),a=0;uint8_t h[16]={0};\n'
         '   /* checked at this generation and the root header as then: no chain_check */\n'
         '   if(!((kv_ok&1u)&&kv_ok_gen==g&&area_addr(&a)&&board.flash->read(a,h,sizeof h)&&'
         'storage_crc32(0,h,sizeof h)==kv_ok_sig)){\n'
         '    kv_ok=0;\n'
         '    if(chain_check(&len,&entries,&banks)!=CODEPLUG_KV_VALID||c->first!=codeplug_kv_serial()||!area_addr(&a)||'
         '!board.flash->read(a,h,sizeof h))c->done=true;\n'
         '    else{kv_ok_gen=g;kv_ok_sig=storage_crc32(0,h,sizeof h);kv_ok=1u;}\n'
         '   }\n'
         '   if(!c->done){c->root_crc=get32(h+8);c->root_next=get32(h+12);}}\n'),
        ('e->key=get16(b);e->len=get16(b+2);e->addr=',
         'e->key=get16(b);e->len=get16(b+2);\n'
         '  /* DDM32 Plus: a trusted page skips page_check\'s entry walk; never step past the page */\n'
         '  if((uint32_t)w->at+4u+e->len>w->len){w->done=true;break;}\n'
         '  e->addr='),
        ('  kv_walk_t walk={c->page,c->next,c->crc,c->hdr,c->len,c->at,c->done,c->started};kv_entry_t e;\n',
         '  if(!c->done&&(kv_ok&1u)&&kv_ok_gen==codeplug_kv_generation())kv_ok|=2u;\t// pages checked: headers only\n'
         '  kv_walk_t walk={c->page,c->next,c->crc,c->hdr,c->len,c->at,c->done,c->started};kv_entry_t e;\n'),
        (' }\n storage_unlock();return found;\n}\n',
         ' }\n kv_ok&=1u;storage_unlock();return found;\n}\n'),
        ('\treturn (uint8_t)(kv_serial + codeplug_serial());\t// both only go up: the sum moves when either does\n}\n',
         '\treturn (uint8_t)(kv_serial + codeplug_serial());\t// both only go up: the sum moves when either does\n}\n\n'
         '// DDM32 Plus: 32 bits, both parts only go up\n'
         'uint32_t codeplug_kv_generation(void)\n{\n'
         '\tstorage_lock();\n'
         '\tif (!storage_untouched) {\n'
         '\t\tstorage_untouched = true;\t// first: a write from now on clears it again\n'
         '\t\tkv_gen++;\n\t}\n'
         '\tuint32_t g = kv_gen + codeplug_serial();\n'
         '\tstorage_unlock();\n'
         '\treturn g;\n}\n'),
        (' if(ok&&changed)kv_serial++;\n',
         ' if(ok&&changed)kv_serial++;\n'
         ' kv_gen++;kv_ok=0;\t// DDM32 Plus: written, failed or cut, the next lookup checks the chain whole\n'),
        ('\treturn codeplug_kv_keep(addr, sector) || storage_sector_write(addr, sector);\n',
         '\tstorage_lock();\n'
         '\tbool ok = codeplug_kv_keep(addr, sector) || storage_sector_write(addr, sector);\n'
         '\tkv_gen++;\t// DDM32 Plus: written, failed or cut, the next lookup checks the chain whole\n'
         '\tkv_ok = 0;\n'
         '\tstorage_unlock();\n'
         '\treturn ok;\n'),
    ])
    patch_vendored(dest, write, KV_H, [
        ('uint8_t codeplug_kv_serial(void);\n',
         'uint8_t codeplug_kv_serial(void);\n'
         '// DDM32 Plus (tools/plus_chlag.py): moves whenever the area may read differently: every key write or sector\n'
         '// commit (also a failed or cut one), every codeplug_serial() change and every flash write or erase through\n'
         '// storage (storage_untouched). A lookup checks the whole chain once per value; caches of keyed values key on it\n'
         'uint32_t codeplug_kv_generation(void);\n'),
    ])
    transform('core/storage/storage.h', [
        ('bool storage_sector_write(uint32_t addr, const uint8_t *data);\n',
         'bool storage_sector_write(uint32_t addr, const uint8_t *data);\n'
         '// DDM32 Plus (tools/plus_chlag.py): cleared by every flash write or erase through storage (journal, records,\n'
         '// sectors, replay) and by storage_init; codeplug_kv_generation() sets it and moves on when it finds it cleared\n'
         'extern bool storage_untouched;\n'),
    ])
    transform('core/storage/storage.c', [
        ('static bool fail(void)\n{',
         'bool storage_untouched;\t// storage.h (DDM32 Plus)\n\nstatic bool fail(void)\n{'),
        ('\treturn board.flash->write(addr, buf, len) || fail();',
         '\tstorage_untouched = false;\t// before: a failed or cut write changes the flash too\n'
         '\treturn board.flash->write(addr, buf, len) || fail();'),
        ('\treturn board.flash->erase(addr) || fail();',
         '\tstorage_untouched = false;\n\treturn board.flash->erase(addr) || fail();'),
        ('\tstorage_lock();\n\tif (same(addr, data, STORAGE_SECTOR)) {',
         '\tstorage_lock();\n\tstorage_untouched = false;\t// also the replace path below\n'
         '\tif (same(addr, data, STORAGE_SECTOR)) {'),
        ('\tready = false;\n\tregion = STORAGE_REGION_NONE;\n',
         '\tstorage_untouched = false;\t// power on: nothing remembered of the flash\n'
         '\tready = false;\n\tregion = STORAGE_REGION_NONE;\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('static uint16_t chlist_raw_count(void)\n{',
         '// DDM32 Plus (tools/plus_chlag.py): the channel we are on, read once per count() of the list (every draw,\n'
         '// move and key counts it before asking for rows), not once per row\n'
         'static uint16_t chlist_cur;\n\n'
         'static uint16_t chlist_count(void)\n{\n'
         '\tchlist_cur = chlist_current();\n'
         '\treturn ui_filter_count();\n}\n\n'
         'static uint16_t chlist_raw_count(void)\n{'),
        ('\tsb_channel_row(&b, &ch);\n\tif (num == chlist_current())\n',
         '\tsb_channel_row(&b, &ch);\n\tif (num == chlist_cur)\n'),
        ('\t.count = ui_filter_count, .item = chlist_item, .select = chlist_select, .context = chlist_context,\n',
         '\t.count = chlist_count, .item = chlist_item, .select = chlist_select, .context = chlist_context,\n'),
    ])
