// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text;

namespace DDM32Plus.Core.Radio;

/// <summary>One entry of a radio search's found list as one poll shows it (no audio). Mode: "P25", "DMR", "YSF" or
/// "NXDN"; Code: P25 NAC ("$1C8/456"), DMR colour code ("CC1"), YSF DG-ID ("DG0") or NXDN RAN ("RAN1"); Slot: DMR slots
/// seen ("TS1", "TS2", "TS1+2") and RPT / DIR; Group: P25 / NXDN talkgroup or DMR talkgroup / called ID; Caller: the
/// YSF source callsign (empty otherwise); Calls: the radio's call counter of the entry; Id: the entry's serial
/// (searchlog_view.c), which stays while the radio fills in its code and group.</summary>
public sealed record SearchCall(DateTime Time, string Mode, long Hz, string Code, string Slot, long Group, long Source,
    bool Encrypted, int Calls, string Caller = "", long Id = -1);

/// <summary>One line of the log: every call heard on one mode + frequency + code (NAC / CC / DG-ID / RAN) + group,
/// counted (owner 2026-11-21: "how many frequencies, which NAC, which group have traffic"). First / Last: when it was
/// first and last heard; Source / Caller / Slot: the last call's; Calls: every call heard there since the log started.</summary>
public sealed class SearchGroup
{
    public required string Mode { get; init; }
    public required long Hz { get; init; }
    public required string Code { get; init; }
    public required long Group { get; init; }
    public DateTime First { get; set; }
    public DateTime Last { get; set; }
    public string Slot { get; set; } = "";
    public long Source { get; set; }
    public string Caller { get; set; } = "";
    public bool Encrypted { get; set; }
    public int Calls { get; set; }
}

/// <summary>The search statistics of one engine (firmware beta.9: the X.st line of p25s / dmrs / alls, 19 fields; see
/// docs/plus.md "Search log on the PC"). Algo: "fast" or "classic" (as the radio sends it); times in ms; 0 for
/// FirstMs / SeenMs / SyncLast / SyncMax / PassMs means none yet.</summary>
public sealed record SearchStats(string Algo, long UpMs, long Passes, long PassMs, long PassSteps, long Probes, long Found,
    long NoSync, long Partial, long Gone, long Skipped, long Early, long Watch, long ProbeMs, long SweepMs, long FirstMs,
    long SeenMs, long SyncLast, long SyncMax)
{
    public const int FieldCount = 19;

    /// <summary>The CSV keys of the fields, in order.</summary>
    public static readonly string[] CsvKeys =
    [
        "algo", "up_ms", "passes", "pass_ms", "pass_steps", "probes", "found", "nosync", "partial", "gone", "skipped",
        "early", "watch", "probe_ms", "sweep_ms", "first_ms", "seen_ms", "sync_last", "sync_max",
    ];

    /// <summary>The fields of an X.st value: ALGO,UP_MS,... Fewer than 19 fields, an empty algo or a field that is not
    /// a number gives null; fields after the 19th are ignored.</summary>
    public static SearchStats? Parse(string text)
    {
        var p = text.Split(',');
        if (p.Length < FieldCount || p[0].Trim().Length == 0)
            return null;
        var v = new long[FieldCount];
        for (var i = 1; i < FieldCount; i++)
            if (!long.TryParse(p[i].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out v[i]) || v[i] < 0)
                return null;
        return new SearchStats(p[0].Trim().ToLowerInvariant(), v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10],
            v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18]);
    }

    /// <summary>Share of the time spent probing (percent), null before any time is counted.</summary>
    public long? ProbingPercent => ProbeMs + SweepMs > 0 ? (ProbeMs * 100 + (ProbeMs + SweepMs) / 2) / (ProbeMs + SweepMs) : null;

    /// <summary>The CSV cells: the values in the order of <see cref="CsvKeys"/>.</summary>
    public string[] CsvValues() =>
    [
        Algo, .. new[] { UpMs, Passes, PassMs, PassSteps, Probes, Found, NoSync, Partial, Gone, Skipped, Early, Watch,
            ProbeMs, SweepMs, FirstMs, SeenMs, SyncLast, SyncMax }.Select(n => n.ToString(CultureInfo.InvariantCulture)),
    ];

    /// <summary>The page's one line, e.g. "Fast | pass 4.1 s (161 steps) | probes 12: found 3, no sync 7, partial 1,
    /// gone 1 | skipped 4, early 2, watch 3 | probing 38% | first find 6.2 s | seen-&gt;found 1.4 s | sync 0.42 s (max
    /// 0.81 s)". <paramref name="text"/> gives the page's string for a key (SLog_Stat_*: format strings of the resx
    /// files); parts with nothing known yet are left out.</summary>
    public string Describe(Func<string, string> text, CultureInfo culture)
    {
        string F(string key, params object[] args) => string.Format(culture, text(key), args);
        static string Sec(long ms, int digits, CultureInfo c) => (ms / 1000.0).ToString("0." + new string('0', digits), c);
        var parts = new List<string>
        {
            Algo switch { "fast" => text("SLog_Stat_Fast"), "classic" => text("SLog_Stat_Classic"), _ => Algo },
        };
        if (PassMs > 0)
            parts.Add(F("SLog_Stat_Pass", Sec(PassMs, 1, culture), PassSteps));
        parts.Add(F("SLog_Stat_Probes", Probes, Found, NoSync, Partial, Gone));
        parts.Add(F("SLog_Stat_Skips", Skipped, Early, Watch));
        if (ProbingPercent is { } pct)
            parts.Add(F("SLog_Stat_Probing", pct));
        if (FirstMs > 0)
            parts.Add(F("SLog_Stat_First", Sec(FirstMs, 1, culture)));
        if (SeenMs > 0)
            parts.Add(F("SLog_Stat_Seen", Sec(SeenMs, 1, culture)));
        if (SyncMax > 0 || SyncLast > 0)
            parts.Add(F("SLog_Stat_Sync", Sec(SyncLast, 2, culture), Sec(SyncMax, 2, culture)));
        return string.Join(" | ", parts);
    }
}

/// <summary>What <see cref="SearchLogBook.Write"/> did: the file written and whether the log's own file was held by
/// another program (Excel), so the rows went to <see cref="SearchLogBook.SidePath"/> instead.</summary>
public sealed record SearchCsvResult(string Path, bool Locked);

/// <summary>
/// The search log (firmware tools/plus_searchlog.py, docs/plus.md "Search log on the PC"): the radio's P25, DMR and all
/// digital searches keep 16 finds each (8 up to firmware 1.0.5) and send the newest 8 (f0..f7); read every second
/// over the debug link (GET_VIEW sections p25s = 28, dmrs = 29,
/// alls = 30, read only, not counted as a look). The log keeps ONE line per mode + frequency + code + group with a call
/// counter. Calls are counted by the radio's own entries (searchlog_view.c): every entry has a serial (ID) and a call
/// counter; the log follows each entry by engine (the P25 search, or the DMR search's engine that the all digital
/// search shares) + ID + frequency + mode and adds the change of its counter to the entry's line. So a call the radio
/// moves to its right talkgroup (one entry down, another up) is one call; an entry whose code / group the radio fills
/// in later moves its calls to the right line (the half known line goes); the same call in two lists is never two.
/// X.seq (the engine's calls since power on) going back, or the radio's uptime going back, is a restart: the entries
/// start anew, nothing counted is lost. Calls the radio counted on entries the log never saw (more than 8 between
/// two polls) are told in <see cref="Missed"/>.
/// </summary>
public sealed class SearchLogBook
{
    public const byte SectionP25 = 0x1C;
    public const byte SectionDmr = 0x1D;
    public const byte SectionAll = 0x1E;

    /// <summary>An entry not seen for this many polls is forgotten (it left the radio's list).</summary>
    public const int ForgetPolls = 10;

    private readonly record struct RowKey(string Mode, long Hz, string Code, long Group);

    private sealed class Entry
    {
        public RowKey? Key;     // its key at the last poll
        public RowKey? Row;     // its line (null: none yet, or its calls went back to 0)
        public DateTime? First; // when its first call was heard (null: no call counted yet, or cleared since)
        public int Calls;       // the radio's counter at the last poll
        public int Counted;     // what this log has counted of it (on Row)
        public int Seen;
    }

    private readonly Dictionary<RowKey, SearchGroup> groups = [];
    // an entry: engine + serial + mode + frequency (a serial wraps; a hot entry may outlive its number's next use)
    private readonly Dictionary<(char Engine, long Id, string Mode, long Hz), Entry> entries = [];
    private readonly Dictionary<char, long> seqs = [];
    private readonly Dictionary<char, int> balance = [];
    private readonly Dictionary<char, int> debt = [];
    private long? uptime;
    private int poll;

    /// <summary>Every line, the most recently heard first.</summary>
    public IReadOnlyList<SearchGroup> Groups => [.. groups.Values.OrderByDescending(g => g.Last).ThenBy(g => g.Hz).ThenBy(g => g.Group)];

    /// <summary>Whether the radio reported each search running at the last poll (null = no answer / not built).</summary>
    public bool? P25On { get; private set; }
    public bool? DmrOn { get; private set; }
    public bool? AllOn { get; private set; }
    public long? P25AtHz { get; private set; }
    public long? DmrAtHz { get; private set; }
    public long? AllAtHz { get; private set; }

    /// <summary>When each search was seen to stop (it ran at the poll before; null while it runs or never ran).</summary>
    public DateTime? P25StoppedAt { get; private set; }
    public DateTime? DmrStoppedAt { get; private set; }
    public DateTime? AllStoppedAt { get; private set; }

    /// <summary>The latest search statistics seen per engine (null: that engine never sent an X.st line; older
    /// firmware sends none).</summary>
    public SearchStats? P25Stats { get; private set; }
    public SearchStats? DmrStats { get; private set; }
    public SearchStats? AllStats { get; private set; }

    /// <summary>The engine whose statistics the page shows ("P25", "DMR" or "ALL"): one that runs now and has
    /// statistics, else the last one shown; null while no engine has sent any.</summary>
    public string? StatsEngine { get; private set; }

    /// <summary>The statistics of <see cref="StatsEngine"/>.</summary>
    public SearchStats? ShownStats => StatsEngine switch { "P25" => P25Stats, "DMR" => DmrStats, "ALL" => AllStats, _ => null };

    /// <summary>Every engine's statistics seen in this session, in the order P25, DMR, ALL.</summary>
    public IReadOnlyList<(string Engine, SearchStats Stats)> AllStatsSeen =>
        [.. new (string, SearchStats?)[] { ("P25", P25Stats), ("DMR", DmrStats), ("ALL", AllStats) }
            .Where(e => e.Item2 is not null).Select(e => (e.Item1, e.Item2!))];

    /// <summary>Radio restarts seen (uptime or a search's call sequence went back).</summary>
    public int Restarts { get; private set; }

    /// <summary>Calls the radio counted that no entry showed (more than 8 changed between two polls).</summary>
    public int Missed => balance.Values.Where(b => b > 0).Sum();

    /// <summary>Distinct frequencies with a call.</summary>
    public int Frequencies => groups.Values.Select(g => (g.Mode, g.Hz)).Distinct().Count();

    /// <summary>Every call counted.</summary>
    public int TotalCalls => groups.Values.Sum(g => g.Calls);

    /// <summary>"Kayıtları temizle": the lines go; the radio's entries stay known, so only calls heard from now on
    /// count (what the radio counted before is not found again).</summary>
    public void Clear()
    {
        groups.Clear();
        foreach (var e in entries.Values)
        {
            e.Row = null;
            e.Counted = 0;
            e.First = null;     // a line made again takes its first-heard from the next call
        }
        balance.Clear();
        debt.Clear();
    }

    /// <summary>Whether one poll's lines come from a firmware with the search log (beta.7 and later: an X.seq line).</summary>
    public static bool HasSearchLog(IEnumerable<string> lines) => lines.Any(l =>
    {
        var s = l.TrimStart();
        return s.StartsWith("p25s.seq=", StringComparison.Ordinal) || s.StartsWith("dmrs.seq=", StringComparison.Ordinal) ||
               s.StartsWith("alls.seq=", StringComparison.Ordinal);
    });

    /// <summary>One poll's lines (all three sections, with the reader's uptime_ms lines); true when a line was added,
    /// changed or removed.</summary>
    public bool Add(IEnumerable<string> lines, DateTime now)
    {
        var kv = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var raw in lines)
        {
            var line = raw.Trim();
            var eq = line.IndexOf('=');
            if (eq > 0)
                kv[line[..eq]] = line[(eq + 1)..];
        }
        P25StoppedAt = Stopped(P25On, P25On = Flag(kv, "p25s.on"), P25StoppedAt, now);
        DmrStoppedAt = Stopped(DmrOn, DmrOn = Flag(kv, "dmrs.on"), DmrStoppedAt, now);
        AllStoppedAt = Stopped(AllOn, AllOn = Flag(kv, "alls.on"), AllStoppedAt, now);
        P25AtHz = Num(kv, "p25s.at");
        DmrAtHz = Num(kv, "dmrs.at");
        AllAtHz = Num(kv, "alls.at");
        poll++;
        TakeStats(kv);

        var seqNow = new Dictionary<char, long>();
        if (Num(kv, "p25s.seq") is { } ps)
            seqNow['P'] = ps & 0xFFFF;
        if ((Num(kv, "dmrs.seq") ?? Num(kv, "alls.seq")) is { } ds)
            seqNow['D'] = ds & 0xFFFF;
        var up = Num(kv, "uptime_ms");
        // a restart: the radio's uptime went back (not its 49.7 day wrap), or a search's call sequence went back
        var restart = up is { } u && uptime is { } u0 && u < u0 && !(u0 > 0xF0000000L && u < 0x10000000L);
        foreach (var (eng, s) in seqNow)
            if (seqs.TryGetValue(eng, out var s0) && s != s0 && ((s - s0) & 0xFFFF) >= 0x8000)
                restart = true;
        if (restart)
        {
            entries.Clear();
            seqs.Clear();
            Restarts++;
        }
        if (up is not null)
            uptime = up;

        var changed = false;
        var delta = new Dictionary<char, int>();
        var seen = new HashSet<(char, long, string, long)>();
        var calls = new List<(char Eng, SearchCall Call)>();
        for (var i = 0; i < 8; i++)
        {
            foreach (var (eng, call) in new (char, SearchCall?)[]
                     {
                         ('P', kv.TryGetValue($"p25s.f{i}", out var p) ? ParseP25(p, now)?.Call : null),
                         ('D', kv.TryGetValue($"dmrs.f{i}", out var d) ? ParseDmr(d, now)?.Call : null),
                         ('D', kv.TryGetValue($"alls.f{i}", out var x) ? ParseAll(x, now)?.Call : null),
                     })
            {
                // the same entry twice in one poll (two entries of one serial, frequency and mode): the newest only
                if (call is not null && call.Id >= 0 && seen.Add((eng, call.Id, call.Mode, call.Hz)))
                    calls.Add((eng, call));
            }
        }
        // a counter one down is the call the radio moved to another entry when another entry of the engine rose in
        // this poll or the engine counted no new call; otherwise (its serial used again, seq moved) a new entry
        var rising = new HashSet<char>();
        foreach (var (eng, c) in calls)
            if (entries.TryGetValue((eng, c.Id, c.Mode, c.Hz), out var e) ? c.Calls > e.Calls : c.Calls > 0)
                rising.Add(eng);
        var counting = new HashSet<char>();
        foreach (var (eng, sq) in seqNow)
            if (seqs.TryGetValue(eng, out var s0) && sq != s0)
                counting.Add(eng);
        foreach (var (eng, c) in calls)
            changed |= Take(eng, c, delta, rising.Contains(eng) || !counting.Contains(eng));
        foreach (var k in entries.Where(e => poll - e.Value.Seen > ForgetPolls).Select(e => e.Key).ToList())
            entries.Remove(k);
        foreach (var (eng, s) in seqNow)
        {
            // never below 0, so it never hides a later real miss; a call the entries showed before seq had it (the
            // radio counted it between printing the two) is owed for one poll only
            if (seqs.TryGetValue(eng, out var s0))
            {
                var owed = debt.GetValueOrDefault(eng);
                var raw = balance.GetValueOrDefault(eng) + owed + (int)((s - s0) & 0xFFFF) - delta.GetValueOrDefault(eng);
                balance[eng] = Math.Max(0, raw);
                debt[eng] = raw < 0 && owed == 0 ? raw : 0;
            }
            seqs[eng] = s;
        }
        return changed;
    }

    /// <summary>The X.st lines of a poll: the latest of each engine is kept (a poll without one leaves it); the page
    /// shows the engine that runs now.</summary>
    private void TakeStats(Dictionary<string, string> kv)
    {
        if (kv.TryGetValue("p25s.st", out var p) && SearchStats.Parse(p) is { } ps)
            P25Stats = ps;
        if (kv.TryGetValue("dmrs.st", out var d) && SearchStats.Parse(d) is { } ds)
            DmrStats = ds;
        if (kv.TryGetValue("alls.st", out var a) && SearchStats.Parse(a) is { } al)
            AllStats = al;
        foreach (var (name, on, st) in new (string, bool?, SearchStats?)[]
                 {
                     ("P25", P25On, P25Stats), ("DMR", DmrOn, DmrStats), ("ALL", AllOn, AllStats),
                 })
        {
            if (st is not null && on == true)
            {
                StatsEngine = name;
                return;
            }
        }
        if (ShownStats is null)
            StatsEngine = AllStatsSeen.Select(e => e.Engine).FirstOrDefault();
    }

    /// <summary>One entry of a poll into its line; true when a line changed. A line exists only while it has calls:
    /// none for an entry without a call yet, and a line whose calls went back to 0 (its call moved to another
    /// talkgroup) goes once no entry with calls is on it.</summary>
    private bool Take(char eng, SearchCall c, Dictionary<char, int> delta, bool moveOk)
    {
        var id = (eng, c.Id, c.Mode, c.Hz);
        var key = new RowKey(c.Mode, c.Hz, c.Code, c.Group);
        // the serial used again by a new entry: its key is not a fill-in of the old one's, or its counter went down by
        // more than the one call the radio moves to another talkgroup. The old entry's calls stay on their line.
        if (!entries.TryGetValue(id, out var e) || c.Calls < e.Calls - 1 || (c.Calls == e.Calls - 1 && !moveOk) ||
            (e.Key is { } k && k != key && !FillIn(k, key)))
        {
            if (c.Calls == 0 && c.Group == 0 && c.Source == 0 && c.Caller.Length == 0)
                return false;   // nothing known yet: taken when it has something
            e = new Entry();
            entries[id] = e;
        }
        e.Seen = poll;
        e.Key = key;
        var d = c.Calls - e.Calls;
        delta[eng] = delta.GetValueOrDefault(eng) + d;
        e.Calls = c.Calls;
        var counted = Math.Max(0, e.Counted + d);
        var changed = false;
        if (e.Row is { } old && (old != key || counted == 0))
        {
            // the radio filled in the code / group, or the entry's last call moved away: it leaves its line
            if (groups.TryGetValue(old, out var og))
            {
                og.Calls -= e.Counted;
                if (e.First is null || og.First < e.First)
                    e.First = og.First;   // heard first under the half known key (or before its call moved)
                if (og.Calls <= 0 && !entries.Values.Any(o => o != e && o.Row == old && o.Counted > 0))
                {
                    groups.Remove(old);
                    foreach (var o in entries.Values.Where(o => o.Row == old))
                        o.Row = null;
                }
            }
            e.Row = null;
            e.Counted = 0;
            changed = true;
        }
        if (counted == 0)
        {
            e.Counted = 0;
            return changed;     // no line until it has a call
        }
        if (e.Row is null)
        {
            e.First ??= c.Time;     // first heard: its first call, not when it was first seen without one
            if (!groups.TryGetValue(key, out var ng))
                groups[key] = ng = new SearchGroup
                {
                    Mode = c.Mode, Hz = c.Hz, Code = c.Code, Group = c.Group, First = e.First.Value, Last = c.Time, Slot = c.Slot,
                    Source = c.Source, Caller = c.Caller, Encrypted = c.Encrypted,
                };
            else if (e.First < ng.First)
                ng.First = e.First.Value;
            ng.Calls += counted;
            e.Row = key;
            changed = true;
        }
        else if (counted != e.Counted)
        {
            groups[key].Calls += counted - e.Counted;
            changed = true;
        }
        e.Counted = counted;
        var g = groups[key];
        if (c.Time > g.Last || c.Source != g.Source || c.Caller != g.Caller || (c.Encrypted && !g.Encrypted) ||
            (c.Slot.Length > 0 && c.Slot != g.Slot))
        {
            if (c.Time > g.Last)
                g.Last = c.Time;
            if (c.Time < g.First)
                g.First = c.Time;
            g.Source = c.Source;
            g.Caller = c.Caller;
            g.Slot = c.Slot.Length > 0 ? c.Slot : g.Slot;
            g.Encrypted |= c.Encrypted;
            changed = true;
        }
        return changed;
    }

    /// <summary>A code the radio has not named yet ("CC?", "$?", "DG?", "RAN?").</summary>
    private static bool Unknown(string code) => code.EndsWith('?');

    /// <summary>The radio filled in an entry's code and / or group: the old key had an unknown field and the known
    /// ones agree (anything else is another entry).</summary>
    private static bool FillIn(RowKey old, RowKey now) =>
        old.Mode == now.Mode && old.Hz == now.Hz && (Unknown(old.Code) || old.Group == 0) &&
        (old.Code == now.Code || Unknown(old.Code)) && (old.Group == now.Group || old.Group == 0);

    private static DateTime? Stopped(bool? before, bool? now, DateTime? at, DateTime t) => now switch
    {
        true => null,
        false when before == true => t,
        _ => at,
    };

    private static bool? Flag(Dictionary<string, string> kv, string key) =>
        kv.TryGetValue(key, out var v) ? v == "1" : null;

    private static long? Num(Dictionary<string, string> kv, string key) =>
        kv.TryGetValue(key, out var v) && long.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) ? n : null;

    private static long[]? Fields(string text, int n)
    {
        var parts = text.Split(',');
        if (parts.Length < n)
            return null;
        var v = new long[n];
        for (var i = 0; i < n; i++)
            if (!long.TryParse(parts[i], NumberStyles.Integer, CultureInfo.InvariantCulture, out v[i]))
                return null;
        return v;
    }

    /// <summary>p25s.fI=HZ,NAC,TG,SRC,CALLS,AGE_S,ID</summary>
    public static (SearchCall Call, int Age)? ParseP25(string text, DateTime now)
    {
        if (Fields(text, 7) is not { } v || v[0] <= 0)
            return null;
        var age = (int)Math.Clamp(v[5], 0, 86400);
        return (new SearchCall(now.AddSeconds(-age), "P25", v[0], v[1] is >= 0 and < 0xFFFF ? Nac(v[1]) : "$?", "",
            v[2], v[3], false, (int)v[4], "", v[6]), age);
    }

    /// <summary>dmrs.fI=HZ,CC,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S,ID</summary>
    public static (SearchCall Call, int Age)? ParseDmr(string text, DateTime now)
    {
        if (Fields(text, 11) is not { } v || v[0] <= 0)
            return null;
        var age = (int)Math.Clamp(v[9], 0, 86400);
        var cc = v[1] is >= 0 and <= 15 ? $"CC{v[1]}" : "CC?";
        return (new SearchCall(now.AddSeconds(-age), "DMR", v[0], cc, Slots(v[2], v[3]), v[6], v[7], v[5] != 0, (int)v[8], "", v[10]), age);
    }

    /// <summary>DMR slots seen and repeater / direct: "TS2 RPT", "TS1+2", "DIR".</summary>
    private static string Slots(long tsMask, long syncMask)
    {
        var slot = (tsMask & 3) switch { 1 => "TS1", 2 => "TS2", 3 => "TS1+2", _ => "" };
        if ((syncMask & 1) != 0)
            slot = (slot + " RPT").Trim();
        else if ((syncMask & 2) != 0)
            slot = (slot + " DIR").Trim();
        return slot;
    }

    /// <summary>alls.fI=HZ,MODE,CODE,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S,ID,CALL (MODE P25 / DMR / YSF / NXDN;
    /// CODE the NAC, colour code, DG-ID or RAN, 65535 not known; CALL the YSF callsign, the last field).</summary>
    public static (SearchCall Call, int Age)? ParseAll(string text, DateTime now)
    {
        var parts = text.Split(',', 13);
        if (parts.Length < 12)
            return null;
        var mode = parts[1].Trim();
        if (mode is not ("P25" or "DMR" or "YSF" or "NXDN"))
            return null;
        string[] nums = [parts[0], .. parts[2..12]];
        if (Fields(string.Join(',', nums), 11) is not { } v || v[0] <= 0)
            return null;
        var age = (int)Math.Clamp(v[9], 0, 86400);
        var known = v[1] is >= 0 and < 65535;
        var code = mode switch
        {
            "P25" => known ? Nac(v[1]) : "$?",
            "DMR" => v[1] is >= 0 and <= 15 ? $"CC{v[1]}" : "CC?",
            "YSF" => known ? $"DG{v[1]}" : "DG?",
            _ => known ? $"RAN{v[1]}" : "RAN?",
        };
        var slot = mode == "DMR" ? Slots(v[2], v[3]) : "";
        var caller = parts.Length > 12 ? parts[12].Trim() : "";
        return (new SearchCall(now.AddSeconds(-age), mode, v[0], code, slot, v[6], v[7], v[5] != 0, (int)v[8], caller, v[10]), age);
    }

    /// <summary>A P25 NAC in both forms the radio shows: "$659/1625" (hex, decimal; owner 2026-11-21).</summary>
    public static string Nac(long nac) =>
        "$" + nac.ToString("X3", CultureInfo.InvariantCulture) + "/" + nac.ToString(CultureInfo.InvariantCulture);

    public static string Mhz(long hz) => (hz / 1e6).ToString("0.0000", CultureInfo.InvariantCulture);

    /// <summary>MHz with the given decimal separator (the CSV's: "433,5000" for Turkish Excel).</summary>
    public static string Mhz(long hz, string dec) => Mhz(hz).Replace(".", dec, StringComparison.Ordinal);

    public static readonly string[] CsvColumns =
        ["first_heard", "last_heard", "mode", "mhz", "code", "slot", "group", "last_source", "caller", "encrypted", "calls"];

    /// <summary>The CSV separator Excel expects here: Windows' list separator (";" in Turkish, "," in English), else ",".</summary>
    public static char ListSeparator(CultureInfo? culture = null)
    {
        var s = (culture ?? CultureInfo.CurrentCulture).TextInfo.ListSeparator;
        return s is [var c] && c is ',' or ';' or '\t' ? c : ',';
    }

    /// <summary>The CSV's separator and decimal separator for Excel here: Windows' list separator and, with it, the
    /// culture's decimal separator (";" and "," in Turkish, so 433,5000 is a number); "," and "." otherwise.</summary>
    public static (char Sep, string Dec) CsvFormat(CultureInfo? culture = null)
    {
        var c = culture ?? CultureInfo.CurrentCulture;
        var sep = ListSeparator(c);
        var dec = c.NumberFormat.NumberDecimalSeparator;
        return sep == ',' || dec is not ("," or ".") ? (sep, ".") : (sep, dec);
    }

    /// <summary>One CSV cell: text Excel would run as a formula (starting with = + - @, a tab or CR) gets a leading
    /// apostrophe; a cell holding the separator, a quote or a line break is quoted.</summary>
    public static string CsvCell(string value, char sep)
    {
        if (value.Length > 0 && value[0] is '=' or '+' or '-' or '@' or '\t' or '\r')
            value = "'" + value;
        return value.IndexOfAny([sep, '"', '\n', '\r']) >= 0 ? "\"" + value.Replace("\"", "\"\"") + "\"" : value;
    }

    /// <summary>The footer row of an engine's statistics: "stats", the engine, then key=value cells.</summary>
    public static string CsvStatsRow(string engine, SearchStats st, char sep = ',') => string.Join(sep,
        new[] { "stats", engine }.Concat(SearchStats.CsvKeys.Zip(st.CsvValues(), (k, v) => k + "=" + v))
            .Select(c => CsvCell(c, sep)));

    public static string CsvHeader(char sep = ',') => string.Join(sep, CsvColumns);

    public static string CsvRow(SearchGroup g, char sep = ',', string dec = ".") => string.Join(sep, new[]
    {
        g.First.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture),
        g.Last.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture), g.Mode, Mhz(g.Hz, dec), g.Code, g.Slot,
        g.Group.ToString(CultureInfo.InvariantCulture), g.Source.ToString(CultureInfo.InvariantCulture),
        g.Caller, g.Encrypted ? "1" : "0", g.Calls.ToString(CultureInfo.InvariantCulture),
    }.Select(c => CsvCell(c, sep)));

    /// <summary>The log's file for a session started at <paramref name="started"/>: folder/search-YYYYMMDD-HHMMSS.csv,
    /// never one that exists (a "-2", "-3" ... after the name then). One file per session: a program restart never
    /// overwrites an earlier session's file, and a run past midnight stays in the file it began in.</summary>
    public static string SessionPath(string folder, DateTime started)
    {
        var name = $"search-{started:yyyyMMdd-HHmmss}";
        var path = Path.Combine(folder, name + ".csv");
        for (var n = 2; File.Exists(path) || File.Exists(SidePath(path)); n++)
            path = Path.Combine(folder, $"{name}-{n}.csv");
        return path;
    }

    /// <summary>Where the rows go while another program (Excel) holds the log's file: name-open.csv beside it.</summary>
    public static string SidePath(string path) =>
        Path.Combine(Path.GetDirectoryName(path) ?? "", Path.GetFileNameWithoutExtension(path) + "-open.csv");

    /// <summary>Writes every line of the log to <paramref name="path"/> (UTF-8 with a BOM, so Excel reads the Turkish
    /// letters; <paramref name="sep"/> between the cells), whole, through a temporary file, so a crash never leaves half a
    /// file. While another program holds the file (Excel locks an open CSV) the rows go to <see cref="SidePath"/> and
    /// the result says so; once the file is free again it is written and the side file goes.</summary>
    public static SearchCsvResult Write(string path, IEnumerable<SearchGroup> lines, char sep = ',', string dec = ".",
        IEnumerable<(string Engine, SearchStats Stats)>? stats = null)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path) ?? ".");
        var sb = new StringBuilder().Append(CsvHeader(sep)).Append("\r\n");
        foreach (var g in lines)
            sb.Append(CsvRow(g, sep, dec)).Append("\r\n");
        foreach (var (engine, st) in stats ?? [])
            sb.Append(CsvStatsRow(engine, st, sep)).Append("\r\n");
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(true));
        var side = SidePath(path);
        try
        {
            File.Move(tmp, path, overwrite: true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            File.Move(tmp, side, overwrite: true);  // held too: the error goes to the caller
            return new SearchCsvResult(side, true);
        }
        try
        {
            if (File.Exists(side))
                File.Delete(side);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // still open in Excel: it stays, the log's own file is whole
        }
        return new SearchCsvResult(path, false);
    }
}
