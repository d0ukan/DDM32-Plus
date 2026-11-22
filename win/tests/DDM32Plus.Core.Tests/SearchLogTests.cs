// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text;
using DDM32Plus.Core.Radio;
using Xunit;

namespace DDM32Plus.Core.Tests;

/// <summary>The search log (SearchLog.cs): the firmware's p25s / dmrs / alls sections (searchlog_view.c) become call
/// rows, counted by the radio's own entries (ID) and its call sequence (seq).</summary>
public sealed class SearchLogTests : IDisposable
{
    private readonly string dir = Path.Combine(Path.GetTempPath(), "ddm32-slog-" + Guid.NewGuid().ToString("N"));
    private static readonly DateTime T0 = new(2026, 11, 21, 22, 0, 0);

    public void Dispose()
    {
        if (Directory.Exists(dir))
            Directory.Delete(dir, true);
    }

    private static string[] Poll(int pseq, string p25 = "", int dseq = 0, string dmr = "", long uptime = 100000) =>
        ["--- view p25s (28) status 0", $"uptime_ms={uptime}", "p25s.on=1", "p25s.phase=1", "p25s.at=433512500", "p25s.n=1",
         $"p25s.seq={pseq}", p25,
         "--- view dmrs (29) status 0", $"uptime_ms={uptime + 1}", "dmrs.on=1", "dmrs.phase=1", "dmrs.at=438000000", "dmrs.n=1",
         $"dmrs.seq={dseq}", dmr];

    private static SearchGroup Line(SearchLogBook b, long group) => b.Groups.Single(g => g.Group == group);

    [Fact]
    public void FieldsAreParsed()
    {
        var p = SearchLogBook.ParseP25("433500000,456,655,1234567,3,10,7", T0)!.Value.Call;
        Assert.Equal(("P25", 433500000L, "$1C8/456", 655L, 1234567L, 3, 7L), (p.Mode, p.Hz, p.Code, p.Group, p.Source, p.Calls, p.Id));
        Assert.Equal(T0.AddSeconds(-10), p.Time);
        var d = SearchLogBook.ParseDmr("438612500,1,2,1,1,0,2620,2620123,4,0,63", T0)!.Value.Call;
        Assert.Equal(("DMR", "CC1", "TS2 RPT", 2620L, 2620123L, false, 63L), (d.Mode, d.Code, d.Slot, d.Group, d.Source, d.Encrypted, d.Id));
        Assert.Equal("CC?", SearchLogBook.ParseDmr("438612500,255,0,2,1,1,0,0,0,0,1", T0)!.Value.Call.Code);
        Assert.Null(SearchLogBook.ParseP25("garbage", T0));
        Assert.Null(SearchLogBook.ParseDmr("1,2,3", T0));
        // the format before the entry serial (an internal build): not read
        Assert.Null(SearchLogBook.ParseP25("433500000,456,655,1234567,3,10", T0));
        Assert.Null(SearchLogBook.ParseDmr("438612500,1,2,1,1,0,2620,2620123,4,0", T0));
    }

    [Fact]
    public void OneLinePerFrequencyCodeAndGroupWithACounter()
    {
        var b = new SearchLogBook();
        // first poll: what the radio already found
        Assert.True(b.Add(Poll(1, "p25s.f0=433500000,456,655,1234567,1,30,1", 2, "dmrs.f0=438612500,1,1,1,1,0,2620,2620123,2,5,1"), T0));
        Assert.Equal(2, b.Groups.Count);
        Assert.Equal("DMR", b.Groups[0].Mode);	// heard last comes first
        Assert.Equal((1, 2), (b.Groups[1].Calls, b.Groups[0].Calls));
        Assert.True(b.P25On);
        Assert.Equal(433512500L, b.P25AtHz);
        // the same counters again: nothing changes
        Assert.False(b.Add(Poll(1, "p25s.f0=433500000,456,655,1234567,1,31,1", 2, "dmrs.f0=438612500,1,1,1,1,0,2620,2620123,2,6,1"), T0.AddSeconds(1)));
        // a new call on the same frequency, NAC and group: the same line, counted, the last source kept
        Assert.True(b.Add(Poll(2, "p25s.f0=433500000,456,655,7654321,2,0,1", 2), T0.AddSeconds(2)));
        var p = b.Groups.Single(g => g.Mode == "P25");
        Assert.Equal((2, 7654321L, T0.AddSeconds(-30), T0.AddSeconds(2)), (p.Calls, p.Source, p.First, p.Last));
        // another talkgroup on that frequency: its own line (another entry)
        Assert.True(b.Add(Poll(5, "p25s.f0=433500000,456,101,1111,3,0,2", 2), T0.AddSeconds(3)));
        Assert.Equal(3, b.Groups.Count);
        Assert.Equal(2, b.Frequencies);
        Assert.Equal(3, Line(b, 101).Calls);
        Assert.Equal(2 + 2 + 3, b.TotalCalls);
        Assert.Equal(0, b.Missed);
        // no answer: the searches are unknown, nothing changes
        Assert.False(b.Add([], T0.AddSeconds(11)));
        Assert.Null(b.DmrOn);
    }

    [Fact]
    public void ACallTheRadioMovesIsOneCall()
    {
        // A2: the radio counts a call on TG 2 (link control late), then moves it to TG 1: 6 then 5 is 5, not 11
        var b = new SearchLogBook();
        b.Add(Poll(9, "p25s.f0=433500000,456,2,5,5,0,3"), T0);
        Assert.Equal(5, Line(b, 2).Calls);
        b.Add(Poll(10, "p25s.f0=433500000,456,2,5,6,0,3"), T0.AddSeconds(1));
        Assert.Equal(6, Line(b, 2).Calls);
        Assert.True(b.Add([.. Poll(10, "p25s.f0=433500000,456,1,9,1,0,4"), "p25s.f1=433500000,456,2,5,5,1,3"], T0.AddSeconds(2)));
        Assert.Equal((5, 1), (Line(b, 2).Calls, Line(b, 1).Calls));
        Assert.Equal(6, b.TotalCalls);
        Assert.Equal(0, b.Missed);
    }

    [Fact]
    public void AnEntryWhoseGroupIsFilledInMovesItsLine()
    {
        // A2: the radio makes the entry before the talkgroup is known and fills it in: no bogus "-" line, no double count
        var b = new SearchLogBook();
        Assert.True(b.Add(Poll(1, "p25s.f0=433500000,456,0,0,1,0,8"), T0));
        Assert.Equal(0L, b.Groups.Single().Group);
        Assert.True(b.Add(Poll(1, "p25s.f0=433500000,456,655,1234,1,0,8"), T0.AddSeconds(1)));
        var g = b.Groups.Single();
        Assert.Equal((655L, 1, 1234L), (g.Group, g.Calls, g.Source));
        Assert.Equal(T0, g.First);
        // the same for a DMR entry with its color code still unknown
        Assert.True(b.Add(Poll(1, "", 1, "dmrs.f0=438612500,255,0,1,1,0,0,0,1,0,5"), T0.AddSeconds(2)));
        Assert.True(b.Add(Poll(1, "", 1, "dmrs.f0=438612500,3,2,1,1,0,91,2860001,1,0,5"), T0.AddSeconds(3)));
        var d = b.Groups.Single(x => x.Mode == "DMR");
        Assert.Equal(("CC3", 91L, 1), (d.Code, d.Group, d.Calls));
        Assert.Equal(2, b.Groups.Count);
    }

    [Fact]
    public void ARadioRestartLosesNothingAndCountsNothingTwice()
    {
        var b = new SearchLogBook();
        b.Add(Poll(4, "p25s.f0=433500000,456,101,1111,4,0,12", uptime: 500000), T0);
        Assert.Equal(4, Line(b, 101).Calls);
        // the radio restarted: uptime and seq back, serials anew (the new entry 12 is not the old entry 12)
        Assert.True(b.Add(Poll(1, "p25s.f0=433500000,456,101,2222,1,0,12", uptime: 3000), T0.AddSeconds(10)));
        Assert.Equal(5, Line(b, 101).Calls);
        Assert.Equal(1, b.Restarts);
        Assert.False(b.Add(Poll(1, "p25s.f0=433500000,456,101,2222,1,1,12", uptime: 4000), T0.AddSeconds(11)));
        Assert.Equal(5, Line(b, 101).Calls);
        // uptime wrapping after 49.7 days is not a restart; seq wrapping at 65536 is not either
        Assert.False(b.Add(Poll(1, "p25s.f0=433500000,456,101,2222,1,2,12", uptime: 0xFFFFF000L), T0.AddSeconds(12)));
        Assert.False(b.Add(Poll(1, "p25s.f0=433500000,456,101,2222,1,3,12", uptime: 1000), T0.AddSeconds(13)));
        Assert.Equal((1, 5), (b.Restarts, Line(b, 101).Calls));
        var w = new SearchLogBook();
        w.Add(Poll(65535, "p25s.f0=433500000,456,101,1,7,0,9"), T0);
        w.Add(Poll(1, "p25s.f0=433500000,456,101,1,9,0,9"), T0.AddSeconds(1));
        Assert.Equal((0, 9, 0), (w.Restarts, Line(w, 101).Calls, w.Missed));
    }

    [Fact]
    public void BothListsAndASecondSearchAreNotCountedTwice()
    {
        // the all digital search shares the DMR search's engine: its entries are the same serials, a new search of the
        // other kind lists new entries; the P25 search's list stays (its own engine)
        var b = new SearchLogBook();
        Assert.True(b.Add(["p25s.on=0", "p25s.seq=2", "p25s.f0=433500000,456,655,1234567,2,30,1",
                           "alls.on=1", "alls.mode=P25", "alls.at=433512500", "alls.n=2", "alls.seq=2",
                           "alls.f0=433500000,P25,456,0,0,1,0,655,1234567,1,0,1,",
                           "alls.f1=439100000,YSF,0,0,0,0,0,0,0,1,4,2,TA2DOU"], T0));
        Assert.Equal(2, b.Groups.Count);
        Assert.True(b.AllOn);
        Assert.False(b.P25On);
        Assert.Equal(433512500L, b.AllAtHz);
        // two searches found the same key: two different sets of calls, added
        Assert.Equal(3, b.Groups.Single(g => g.Mode == "P25").Calls);
        Assert.Equal("TA2DOU", b.Groups.Single(g => g.Mode == "YSF").Caller);
        // the same again: nothing; another YSF caller on the YSF entry: the same line, the caller updated
        Assert.False(b.Add(["p25s.seq=2", "p25s.f0=433500000,456,655,1234567,2,31,1", "alls.on=1", "alls.seq=2",
                            "alls.f0=433500000,P25,456,0,0,1,0,655,1234567,1,1,1,", "alls.f1=439100000,YSF,0,0,0,0,0,0,0,1,5,2,TA2DOU"],
            T0.AddSeconds(1)));
        Assert.True(b.Add(["alls.on=1", "alls.seq=3", "alls.f0=439100000,YSF,0,0,0,0,0,0,0,2,0,2,YM2KDZ"], T0.AddSeconds(2)));
        var y = b.Groups.Single(g => g.Mode == "YSF");
        Assert.Equal(("YM2KDZ", 2), (y.Caller, y.Calls));
        // the DMR search starts: the all digital list is gone, the DMR list is new entries (no counter read as a drop)
        Assert.True(b.Add(["alls.on=0", "alls.seq=4", "dmrs.on=1", "dmrs.seq=4", "dmrs.f0=438612500,1,2,1,1,0,2620,2620123,1,0,3"],
            T0.AddSeconds(3)));
        Assert.Equal((3, 2, 1), (b.Groups.Single(g => g.Mode == "P25").Calls, y.Calls, b.Groups.Single(g => g.Mode == "DMR").Calls));
        Assert.Equal(T0.AddSeconds(3), b.AllStoppedAt);
        Assert.Null(b.DmrStoppedAt);
        Assert.Equal(0, b.Missed);
    }

    [Fact]
    public void TwoLiveEntriesWithOneSerialAreCountedApart()
    {
        // verifier: a hot DMR entry survives while its serial comes round again on another frequency / mode; read 6
        // times, the radio's 4 and 3 stay 4 and 3 (not 24 and 12)
        var b = new SearchLogBook();
        for (var i = 0; i < 6; i++)
            b.Add(["dmrs.on=1", "dmrs.seq=7", "dmrs.f0=438612500,1,2,1,1,0,91,1,4,0,5",
                   "dmrs.f1=439000000,2,2,1,1,0,92,2,3,0,5"], T0.AddSeconds(i));
        Assert.Equal((4, 3, 7, 0), (Line(b, 91).Calls, Line(b, 92).Calls, b.TotalCalls, b.Missed));
        // the same entry twice in one poll: counted once
        var t = new SearchLogBook();
        t.Add(["dmrs.seq=2", "dmrs.f0=438612500,1,2,1,1,0,91,1,2,0,9", "dmrs.f1=438612500,1,2,1,1,0,91,1,2,0,9"], T0);
        Assert.Equal(2, t.TotalCalls);
    }

    [Fact]
    public void ASerialUsedAgainIsANewEntry()
    {
        // TG 91 with 4 calls; the same serial on the same frequency then names TG 92 (another entry): 4 and 1
        var b = new SearchLogBook();
        b.Add(Poll(0, "", 4, "dmrs.f0=438612500,1,2,1,1,0,91,1,4,0,5"), T0);
        Assert.True(b.Add(Poll(0, "", 5, "dmrs.f0=438612500,1,2,1,1,0,92,2,1,0,5"), T0.AddSeconds(1)));
        Assert.Equal((4, 1, 0), (Line(b, 91).Calls, Line(b, 92).Calls, b.Missed));
        // the same key, the counter 5 then 1: a new entry with one call, the 5 kept
        var s = new SearchLogBook();
        s.Add(Poll(5, "p25s.f0=433500000,456,655,1,5,0,7"), T0);
        s.Add(Poll(6, "p25s.f0=433500000,456,655,1,1,0,7"), T0.AddSeconds(1));
        Assert.Equal((6, 0), (Line(s, 655).Calls, s.Missed));
    }

    [Fact]
    public void ALineNeedsACall()
    {
        // an entry first seen without a call (its talkgroup known): no line, no frequency, nothing in the CSV
        var b = new SearchLogBook();
        Assert.False(b.Add(Poll(0, "", 0, "dmrs.f0=438612500,1,2,1,1,0,91,1,0,0,5"), T0));
        Assert.Equal((0, 0, 0), (b.Groups.Count, b.Frequencies, b.TotalCalls));
        Assert.Single(SearchLogBook.Write(Path.Combine(dir, "a.csv"), b.Groups) is var r ? File.ReadAllLines(r.Path) : []);
        // its first call an hour later: the line, first heard at that call (not when the entry was first seen)
        Assert.False(b.Add(Poll(0, "", 0, "dmrs.f0=438612500,1,2,1,1,0,91,1,0,1800,5"), T0.AddSeconds(1800)));
        Assert.True(b.Add(Poll(0, "", 1, "dmrs.f0=438612500,1,2,1,1,0,91,1,1,0,5"), T0.AddSeconds(3600)));
        Assert.Equal((1, 1, 1, T0.AddSeconds(3600)), (b.Groups.Count, b.Frequencies, b.TotalCalls, Line(b, 91).First));
    }

    [Fact]
    public void ALineMadeAgainAfterClearIsFirstHeardAtItsNextCall()
    {
        var b = new SearchLogBook();
        b.Add(Poll(1, "p25s.f0=433500000,456,655,1,1,0,1"), T0);
        b.Clear();
        b.Add(Poll(2, "p25s.f0=433500000,456,655,1,2,0,1"), T0.AddSeconds(600));
        Assert.Equal((1, T0.AddSeconds(600)), (Line(b, 655).Calls, Line(b, 655).First));
    }

    [Fact]
    public void ALineWhoseLastCallMovedGoes()
    {
        // the radio counts a call on TG 2 (a new entry), then moves it to TG 1: no TG 2 line left at 0
        var b = new SearchLogBook();
        b.Add(Poll(1, "p25s.f0=433500000,456,2,5,1,0,3"), T0);
        Assert.True(b.Add([.. Poll(1, "p25s.f0=433500000,456,1,9,1,0,4"), "p25s.f1=433500000,456,2,5,0,1,3"], T0.AddSeconds(1)));
        Assert.Equal((1, 1, 1, 0), (b.Groups.Count, Line(b, 1).Calls, b.Frequencies, b.Missed));
        // a later call on TG 2 brings its line back
        b.Add([.. Poll(2, "p25s.f0=433500000,456,2,5,1,0,3"), "p25s.f1=433500000,456,1,9,1,9,4"], T0.AddSeconds(20));
        Assert.Equal((1, 1, 2), (Line(b, 1).Calls, Line(b, 2).Calls, b.TotalCalls));
        Assert.Equal(T0, Line(b, 2).First);	// made again by the same entry: first heard at its first call
    }

    [Fact]
    public void ACounterOneDownIsAMoveOnlyWithoutANewCall()
    {
        // one down while the engine counted a call and no entry rose: the serial used again (the 2 stay, 1 more)
        var b = new SearchLogBook();
        b.Add(Poll(2, "p25s.f0=433500000,456,655,1,2,0,7"), T0);
        b.Add(Poll(3, "p25s.f0=433500000,456,655,1,1,0,7"), T0.AddSeconds(1));
        Assert.Equal((3, 0), (Line(b, 655).Calls, b.Missed));
        // one down with no new call (the moved call's other entry read in the next poll): a move, nothing doubled
        var m = new SearchLogBook();
        m.Add(Poll(6, "p25s.f0=433500000,456,2,5,6,0,3"), T0);
        m.Add(Poll(6, "p25s.f0=433500000,456,2,5,5,0,3"), T0.AddSeconds(1));
        m.Add([.. Poll(6, "p25s.f0=433500000,456,1,9,1,0,4"), "p25s.f1=433500000,456,2,5,5,1,3"], T0.AddSeconds(2));
        Assert.Equal((5, 1, 6, 0), (Line(m, 2).Calls, Line(m, 1).Calls, m.TotalCalls, m.Missed));
    }

    [Fact]
    public void MissedNeverGoesBelowZero()
    {
        var b = new SearchLogBook();
        b.Add(Poll(3, "p25s.f0=433500000,456,655,1,3,0,1"), T0);
        // a call the entries show before seq has it: the balance stays 0, a later real miss still shows
        b.Add(Poll(3, "p25s.f0=433500000,456,655,1,4,0,1"), T0.AddSeconds(1));
        b.Add(Poll(6, "p25s.f0=433500000,456,655,1,5,0,1"), T0.AddSeconds(2));
        Assert.Equal((5, 1), (b.TotalCalls, b.Missed));
    }

    [Fact]
    public void CallsNoEntryShowedAreToldAsMissed()
    {
        var b = new SearchLogBook();
        b.Add(Poll(3, "p25s.f0=433500000,456,655,1,3,0,1"), T0);
        // 4 calls counted, one shown: 3 were on entries that came and went between the polls
        b.Add(Poll(7, "p25s.f0=433500000,456,655,1,4,0,1"), T0.AddSeconds(1));
        Assert.Equal((4, 3), (b.TotalCalls, b.Missed));
    }

    [Fact]
    public void AClearedLogCountsOnlyNewCalls()
    {
        var b = new SearchLogBook();
        b.Add(Poll(3, "p25s.f0=433500000,456,655,1,3,0,1"), T0);
        b.Clear();
        Assert.Empty(b.Groups);
        Assert.False(b.Add(Poll(3, "p25s.f0=433500000,456,655,1,3,1,1"), T0.AddSeconds(1)));
        Assert.Empty(b.Groups);
        Assert.True(b.Add(Poll(4, "p25s.f0=433500000,456,655,1,4,0,1"), T0.AddSeconds(2)));
        Assert.Equal(1, b.TotalCalls);
    }

    [Fact]
    public void TheSessionsCsvIsTheLogItself()
    {
        var b = new SearchLogBook();
        b.Add(Poll(1, "p25s.f0=433500000,456,655,1234567,1,0,1"), T0);
        var path = SearchLogBook.SessionPath(dir, T0);
        Assert.Equal(Path.Combine(dir, "search-20261121-220000.csv"), path);
        Assert.Equal(new SearchCsvResult(path, false), SearchLogBook.Write(path, b.Groups));
        b.Add(Poll(2, "p25s.f0=433500000,456,655,7654321,2,0,1"), T0.AddSeconds(5));
        Assert.Equal(new SearchCsvResult(path, false), SearchLogBook.Write(path, b.Groups));
        var bytes = File.ReadAllBytes(path);
        Assert.Equal(new byte[] { 0xEF, 0xBB, 0xBF }, bytes[..3]);	// a BOM: Excel reads the Turkish letters
        var lines = File.ReadAllLines(path, Encoding.UTF8);
        Assert.Equal(SearchLogBook.CsvHeader(), lines[0]);
        Assert.Equal(2, lines.Length);	// one line, not one per call
        Assert.Equal("2026-11-21 22:00:00,2026-11-21 22:00:05,P25,433.5000,$1C8/456,,655,7654321,,0,2", lines[1]);
        Assert.False(File.Exists(path + ".tmp"));
        // a program restart: a new session never takes an earlier session's file, even in the same second
        Assert.Equal(Path.Combine(dir, "search-20261121-220000-2.csv"), SearchLogBook.SessionPath(dir, T0));
    }

    [Fact]
    public void CsvCellsFitExcel()
    {
        Assert.Equal(';', SearchLogBook.ListSeparator(CultureInfo.GetCultureInfo("tr-TR")));
        Assert.Equal(',', SearchLogBook.ListSeparator(CultureInfo.InvariantCulture));
        Assert.Equal("'-TA1ABC", SearchLogBook.CsvCell("-TA1ABC", ','));
        Assert.Equal("'=HYPERLINK(1)", SearchLogBook.CsvCell("=HYPERLINK(1)", ';'));
        Assert.Equal("'+1", SearchLogBook.CsvCell("+1", ','));
        Assert.Equal("'@x", SearchLogBook.CsvCell("@x", ','));
        Assert.Equal("\"a;b\"", SearchLogBook.CsvCell("a;b", ';'));
        Assert.Equal("a,b", SearchLogBook.CsvCell("a,b", ';'));
        Assert.Equal("\"say \"\"hi\"\"\"", SearchLogBook.CsvCell("say \"hi\"", ','));
        Assert.Equal("$1C8/456", SearchLogBook.CsvCell("$1C8/456", ','));
        var g = new SearchGroup { Mode = "YSF", Hz = 439100000, Code = "DG0", Group = 0, First = T0, Last = T0, Caller = "=CMD|x", Calls = 2 };
        Assert.Equal("2026-11-21 22:00:00;2026-11-21 22:00:00;YSF;439.1000;DG0;;0;0;'=CMD|x;0;2", SearchLogBook.CsvRow(g, ';'));
        // Turkish Excel: ";" between the cells and the frequency with a decimal comma, so it reads as a number
        var tr = SearchLogBook.CsvFormat(CultureInfo.GetCultureInfo("tr-TR"));
        Assert.Equal((';', ","), tr);
        Assert.Equal((',', "."), SearchLogBook.CsvFormat(CultureInfo.GetCultureInfo("en-US")));
        Assert.Equal("2026-11-21 22:00:00;2026-11-21 22:00:00;YSF;439,1000;DG0;;0;0;'=CMD|x;0;2", SearchLogBook.CsvRow(g, tr.Sep, tr.Dec));
        Assert.Equal("\"439,1000\"", SearchLogBook.CsvCell(SearchLogBook.Mhz(439100000, ","), ','));
    }

    [Fact]
    public void AnOpenCsvGoesToASideFileUntilItIsFree()
    {
        var b = new SearchLogBook();
        b.Add(Poll(1, "p25s.f0=433500000,456,655,1,1,0,1"), T0);
        var path = SearchLogBook.SessionPath(dir, T0);
        SearchLogBook.Write(path, b.Groups);
        SearchCsvResult held;
        using (new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))	// Excel holds it
        {
            b.Add(Poll(2, "p25s.f0=433500000,456,655,1,2,0,1"), T0.AddSeconds(1));
            held = SearchLogBook.Write(path, b.Groups);
        }
        Assert.Equal(new SearchCsvResult(SearchLogBook.SidePath(path), true), held);
        Assert.EndsWith("-open.csv", held.Path, StringComparison.Ordinal);
        Assert.Contains(File.ReadAllLines(held.Path), l => l.EndsWith(",2", StringComparison.Ordinal));
        // closed again: the log's own file is written whole and the side file goes
        Assert.Equal(new SearchCsvResult(path, false), SearchLogBook.Write(path, b.Groups));
        Assert.False(File.Exists(held.Path));
        Assert.Contains(File.ReadAllLines(path), l => l.EndsWith(",2", StringComparison.Ordinal));
    }

    [Fact]
    public void TheReaderSendsOnlyItsThreeViews()
    {
        Assert.Equal(3, DebugReaders.SearchLogAllowed.Count);
        Assert.Contains(((byte)0x09, (byte)28), DebugReaders.SearchLogAllowed);
        Assert.Contains(((byte)0x09, (byte)29), DebugReaders.SearchLogAllowed);
        Assert.Contains(((byte)0x09, (byte)30), DebugReaders.SearchLogAllowed);
        Assert.Contains(AppInfo.LogsFolder, AppInfo.ReservedNames);
    }

    [Fact]
    public void AllDigitalFieldsAreParsed()
    {
        var p = SearchLogBook.ParseAll("433500000,P25,456,0,0,1,0,655,1234567,3,10,4,", T0)!.Value.Call;
        Assert.Equal(("P25", 433500000L, "$1C8/456", "", 655L, 1234567L, 3, "", 4L), (p.Mode, p.Hz, p.Code, p.Slot, p.Group, p.Source, p.Calls, p.Caller, p.Id));
        Assert.Equal(T0.AddSeconds(-10), p.Time);
        var d = SearchLogBook.ParseAll("438612500,DMR,1,2,1,1,0,2620,2620123,4,0,5,", T0)!.Value.Call;
        Assert.Equal(("DMR", "CC1", "TS2 RPT", 2620L, 2620123L), (d.Mode, d.Code, d.Slot, d.Group, d.Source));
        var y = SearchLogBook.ParseAll("439100000,YSF,0,0,0,0,0,0,0,2,5,6,TA2DOU/P", T0)!.Value.Call;
        Assert.Equal(("YSF", "DG0", "", 0L, 0L, 2, "TA2DOU/P"), (y.Mode, y.Code, y.Slot, y.Group, y.Source, y.Calls, y.Caller));
        var n = SearchLogBook.ParseAll("438800000,NXDN,5,0,0,1,1,101,1234,1,0,7,", T0)!.Value.Call;
        Assert.Equal(("NXDN", "RAN5", 101L, 1234L, true), (n.Mode, n.Code, n.Group, n.Source, n.Encrypted));
        Assert.Equal("RAN?", SearchLogBook.ParseAll("438800000,NXDN,65535,0,0,0,0,0,0,0,0,1,", T0)!.Value.Call.Code);
        Assert.Equal("$?", SearchLogBook.ParseAll("438800000,P25,65535,0,0,0,0,0,0,0,0,1,", T0)!.Value.Call.Code);
        Assert.Equal("", SearchLogBook.ParseAll("438800000,YSF,0,0,0,0,0,0,0,0,0,1", T0)!.Value.Call.Caller);	// no CALL field
        Assert.Null(SearchLogBook.ParseAll("438800000,FM,0,0,0,0,0,0,0,0,0,1,", T0));
        Assert.Null(SearchLogBook.ParseAll("438800000,P25,1,2", T0));
        Assert.Null(SearchLogBook.ParseAll("0,P25,0,0,0,0,0,0,0,0,0,1,", T0));
    }

    [Fact]
    public void FirmwareWithoutTheSearchLogIsTold()
    {
        Assert.True(SearchLogBook.HasSearchLog(["uptime_ms=5", "p25s.on=0", "p25s.seq=0"]));
        Assert.False(SearchLogBook.HasSearchLog(["uptime_ms=5", "p25s.on=0", "p25s.n=0"]));	// before the serials
        Assert.False(SearchLogBook.HasSearchLog(["p25s.built=0", "dmrs.built=0", "alls.built=0"]));
        Assert.False(SearchLogBook.HasSearchLog([]));
    }

    // ---- search statistics (firmware beta.9: the X.st line) ----

    private const string St = "fast,60000,4,4100,161,12,3,7,1,1,4,2,3,3800,6200,6200,1400,420,810";

    private static string[] WithSt(string p25St, string dmrSt = "", bool dmrOn = true)
    {
        var l = Poll(1, "p25s.f0=433500000,456,655,1234567,1,30,1", 0, "").ToList();
        if (p25St.Length > 0)
            l.Insert(l.IndexOf("p25s.seq=1") + 1, "p25s.st=" + p25St);
        if (dmrSt.Length > 0)
            l.Add("dmrs.st=" + dmrSt);
        if (!dmrOn)
            l[l.IndexOf("dmrs.on=1")] = "dmrs.on=0";
        return [.. l];
    }

    private static Dictionary<string, string> Resx(string name)
    {
        var d = new DirectoryInfo(AppContext.BaseDirectory);
        while (d is not null && !File.Exists(Path.Combine(d.FullName, "DDM32Plus.sln")))
            d = d.Parent;
        return System.Xml.Linq.XDocument.Load(Path.Combine(d!.FullName, "src", "DDM32Plus", "Resources", name)).Root!
            .Elements("data").ToDictionary(e => (string)e.Attribute("name")!, e => (string)e.Element("value")!);
    }

    [Fact]
    public void StatsLineIsParsedTolerantly()
    {
        var s = SearchStats.Parse(St)!;
        Assert.Equal(("fast", 60000L, 4L, 4100L, 161L, 12L), (s.Algo, s.UpMs, s.Passes, s.PassMs, s.PassSteps, s.Probes));
        Assert.Equal((3L, 7L, 1L, 1L, 4L, 2L, 3L), (s.Found, s.NoSync, s.Partial, s.Gone, s.Skipped, s.Early, s.Watch));
        Assert.Equal((3800L, 6200L, 6200L, 1400L, 420L, 810L), (s.ProbeMs, s.SweepMs, s.FirstMs, s.SeenMs, s.SyncLast, s.SyncMax));
        Assert.Equal(38L, s.ProbingPercent);
        Assert.Equal("classic", SearchStats.Parse(St.Replace("fast", "Classic"))!.Algo);
        Assert.NotNull(SearchStats.Parse(St + ",99,extra"));          // extra trailing fields: the first 19 count
        Assert.Null(SearchStats.Parse(St[..St.LastIndexOf(',')]));    // 18 fields
        Assert.Null(SearchStats.Parse(St.Replace("4100", "x")));      // not a number
        Assert.Null(SearchStats.Parse(St.Replace("4100", "-1")));
        Assert.Null(SearchStats.Parse(""));
        Assert.Null(SearchStats.Parse("garbage"));
        Assert.Null(SearchStats.Parse(St.Replace("fast", "")));
    }

    [Fact]
    public void StatsLineTextInBothLanguages()
    {
        var s = SearchStats.Parse(St)!;
        var en = Resx("Strings.resx");
        var tr = Resx("Strings.tr.resx");
        Assert.Equal("Fast | pass 4.1 s (161 steps) | probes 12: found 3, no sync 7, partial 1, gone 1 | skipped 4, early 2, " +
                     "watch 3 | probing 38% | first find 6.2 s | seen->found 1.4 s | sync 0.42 s (max 0.81 s)",
            s.Describe(k => en[k], CultureInfo.InvariantCulture));
        Assert.Equal("Klasik | tur 4,1 sn (161 adım) | deneme 12: bulundu 3, senk yok 7, kısmi 1, kayboldu 1 | atlandı 4, erken 2, " +
                     "izleme 3 | deneme payı %38 | ilk bulma 6,2 sn | görülme->bulma 1,4 sn | senk 0,42 sn (en çok 0,81 sn)",
            (s with { Algo = "classic" }).Describe(k => tr[k], new CultureInfo("tr-TR")));
        // nothing known yet: the parts without a value are left out
        var none = SearchStats.Parse("fast,500,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0")!;
        Assert.Equal("Fast | probes 0: found 0, no sync 0, partial 0, gone 0 | skipped 0, early 0, watch 0",
            none.Describe(k => en[k], CultureInfo.InvariantCulture));
    }

    [Fact]
    public void BookKeepsTheLatestStatsPerEngineAndShowsTheRunningOne()
    {
        var b = new SearchLogBook();
        Assert.Null(b.ShownStats);
        b.Add(WithSt(St, "classic,1000,1,2000,10,1,0,0,1,0,0,0,0,10,20,0,0,0,0"), T0);
        Assert.Equal("P25", b.StatsEngine);              // both run: P25 first
        Assert.Equal("fast", b.P25Stats!.Algo);
        Assert.Equal("classic", b.DmrStats!.Algo);
        b.Add(WithSt("", "classic,2000,1,2000,10,1,0,0,1,0,0,0,0,10,20,0,0,0,0"), T0.AddSeconds(1));
        Assert.Equal(60000L, b.P25Stats!.UpMs);          // kept when a poll sends none
        Assert.Equal(2000L, b.DmrStats!.UpMs);
        // P25 stopped (its line stays), DMR runs: DMR is shown
        var l = WithSt(St, "classic,3000,1,2000,10,1,0,0,1,0,0,0,0,10,20,0,0,0,0").ToList();
        l[l.IndexOf("p25s.on=1")] = "p25s.on=0";
        b.Add(l, T0.AddSeconds(2));
        Assert.Equal("DMR", b.StatsEngine);
        // both stopped: the last shown stays
        l[l.IndexOf("dmrs.on=1")] = "dmrs.on=0";
        b.Add(l, T0.AddSeconds(3));
        Assert.Equal("DMR", b.StatsEngine);
        Assert.Equal(["P25", "DMR"], b.AllStatsSeen.Select(e => e.Engine));
        // garbage in the line changes nothing
        b.Add(WithSt("fast,1,2"), T0.AddSeconds(4));
        Assert.Equal(60000L, b.P25Stats!.UpMs);
    }

    [Fact]
    public void CsvFooterHasOneStatsRowPerEngineAndOnlyWithStats()
    {
        var b = new SearchLogBook();
        b.Add(WithSt(St, "classic,1000,1,2000,10,1,0,0,1,0,0,0,0,10,20,0,0,0,0"), T0);
        var path = Path.Combine(dir, "s.csv");
        SearchLogBook.Write(path, b.Groups, ',', ".", b.AllStatsSeen);
        var lines = File.ReadAllLines(path, Encoding.UTF8);
        Assert.Equal(4, lines.Length);                       // header, one data row, two stats rows
        Assert.Equal("stats,P25,algo=fast,up_ms=60000,passes=4,pass_ms=4100,pass_steps=161,probes=12,found=3,nosync=7," +
                     "partial=1,gone=1,skipped=4,early=2,watch=3,probe_ms=3800,sweep_ms=6200,first_ms=6200,seen_ms=1400," +
                     "sync_last=420,sync_max=810", lines[2].TrimStart('﻿'));
        Assert.StartsWith("stats,DMR,algo=classic,up_ms=1000,", lines[3]);
        Assert.Equal(SearchLogBook.CsvRow(b.Groups[0]), lines[1]);   // data rows unchanged
        // Turkish Excel: the list separator between the cells, same quoting rules
        Assert.StartsWith("stats;P25;algo=fast;up_ms=60000;", SearchLogBook.CsvStatsRow("P25", b.P25Stats!, ';'));
        // no stats (an older firmware): exactly the file as before
        var old = new SearchLogBook();
        old.Add(Poll(1, "p25s.f0=433500000,456,655,1234567,1,30,1"), T0);
        Assert.Null(old.ShownStats);
        Assert.Empty(old.AllStatsSeen);
        var p2 = Path.Combine(dir, "old.csv");
        SearchLogBook.Write(p2, old.Groups, ',', ".", old.AllStatsSeen);
        var p3 = Path.Combine(dir, "old2.csv");
        SearchLogBook.Write(p3, old.Groups);
        Assert.Equal(File.ReadAllBytes(p3), File.ReadAllBytes(p2));
        Assert.Equal(2, File.ReadAllLines(p2).Length);
    }

    [Fact]
    public void StatsDoNotChangeTheLogItself()
    {
        var with = new SearchLogBook();
        var without = new SearchLogBook();
        for (var i = 0; i < 4; i++)
        {
            var line = $"p25s.f0=433500000,456,655,1234567,{i + 1},0,1";
            var a = Poll(i + 1, line).ToList();
            a.Insert(a.IndexOf($"p25s.seq={i + 1}") + 1, "p25s.st=" + St);
            Assert.Equal(without.Add(Poll(i + 1, line), T0.AddSeconds(i)), with.Add(a, T0.AddSeconds(i)));
            Assert.Equal(SearchLogBook.HasSearchLog(Poll(i + 1, line)), SearchLogBook.HasSearchLog(a));
        }
        Assert.Equal((without.TotalCalls, without.Missed, without.Restarts), (with.TotalCalls, with.Missed, with.Restarts));
        Assert.Equal(without.Groups.Select(g => g.Calls), with.Groups.Select(g => g.Calls));
    }
}

/// <summary>The search log reader (DebugReaders.SearchLog) on a fake radio: an older firmware is told, a silent radio
/// or a port that went away is waited for with --reconnect.</summary>
public sealed class SearchLogReaderTests
{
    /// <summary>A fake radio answering GET_VIEW with a fixed text (per section), or going away / silent on cue.</summary>
    private sealed class FakeSearchRadio(Func<byte, string?> view) : IRawSerial
    {
        private readonly Queue<byte> pending = new();
        public int Opens { get; private set; }
        public int Closes { get; private set; }
        public bool Gone { get; set; }
        public bool Silent { get; set; }
        public int Requests { get; private set; }
        public double Timeout { get; set; }
        public int BaudRate { get; set; }

        public void Open()
        {
            if (Gone)
                throw new FileNotFoundException("Could not find file 'COM14'");
            Opens++;
        }

        public void Close() => Closes++;
        public void ResetInputBuffer() => pending.Clear();
        public void Flush() { }

        public int? Write(byte[] data)
        {
            if (Gone)
                throw new IOException("The port is closed");
            Requests++;
            if (Silent)
                return data.Length;
            if (view(data[6]) is not { } text)
            {
                // a firmware without the section: "bad argument"
                foreach (var b in DebugFrame.Build(DebugFrame.CmdError, data[3], [2]))
                    pending.Enqueue(b);
                return data.Length;
            }
            var body = new List<byte> { 0, 0x50, 0x40, 0x01, 0x00, 0xFF };
            body.AddRange(Encoding.ASCII.GetBytes(text));
            foreach (var b in DebugFrame.Build((byte)(data[2] | 0x80), data[3], body.ToArray()))
                pending.Enqueue(b);
            return data.Length;
        }

        public byte[] Read(int n)
        {
            var out_ = new List<byte>();
            while (out_.Count < n && pending.Count > 0)
                out_.Add(pending.Dequeue());
            return [.. out_];
        }
    }

    private static string Beta7(byte section) => section switch
    {
        0x1C => "p25s.on=1\np25s.seq=3\np25s.f0=433500000,456,655,1,3,0,1\n",
        0x1D => "dmrs.on=0\ndmrs.seq=0\n",
        _ => "alls.on=0\nalls.seq=0\n",
    };

    [Fact]
    public void AnOlderFirmwareIsToldAndTheRunEnds()
    {
        foreach (var old in new Func<byte, string?>[] { _ => "", s => s == 0x1C ? "p25s.on=0\np25s.n=0\n" : "", _ => "p25s.built=0\n" })
        {
            var radio = new FakeSearchRadio(old);
            var lines = new List<string>();
            var ctx = new ToolContext { Print = s => lines.AddRange(s.Split('\n')), Clock = new VirtualClock(), Serial = _ => radio };
            var e = Assert.Throws<ToolExit>(() => DebugReaders.SearchLog(["--port", "COM14", "--reconnect"], ctx));
            Assert.Equal("4", e.Message);
            Assert.Contains(DebugReaders.SearchLogMissingLine, lines);
            Assert.Equal(DebugReaders.SearchLogMissingPolls, lines.Count(l => l.StartsWith("=== end ", StringComparison.Ordinal)));
            Assert.Equal((1, 1), (radio.Opens, radio.Closes));
        }
        // the unknown section's "bad argument" of a firmware before the search log: the same
        var bad = new FakeSearchRadio(_ => null);
        var c = new ToolContext { Clock = new VirtualClock(), Serial = _ => bad };
        Assert.Throws<ToolExit>(() => DebugReaders.SearchLog(["--port", "COM14", "--count", "5"], c));
    }

    [Fact]
    public void AGoneRadioIsWaitedForWithTheTableKept()
    {
        var radio = new FakeSearchRadio(Beta7);
        var lines = new List<string>();
        var ends = 0;
        var ctx = new ToolContext
        {
            Print = s =>
            {
                lines.AddRange(s.Split('\n'));
                if (s.StartsWith("=== end ", StringComparison.Ordinal) && ++ends == 2)
                    radio.Gone = true;  // the cable is pulled after the second poll
                if (s.StartsWith(DebugReaders.SearchLogWaiting, StringComparison.Ordinal) &&
                    lines.Count(l => l.StartsWith(DebugReaders.SearchLogWaiting, StringComparison.Ordinal)) == 3)
                    radio.Gone = false; // and back after three tries
            },
            Clock = new VirtualClock(), Serial = _ => radio,
        };
        DebugReaders.SearchLog(["--port", "COM14", "--count", "4", "--reconnect"], ctx);
        Assert.Equal(3, ends);  // poll 3 met the pulled cable (no end), poll 4 the radio again
        Assert.Equal(4, lines.Count(l => l.StartsWith("=== poll ", StringComparison.Ordinal)));
        Assert.Equal(2, radio.Opens);
        Assert.Contains("SEARCHLOG: COM14 open again", lines);
        Assert.True(lines.Count(l => l.StartsWith(DebugReaders.SearchLogWaiting, StringComparison.Ordinal)) >= 3);
        // without --reconnect a pulled cable ends the run with its error
        var r2 = new FakeSearchRadio(Beta7) { Gone = true };
        Assert.Throws<FileNotFoundException>(() => DebugReaders.SearchLog(["--port", "COM14"],
            new ToolContext { Clock = new VirtualClock(), Serial = _ => r2 }));
    }

    [Fact]
    public void ASilentRadioIsWaitedForAfterAMinute()
    {
        var radio = new FakeSearchRadio(Beta7) { Silent = true };
        var clock = new VirtualClock();
        var lines = new List<string>();
        double? waited = null;
        var t0 = clock.T;
        var ctx = new ToolContext
        {
            Print = s =>
            {
                lines.AddRange(s.Split('\n'));
                if (s.StartsWith(DebugReaders.SearchLogWaiting, StringComparison.Ordinal) && waited is null)
                {
                    waited = clock.T - t0;
                    radio.Silent = false;
                }
            },
            Clock = clock, Serial = _ => radio,
        };
        DebugReaders.SearchLog(["--port", "COM14", "--count", "30", "--reconnect"], ctx);
        Assert.NotNull(waited);
        Assert.InRange(waited!.Value, DebugReaders.SearchLogSilentSeconds, DebugReaders.SearchLogSilentSeconds + 5);
        Assert.Equal(2, radio.Opens);
        // without --reconnect: exit 3 after the same minute
        var r2 = new FakeSearchRadio(Beta7) { Silent = true };
        var e = Assert.Throws<ToolExit>(() => DebugReaders.SearchLog(["--port", "COM14"],
            new ToolContext { Clock = new VirtualClock(), Serial = _ => r2 }));
        Assert.Equal("3", e.Message);
    }

    [Fact]
    public void ReconnectStopsOnCancelWhileWaiting()
    {
        var radio = new FakeSearchRadio(Beta7) { Gone = true };
        using var cts = new CancellationTokenSource();
        var ctx = new ToolContext
        {
            Print = s =>
            {
                if (s.StartsWith(DebugReaders.SearchLogWaiting, StringComparison.Ordinal))
                    cts.Cancel();
            },
            Clock = new VirtualClock(), Serial = _ => radio, Cancel = cts.Token,
        };
        DebugReaders.SearchLog(["--port", "COM14", "--reconnect"], ctx);
        Assert.Equal(0, radio.Opens);
        Assert.False(DebugReaders.PortTrouble(new ToolContext.NoPortGivenException()));
        Assert.False(DebugReaders.PortTrouble(new ValueError("Refused")));
        Assert.True(DebugReaders.PortTrouble(new UnauthorizedAccessException()));
    }
}
