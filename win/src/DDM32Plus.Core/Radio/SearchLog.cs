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

/// <summary>What <see cref="SearchLogBook.Write"/> did: the file written and whether the log's own file was held by
/// another program (Excel), so the rows went to <see cref="SearchLogBook.SidePath"/> instead.</summary>
public sealed record SearchCsvResult(string Path, bool Locked);

/// <summary>
/// The search log (firmware tools/plus_searchlog.py, docs/plus.md "Search log on the PC"): the radio's P25, DMR and all
/// digital searches keep 8 finds each; read every second over the debug link (GET_VIEW sections p25s = 28, dmrs = 29,
/// alls = 30, read only, not counted as a look). The log keeps ONE line per mode + frequency + code + group with a call
/// counter. Calls are counted by the radio's own entries (searchlog_view.c): every entry has a serial (ID) and a call
/// counter; the log follows each entry by engine (the P25 search, or the DMR search's engine that the all digital
/// search shares) + ID + frequency + mode and adds the change of its counter to the entry's line. So a call the radio
/// moves to its right talkgroup (one entry down, another up) is one call; an entry whose code / group the radio fills
/// in later moves its calls to the right line (the half known line goes); the same call in two lists is never two.
/// X.seq (the engine's calls since power on) going back, or the radio's uptime going back, is a restart: the entries
/// start anew, nothing counted is lost. Calls the radio counted on entries the log never saw (more than its 8 between
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
        public required string Mode;
        public required long Hz;
        public RowKey? Row;
        public int Calls;       // the radio's counter at the last poll
        public int Counted;     // what this log has counted of it (on Row)
        public int Seen;
    }

    private readonly Dictionary<RowKey, SearchGroup> groups = [];
    private readonly Dictionary<(char Engine, long Id), Entry> entries = [];
    private readonly Dictionary<char, long> seqs = [];
    private readonly Dictionary<char, int> balance = [];
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

    /// <summary>Radio restarts seen (uptime or a search's call sequence went back).</summary>
    public int Restarts { get; private set; }

    /// <summary>Calls the radio counted that no entry showed (it found more than 8 between two polls).</summary>
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
        }
        balance.Clear();
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
        for (var i = 0; i < 8; i++)
        {
            foreach (var (eng, call) in new (char, SearchCall?)[]
                     {
                         ('P', kv.TryGetValue($"p25s.f{i}", out var p) ? ParseP25(p, now)?.Call : null),
                         ('D', kv.TryGetValue($"dmrs.f{i}", out var d) ? ParseDmr(d, now)?.Call : null),
                         ('D', kv.TryGetValue($"alls.f{i}", out var x) ? ParseAll(x, now)?.Call : null),
                     })
            {
                if (call is null || call.Id < 0)
                    continue;
                changed |= Take(eng, call, delta);
            }
        }
        foreach (var k in entries.Where(e => poll - e.Value.Seen > ForgetPolls).Select(e => e.Key).ToList())
            entries.Remove(k);
        foreach (var (eng, s) in seqNow)
        {
            if (seqs.TryGetValue(eng, out var s0))
                balance[eng] = balance.GetValueOrDefault(eng) + (int)((s - s0) & 0xFFFF) - delta.GetValueOrDefault(eng);
            seqs[eng] = s;
        }
        return changed;
    }

    /// <summary>One entry of a poll into its line; true when a line changed.</summary>
    private bool Take(char eng, SearchCall c, Dictionary<char, int> delta)
    {
        var id = (eng, c.Id);
        var fresh = false;
        if (!entries.TryGetValue(id, out var e) || e.Mode != c.Mode || e.Hz != c.Hz)
        {
            // new (or a serial used again for another frequency): nothing of it seen before
            if (c.Calls == 0 && c.Group == 0 && c.Source == 0 && c.Caller.Length == 0)
                return false;   // nothing known yet: taken when it has something
            e = new Entry { Mode = c.Mode, Hz = c.Hz };
            entries[id] = e;
            fresh = true;
        }
        e.Seen = poll;
        var d = c.Calls - e.Calls;
        delta[eng] = delta.GetValueOrDefault(eng) + d;
        e.Calls = c.Calls;
        var counted = Math.Max(0, e.Counted + d);
        if (e.Row is null && !fresh && counted == 0)
            return false;   // the log was cleared: back on a line with its next call
        var key = new RowKey(c.Mode, c.Hz, c.Code, c.Group);
        var changed = false;
        if (e.Row != key)
        {
            // the radio filled in the code / group (or the entry is new): its calls go to the right line
            var first = c.Time;
            if (e.Row is { } old && groups.TryGetValue(old, out var og))
            {
                og.Calls -= e.Counted;
                if (og.First < first)
                    first = og.First;   // heard first under the half known key
                if (og.Calls <= 0 && !entries.Values.Any(o => o != e && o.Row == old))
                    groups.Remove(old);
            }
            if (!groups.TryGetValue(key, out var ng))
                groups[key] = ng = new SearchGroup
                {
                    Mode = c.Mode, Hz = c.Hz, Code = c.Code, Group = c.Group, First = first, Last = c.Time, Slot = c.Slot,
                    Source = c.Source, Caller = c.Caller, Encrypted = c.Encrypted,
                };
            else if (first < ng.First)
                ng.First = first;
            ng.Calls += counted;
            e.Row = key;
            changed = true;
        }
        var g = groups[key];
        if (counted != e.Counted && !changed)
        {
            g.Calls += counted - e.Counted;
            changed = true;
        }
        e.Counted = counted;
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
        return (new SearchCall(now.AddSeconds(-age), "P25", v[0], Nac(v[1]), "",
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

    public static readonly string[] CsvColumns =
        ["first_heard", "last_heard", "mode", "mhz", "code", "slot", "group", "last_source", "caller", "encrypted", "calls"];

    /// <summary>The CSV separator Excel expects here: Windows' list separator (";" in Turkish, "," in English), else ",".</summary>
    public static char ListSeparator(CultureInfo? culture = null)
    {
        var s = (culture ?? CultureInfo.CurrentCulture).TextInfo.ListSeparator;
        return s is [var c] && c is ',' or ';' or '\t' ? c : ',';
    }

    /// <summary>One CSV cell: text Excel would run as a formula (starting with = + - @, a tab or CR) gets a leading
    /// apostrophe; a cell holding the separator, a quote or a line break is quoted.</summary>
    public static string CsvCell(string value, char sep)
    {
        if (value.Length > 0 && value[0] is '=' or '+' or '-' or '@' or '\t' or '\r')
            value = "'" + value;
        return value.IndexOfAny([sep, '"', '\n', '\r']) >= 0 ? "\"" + value.Replace("\"", "\"\"") + "\"" : value;
    }

    public static string CsvHeader(char sep = ',') => string.Join(sep, CsvColumns);

    public static string CsvRow(SearchGroup g, char sep = ',') => string.Join(sep, new[]
    {
        g.First.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture),
        g.Last.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture), g.Mode, Mhz(g.Hz), g.Code, g.Slot,
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
    public static SearchCsvResult Write(string path, IEnumerable<SearchGroup> lines, char sep = ',')
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path) ?? ".");
        var sb = new StringBuilder().Append(CsvHeader(sep)).Append("\r\n");
        foreach (var g in lines)
            sb.Append(CsvRow(g, sep)).Append("\r\n");
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
