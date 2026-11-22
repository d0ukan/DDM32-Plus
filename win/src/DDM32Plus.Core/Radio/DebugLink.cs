// SPDX-License-Identifier: GPL-3.0-or-later
using System.Buffers.Binary;
using System.Globalization;
using System.Text;

namespace DDM32Plus.Core.Radio;

/// <summary>The radio answered with an error status, or did not answer (rfoundry_debug.DebugLinkError).</summary>
public sealed class DebugLinkError(string message, int? status = null) : ToolError("DebugLinkError", message)
{
    /// <summary>The radio's status byte; null when there was no answer.</summary>
    public int? Status { get; } = status;
}

/// <summary>
/// The RadioFoundry debug-link frame (the pinned tools/rfoundry_debug.py, RadioFoundry MIT): SOF A5 5A, command, sequence,
/// payload length u16, payload, CRC-16/CCITT-FALSE u16 over command..payload. An answer is command | 0x80 (or 0xFF for
/// an error) with the same sequence; its first payload byte is the status.
/// </summary>
public static class DebugFrame
{
    public const byte Sof1 = 0xA5, Sof2 = 0x5A, CmdError = 0xFF;
    public const int Header = 6, RequestMax = 64, AnswerMax = 1024;
    public const byte StatusOk = 0, StatusBusy = 3;

    public static readonly IReadOnlyDictionary<int, string> StatusNames = new Dictionary<int, string>
    {
        [0] = "ok", [1] = "unknown command", [2] = "bad argument", [3] = "busy", [4] = "unavailable", [5] = "bad CRC",
        [6] = "request too long", [7] = "refused",
    };

    public static ushort Crc16(ReadOnlySpan<byte> data, ushort crc = 0xFFFF)
    {
        foreach (var b in data)
        {
            crc ^= (ushort)(b << 8);
            for (var i = 0; i < 8; i++)
                crc = (crc & 0x8000) != 0 ? (ushort)((crc << 1) ^ 0x1021) : (ushort)(crc << 1);
        }
        return crc;
    }

    public static byte[] Build(byte cmd, byte seq, ReadOnlySpan<byte> payload)
    {
        var f = new byte[Header + payload.Length + 2];
        f[0] = Sof1;
        f[1] = Sof2;
        f[2] = cmd;
        f[3] = seq;
        BinaryPrimitives.WriteUInt16LittleEndian(f.AsSpan(4), (ushort)payload.Length);
        payload.CopyTo(f.AsSpan(Header));
        BinaryPrimitives.WriteUInt16LittleEndian(f.AsSpan(Header + payload.Length), Crc16(f.AsSpan(2, 4 + payload.Length)));
        return f;
    }

    /// <summary>(command, payload) of one whole, sound request frame; null for anything else.</summary>
    public static (byte Cmd, byte[] Payload)? ParseRequest(ReadOnlySpan<byte> f)
    {
        if (f.Length < Header + 2 || f[0] != Sof1 || f[1] != Sof2)
            return null;
        var n = f[4] | (f[5] << 8);
        if (f.Length != Header + n + 2 || n > RequestMax)
            return null;
        if (Crc16(f.Slice(2, 4 + n)) != (f[Header + n] | (f[Header + n + 1] << 8)))
            return null;
        return (f[2], f[Header..(Header + n)].ToArray());
    }

    /// <summary>Python's STATUS.get(st, st): the name, the number, or "None".</summary>
    public static string StatusText(int? st) =>
        st is null ? "None" : StatusNames.TryGetValue(st.Value, out var s) ? s : st.Value.ToString(CultureInfo.InvariantCulture);
}

/// <summary>
/// The read-only guard of the debug link: every frame is checked before it is sent and only the listed pairs of
/// command and exact payload pass (plus_live.GuardedLink, plus_txdiag.open_link's allowed set). Anything else, a
/// damaged frame or a baud change is refused, so a bug in a reader can never send a TX, key, radio operation, flash,
/// storage or register command.
/// </summary>
public sealed class DebugLinkPort(IRadioPort raw, IReadOnlySet<(byte Cmd, byte Arg)> allowed, string tool) : IRadioPort
{
    public List<string> Refusals { get; } = [];

    public double Timeout
    {
        get => raw.Timeout;
        set => raw.Timeout = value;
    }

    public int BaudRate
    {
        get => raw.BaudRate;
        set
        {
            if (value != 115200)
                throw new ValueError("The debug link is fixed at 115200 baud");
            raw.BaudRate = value;
        }
    }

    public byte[] Read(int n) => raw.Read(n);
    public void ResetInputBuffer() => raw.ResetInputBuffer();
    public void Flush() => raw.Flush();

    public void Validate(byte[] data)
    {
        var r = DebugFrame.ParseRequest(data);
        if (r is not { } q)
            throw Refuse($"{tool} refused a damaged frame before transmission");
        if (q.Payload.Length != 1 || !allowed.Contains((q.Cmd, q.Payload[0])))
            throw Refuse($"{tool} refused command 0x{q.Cmd:x2} {Convert.ToHexStringLower(q.Payload)} before transmission");
    }

    private ValueError Refuse(string why)
    {
        Refusals.Add(why);
        return new ValueError(why, "Refused");
    }

    public int? Write(byte[] data)
    {
        Validate(data);
        return raw.Write(data);
    }
}

/// <summary>
/// One open debug link (rfoundry_debug.DebugLink as plus_txdiag.DiagLink opens it: 115200 baud, a 1 s answer timeout,
/// busy_wait 0: one try). Raises <see cref="DebugLinkError"/> on an error status or no answer.
/// </summary>
public sealed class DebugLinkClient(IRadioPort port, IClock clock, string portName, double timeout = 1.0)
{
    private readonly List<byte> buf = [];
    private int seq;

    private (byte Cmd, byte Seq, byte[] Data)? ReadFrame(double deadline)
    {
        while (clock.Monotonic() < deadline)
        {
            buf.AddRange(port.Read(256));
            while (true)
            {
                var i = IndexOfSof();
                if (i < 0)
                {
                    if (buf.Count > 1)
                        buf.RemoveRange(0, buf.Count - 1);      // other protocols' bytes are skipped
                    break;
                }
                buf.RemoveRange(0, i);
                if (buf.Count < DebugFrame.Header)
                    break;
                var n = buf[4] | (buf[5] << 8);
                if (n > DebugFrame.AnswerMax)
                {
                    buf.RemoveAt(0);                // can't be ours: resync past this SOF
                    continue;
                }
                if (buf.Count < DebugFrame.Header + n + 2)
                    break;
                var f = buf.GetRange(0, DebugFrame.Header + n + 2).ToArray();
                if (DebugFrame.Crc16(f.AsSpan(2, 4 + n)) != (f[DebugFrame.Header + n] | (f[DebugFrame.Header + n + 1] << 8)))
                {
                    buf.RemoveAt(0);
                    continue;
                }
                buf.RemoveRange(0, DebugFrame.Header + n + 2);
                return (f[2], f[3], f[DebugFrame.Header..(DebugFrame.Header + n)]);
            }
        }
        return null;
    }

    private int IndexOfSof()
    {
        for (var i = 0; i + 1 < buf.Count; i++)
            if (buf[i] == DebugFrame.Sof1 && buf[i + 1] == DebugFrame.Sof2)
                return i;
        return -1;
    }

    private static string Seconds(double s) =>
        s == Math.Floor(s) ? ((long)s).ToString(CultureInfo.InvariantCulture) : s.ToString("R", CultureInfo.InvariantCulture);

    /// <summary>(status, data) of one command.</summary>
    public (int Status, byte[] Data) Request(byte cmd, byte[] payload)
    {
        if (payload.Length > DebugFrame.RequestMax)
            throw new DebugLinkError($"request payload {payload.Length} > {DebugFrame.RequestMax}");
        seq = (seq + 1) & 0xFF;
        var s = (byte)seq;
        port.Write(DebugFrame.Build(cmd, s, payload));
        var deadline = clock.Monotonic() + timeout;
        byte[] data;
        while (true)
        {
            var r = ReadFrame(deadline) ?? throw new DebugLinkError(
                $"no answer to command 0x{cmd:X2} within {Seconds(timeout)} s on {portName} " +
                "(firmware without the debug link? another program has the port?)");
            if (r.Seq != s)
                continue;                   // a late answer to an earlier request
            if (r.Cmd == DebugFrame.CmdError)
            {
                int? est = r.Data.Length > 0 ? r.Data[0] : null;
                throw new DebugLinkError($"command 0x{cmd:X2}: {DebugFrame.StatusText(est)}", est);
            }
            if (r.Cmd == (cmd | 0x80))
            {
                data = r.Data;
                break;
            }
        }
        int? st = data.Length > 0 ? data[0] : null;
        if (st != DebugFrame.StatusOk)
            throw new DebugLinkError($"command 0x{cmd:X2}: {DebugFrame.StatusText(st)}", st);
        return (0, data[1..]);
    }
}

/// <summary>Field and state names of the native modem status (pc/radiotools/native_modem.py).</summary>
public static class NativeModem
{
    public static readonly string[] Fields = ("version state reason session owned mode frequency_hz blocks duration_ms " +
        "lease_ms rx_count tx_count rx_seen rx_saved rx_dropped rx_read tx_loaded tx_sent phy_irqs tx_slot_irqs " +
        "rx_slot_irqs rf_irqs abnormal_irqs duplicate_irqs ignored_irqs pa_keys quiets faults rejected max_gap_us " +
        "last_gap_us restore_failures started_us boundary_us p25_sync key_requested keyed").Split(' ');

    public static readonly string[] States = ["idle", "armed", "receive", "transmit", "draining", "stopped", "fault"];

    public static readonly string[] Reasons = ["ok", "user_stop", "rx_limit", "tx_complete", "rx_duration",
        "lease_expired", "deadline", "stalled", "late_boundary", "early_boundary", "underrun", "io", "rx_overflow",
        "abnormal_slot", "denied", "interrupted", "restore_failed", "unexpected_rf"];

    /// <summary>The P25 zone scan counters (plus_txdiag.SCAN_FIELDS).</summary>
    public static readonly string[] ScanFields =
        "version stops carriers cooled takes synced voiced ended tails quiet foreign sync_ms voice_ms".Split(' ');
}

/// <summary>
/// The two read-only debug-link readers: <see cref="Health"/> (plus_txdiag.py --health: the views tasks, crash,
/// reset once) and <see cref="Live"/> (plus_live.py: crash and reset once, then core, tasks, native modem status and
/// the P25 zone scan counters every 1-5 s until stopped). Each opens the port, puts <see cref="DebugLinkPort"/> with its
/// own allowlist in front of it and prints what the Python tool prints, line for line (LiveTests.cs, live-results.json).
/// </summary>
public static class DebugReaders
{
    /// <summary>plus_txdiag --health sends only these: GET_VIEW tasks (20), crash (21), reset (23).</summary>
    public static readonly IReadOnlySet<(byte, byte)> HealthAllowed = new HashSet<(byte, byte)> { (0x09, 0x14), (0x09, 0x15), (0x09, 0x17) };

    /// <summary>plus_live.ALLOWED: the health views, GET_VIEW core (0), native status (0x21/0), scan counters (0x21/12).</summary>
    public static readonly IReadOnlySet<(byte, byte)> LiveAllowed = new HashSet<(byte, byte)>
    {
        (0x09, 0x00), (0x09, 0x14), (0x09, 0x15), (0x09, 0x17), (0x21, 0x00), (0x21, 0x0C),
    };

    public const int MaxSilentPolls = 3;

    /// <summary>The search log (firmware tools/plus_searchlog.py, tools/plus_alldigital.py) sends only these: GET_VIEW p25s
    /// (28), dmrs (29) and alls (30).</summary>
    public static readonly IReadOnlySet<(byte, byte)> SearchLogAllowed = new HashSet<(byte, byte)>
    {
        (0x09, SearchLogBook.SectionP25), (0x09, SearchLogBook.SectionDmr), (0x09, SearchLogBook.SectionAll),
    };

    /// <summary>The search log runs all night: a radio that does not answer at all for this long (seconds: a menu, a
    /// restart, the cable out) ends the run, or with --reconnect closes the port and waits for the radio.</summary>
    public const double SearchLogSilentSeconds = 60.0;

    /// <summary>With --reconnect: the port is tried again this often (seconds) while the radio is away.</summary>
    public const double SearchLogRetrySeconds = 5.0;

    /// <summary>Polls in a row that the radio answered without any search log section (a firmware before
    /// v1.0.0-beta.7, Classic, or one built without the searches) before the run ends with exit 4.</summary>
    public const int SearchLogMissingPolls = 3;

    /// <summary>The line the reader prints before it ends with exit 4 (the page shows its own sentence).</summary>
    public const string SearchLogMissingLine = "NO_SEARCHLOG: this radio's firmware has no search log (needs v1.0.0-beta.7)";

    /// <summary>The line printed while the radio is away (--reconnect): "WAITING: COM14 ...".</summary>
    public const string SearchLogWaiting = "WAITING:";

    /// <summary>search log: --port P [--count N] [--reconnect]. Every second the p25s, dmrs and alls sections, printed as
    /// the live reader prints a poll ("=== poll N" ... "=== end N"), until <see cref="ToolContext.Cancel"/> or N polls.
    /// A radio silent for <see cref="SearchLogSilentSeconds"/> ends the run with exit 3; with --reconnect (the page's
    /// all night run) the port is closed instead and opened again every <see cref="SearchLogRetrySeconds"/> ("WAITING:"
    /// lines) until the radio answers again, through a cable pull, a sleep of the PC or a radio restart. A radio that
    /// answers without the search log ends it with exit 4 (<see cref="SearchLogMissingLine"/>).</summary>
    public static void SearchLog(IReadOnlyList<string> argv, ToolContext ctx)
    {
        var a = new Args(argv, ["--port", "--count"], ["--reconnect"], ["--port"]);
        var portName = a.Get("--port")!;
        if (!int.TryParse(a.Get("--count") ?? "0", NumberStyles.Integer, CultureInfo.InvariantCulture, out var count) || count < 0)
            throw new ToolExit("2");
        var reconnect = a.Has("--reconnect");
        var polls = 0;
        var first = true;
        while (true)
        {
            int code;
            IRawSerial? raw = null;
            try
            {
                raw = Open(ctx, portName);
                ctx.Print(first ? $"SEARCHLOG: {portName} open, read only, every 1 s" : $"SEARCHLOG: {portName} open again");
                first = false;
                var link = new DebugLinkClient(new DebugLinkPort(raw, SearchLogAllowed, "searchlog"), ctx.Clock, portName);
                code = PollSearch(link, count, ref polls, ctx);
            }
            catch (Exception e) when (reconnect && PortTrouble(e))
            {
                ctx.Print($"error={e.Message}");
                code = 3;
            }
            finally
            {
                try
                {
                    raw?.Close();
                }
                catch (Exception e) when (PortTrouble(e))
                {
                    // a port that went away closes with an error: it is closed all the same
                }
            }
            if (code == 4)
                ctx.Print(SearchLogMissingLine);
            if (code != 3 || !reconnect || ctx.Cancel.IsCancellationRequested || (count != 0 && polls >= count))
            {
                if (code != 0 && !(code == 3 && reconnect))
                    throw new ToolExit(code.ToString(CultureInfo.InvariantCulture));
                return;
            }
            ctx.Print($"{SearchLogWaiting} {portName}: no radio; trying again in {SearchLogRetrySeconds:0} s");
            Wait(ctx, SearchLogRetrySeconds);
            if (ctx.Cancel.IsCancellationRequested)
                return;
        }
    }

    /// <summary>A serial port that went away (a cable pull, a sleep of the PC, a new COM number): worth trying again.
    /// A refused frame, a missing port factory or radio access switched off are not.</summary>
    public static bool PortTrouble(Exception e) => e switch
    {
        ToolContext.NoPortGivenException or RadioAccess.DisabledException => false,
        IOException or UnauthorizedAccessException or TimeoutException or InvalidOperationException => true,
        _ => false,
    };

    /// <summary>The polls of one open port: 0 after <paramref name="count"/> polls (all of the run, counted in
    /// <paramref name="polls"/>) or a stop, 3 when the radio was silent for <see cref="SearchLogSilentSeconds"/>, 4 when
    /// it answered <see cref="SearchLogMissingPolls"/> times in a row without the search log.</summary>
    public static int PollSearch(DebugLinkClient link, int count, ref int polls, ToolContext ctx)
    {
        var heard = ctx.Clock.Monotonic();
        var missing = 0;
        while ((count == 0 || polls < count) && !ctx.Cancel.IsCancellationRequested)
        {
            var n = ++polls;
            var started = ctx.Clock.Monotonic();
            ctx.Print($"=== poll {n}");
            var text = new List<string>();
            int[] codes = [PrintView(link, "view p25s", SearchLogBook.SectionP25, ctx, text),
                PrintView(link, "view dmrs", SearchLogBook.SectionDmr, ctx, text),
                PrintView(link, "view alls", SearchLogBook.SectionAll, ctx, text)];
            ctx.Print($"=== end {n}");
            if (codes.Any(c => c != -1))
                heard = ctx.Clock.Monotonic();
            else if (ctx.Clock.Monotonic() - heard >= SearchLogSilentSeconds)
            {
                ctx.Print($"No answer from the radio for {SearchLogSilentSeconds:0} s; stopping.");
                return 3;
            }
            // answered (ok, unknown command, bad argument: not busy) and no search log in it: an older firmware
            missing = codes.All(c => c is 0 or 1 or 2) && !SearchLogBook.HasSearchLog(text) ? missing + 1 : 0;
            if (missing >= SearchLogMissingPolls)
                return 4;
            if (count == 0 || polls < count)
                Wait(ctx, Math.Max(0.0, 1.0 - (ctx.Clock.Monotonic() - started)));
        }
        return 0;
    }

    private static (string Name, byte Section)[] HealthViews => [("tasks", 0x14), ("crash", 0x15), ("reset", 0x17)];

    private static IRawSerial Open(ToolContext ctx, string portName)
    {
        if (string.IsNullOrEmpty(portName) || portName.Contains("://", StringComparison.Ordinal) ||
            portName.StartsWith("udp:", StringComparison.Ordinal))
            throw new ValueError("Explicit local serial port required");
        var raw = ctx.Serial(portName);
        raw.Open();
        raw.BaudRate = 115200;
        raw.Timeout = 0.02;
        return raw;
    }

    /// <summary>bytes.decode('ascii', 'replace')</summary>
    public static string Ascii(ReadOnlySpan<byte> b)
    {
        var sb = new StringBuilder(b.Length);
        foreach (var c in b)
            sb.Append(c < 0x80 ? (char)c : '�');
        return sb.ToString();
    }

    private static uint U32(byte[] d, int o) => BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(o));

    /// <summary>plus_txdiag.py --port P --health</summary>
    public static void Health(IReadOnlyList<string> argv, ToolContext ctx)
    {
        var a = new Args(argv, ["--port"], ["--health"], ["--port"]);
        var raw = Open(ctx, a.Get("--port")!);
        try
        {
            var link = new DebugLinkClient(new DebugLinkPort(raw, HealthAllowed, "plus_txdiag"), ctx.Clock, a.Get("--port")!);
            foreach (var (name, section) in HealthViews)
            {
                var (code, data) = link.Request(0x09, [section]);
                ctx.Print($"--- view {name} ({section}) status {code}");
                if (code == 0 && data.Length >= 5)
                {
                    ctx.Print($"uptime_ms={U32(data, 0)}");
                    ctx.Print(Ascii(data.AsSpan(5)).TrimEnd());
                }
            }
        }
        finally
        {
            raw.Close();
        }
    }

    /// <summary>plus_live.py --port P [--interval 1..5] [--count N]: polls until <see cref="ToolContext.Cancel"/> (the
    /// page's "Durdur") or N polls. Three polls without any answer end the run (SystemExit 3, as the Python tool).</summary>
    public static void Live(IReadOnlyList<string> argv, ToolContext ctx)
    {
        var a = new Args(argv, ["--port", "--interval", "--count"], [], ["--port"]);
        var portName = a.Get("--port")!;
        if (!double.TryParse(a.Get("--interval") ?? "2", NumberStyles.Float, CultureInfo.InvariantCulture, out var interval) ||
            interval < 1 || interval > 5)
            throw new ToolExit("2");
        if (!int.TryParse(a.Get("--count") ?? "0", NumberStyles.Integer, CultureInfo.InvariantCulture, out var count) || count < 0)
            throw new ToolExit("2");
        var raw = Open(ctx, portName);
        int code;
        try
        {
            ctx.Print($"LIVE: {portName} open, read only, every {interval.ToString("0.###", CultureInfo.InvariantCulture)} s");
            var link = new DebugLinkClient(new DebugLinkPort(raw, LiveAllowed, "plus_live"), ctx.Clock, portName);
            code = Poll(link, interval, count, ctx);
        }
        finally
        {
            raw.Close();
        }
        if (code != 0)
            throw new ToolExit(code.ToString(CultureInfo.InvariantCulture));
    }

    /// <summary>plus_live.run on an open (guarded) link: 0 after count polls or a stop, 3 when the radio went silent.</summary>
    public static int Poll(DebugLinkClient link, double interval, int count, ToolContext ctx)
    {
        PrintView(link, "view crash", 0x15, ctx);
        PrintView(link, "view reset", 0x17, ctx);
        int n = 0, silent = 0;
        while ((count == 0 || n < count) && !ctx.Cancel.IsCancellationRequested)
        {
            n++;
            var started = ctx.Clock.Monotonic();
            ctx.Print($"=== poll {n}");
            int[] codes = [PrintView(link, "view core", 0x00, ctx), PrintView(link, "view tasks", 0x14, ctx),
                PrintNative(link, ctx), PrintScan(link, ctx)];
            ctx.Print($"=== end {n}");
            silent = codes.All(c => c == -1) ? silent + 1 : 0;
            if (silent >= MaxSilentPolls)
            {
                ctx.Print($"No answer from the radio for {silent} polls; stopping.");
                return 3;
            }
            if (count == 0 || n < count)
                Wait(ctx, Math.Max(0.0, interval - (ctx.Clock.Monotonic() - started)));
        }
        return 0;
    }

    private static void Wait(ToolContext ctx, double seconds)
    {
        var end = ctx.Clock.Monotonic() + seconds;
        while (!ctx.Cancel.IsCancellationRequested)
        {
            var left = end - ctx.Clock.Monotonic();
            if (left <= 0)
                break;
            ctx.Clock.Sleep(Math.Min(0.05, left));
        }
    }

    /// <summary>One query, printed; (status, data or null). A radio error or no answer is reported, never thrown.</summary>
    private static (int Code, byte[]? Data) Query(DebugLinkClient link, string name, byte cmd, byte arg, ToolContext ctx)
    {
        var label = cmd != 0x09 ? $"0x{cmd:x2}/{arg}" : arg.ToString(CultureInfo.InvariantCulture);
        try
        {
            var (code, data) = link.Request(cmd, [arg]);
            ctx.Print($"--- {name} ({label}) status {code}");
            return (code, data);
        }
        catch (DebugLinkError e)
        {
            var code = e.Status ?? -1;
            ctx.Print($"--- {name} ({label}) status {code}");
            ctx.Print($"error={e.Record}");
            return (code, null);
        }
    }

    private static int PrintView(DebugLinkClient link, string name, byte section, ToolContext ctx, List<string>? sink = null)
    {
        var (code, data) = Query(link, name, 0x09, section, ctx);
        if (code == 0 && data is { Length: >= 5 })
        {
            ctx.Print($"uptime_ms={U32(data, 0)}");
            var text = Ascii(data.AsSpan(5)).TrimEnd();
            if (text.Length > 0)
            {
                ctx.Print(text);
                sink?.AddRange(text.Split('\n'));
            }
        }
        return code;
    }

    private static int PrintNative(DebugLinkClient link, ToolContext ctx)
    {
        var (code, data) = Query(link, "native", 0x21, 0x00, ctx);
        if (code != 0 || data is null)
            return code;
        if (data.Length == 4 * NativeModem.Fields.Length)
        {
            var v = new uint[NativeModem.Fields.Length];
            for (var i = 0; i < v.Length; i++)
            {
                v[i] = U32(data, 4 * i);
                ctx.Print($"{NativeModem.Fields[i]}={v[i]}");
            }
            if (v[1] < NativeModem.States.Length)
                ctx.Print("state_name=" + NativeModem.States[v[1]]);
            if (v[2] < NativeModem.Reasons.Length)
                ctx.Print("reason_name=" + NativeModem.Reasons[v[2]]);
        }
        else
            ctx.Print("raw=" + Convert.ToHexStringLower(data));
        return code;
    }

    private static int PrintScan(DebugLinkClient link, ToolContext ctx)
    {
        var (code, data) = Query(link, "scan", 0x21, 0x0C, ctx);
        if (code != 0 || data is null)
            return code;
        if (data.Length == 4 * NativeModem.ScanFields.Length)
            for (var i = 0; i < NativeModem.ScanFields.Length; i++)
                ctx.Print($"{NativeModem.ScanFields[i]}={U32(data, 4 * i)}");
        else
            ctx.Print("raw=" + Convert.ToHexStringLower(data));
        return code;
    }
}
