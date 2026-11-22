// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using DDM32Plus.Core;
using DDM32Plus.Core.Radio;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>A row of the table (strings in the page's language).</summary>
public sealed record SearchRow(string First, string Time, string Mode, string Mhz, string Code, string Slot, string Group, string Source,
    string Calls);

/// <summary>
/// Arama kaydı / Search log: "Kaydı başlat" is the user's approval to open the port for this page. Every second
/// DebugReaders.SearchLog reads the radio's P25, DMR and all digital search finds (read only, not counted as a look);
/// every call is counted on its line (newest first) and in this session's logs\search-YYYYMMDD-HHMMSS.csv, until
/// "Durdur". The radio runs the search itself (Tools > P25 search / DMR search / All digital search); this page only
/// keeps what it finds, all night if left on: Windows is kept awake while it logs, and the reader (--reconnect) waits
/// for a radio that went away (cable, sleep, restart) with the table kept. One CSV per session (docs/plus.md "Search log
/// on the PC"): named after the moment the log started, never overwritten by a later session, a run past midnight stays
/// in it; a stop and a new start in the same program run go on in the same file.
/// </summary>
public partial class SearchLogPage : UserControl, IPage
{
    private AppState? state;
    private ToolRunner? runner;
    private CancellationTokenSource? cancel;
    private List<string>? block;
    private bool stopping;
    private bool noFirmware;
    private string status = "";
    private string error = "";
    private SearchLogBook book = new();
    private readonly ObservableCollection<SearchRow> rows = [];
    private string? csvPath;
    private string? writtenPath;
    private DateTime? started;

    public SearchLogPage() => InitializeComponent();

    public bool Running => runner?.Busy == true;

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        runner = new ToolRunner(Dispatcher);
        Grid.ItemsSource = rows;
        Columns();
    }

    private void Columns()
    {
        Grid.Columns.Clear();
        foreach (var (key, path) in new[] { ("SLog_First", "First"), ("SLog_Time", "Time"), ("SLog_Mode", "Mode"), ("SLog_Mhz", "Mhz"), ("SLog_Code", "Code"),
                     ("SLog_Slot", "Slot"), ("SLog_Group", "Group"), ("SLog_Source", "Source"), ("SLog_Calls", "Calls") })
            Grid.Columns.Add(new DataGridTextColumn { Header = Loc.T(key), Binding = new Binding(path) });
    }

    public void Refresh()
    {
        if (state is null)
            return;
        Columns();
        CableText.Text = CableScanner.Text(state.Cable);
        var busy = Running;
        StartButton.IsEnabled = !busy && !ToolRunner.AnyBusy && state.Port is not null && state.DisclaimerAccepted;
        StopButton.IsEnabled = busy && !stopping;
        ClearButton.IsEnabled = rows.Count > 0 || (csvPath is not null && File.Exists(csvPath));
        StatusText.Text = status.Length > 0 ? status : Loc.T("SLog_NotRunning");
        SearchText.Text = Running ? Searches() : "";
        ErrorText.Text = error;
        ErrorText.Visibility = error.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private string Searches()
    {
        string One(string name, bool? on, long? at, DateTime? stopped) => on switch
        {
            true => Loc.F("SLog_SearchOn", name, at is { } hz ? SearchLogBook.Mhz(hz) : "-"),
            false when stopped is { } t => Loc.F("SLog_SearchStopped", name, t.ToString("HH:mm", CultureInfo.InvariantCulture)),
            false => Loc.F("SLog_SearchOff", name),
            _ => Loc.F("SLog_SearchNone", name),
        };
        return One("P25", book.P25On, book.P25AtHz, book.P25StoppedAt) + "   " +
               One("DMR", book.DmrOn, book.DmrAtHz, book.DmrStoppedAt) + "   " +
               One(Loc.T("SLog_All"), book.AllOn, book.AllAtHz, book.AllStoppedAt) + "   " +
               Loc.F("SLog_Count", book.TotalCalls, book.Frequencies) +
               (book.Missed > 0 ? " " + Loc.F("SLog_Missed", book.Missed) : "");
    }

    private async void OnStart(object sender, RoutedEventArgs e)
    {
        if (state?.Port is not { } port || runner is null || Running || ToolRunner.AnyBusy || !state.DisclaimerAccepted)
            return;
        block = null;
        stopping = false;
        noFirmware = false;
        error = "";
        started ??= DateTime.Now;   // the session (its CSV file) begins with its first start
        status = Loc.F("Live_Connecting", port);
        cancel = new CancellationTokenSource();
        Refresh();
        KeepAwake.On();
        RunEnd end;
        try
        {
            end = await runner.RunAsync(DebugReaders.SearchLog, ["--port", port, "--reconnect"], null, OnLine, writes: false,
                cancel.Token);
        }
        finally
        {
            KeepAwake.Off();
        }
        var stamp = DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
        if (end.AccessOff)
            status = Loc.T("Radio_Disabled");
        else if (noFirmware || end.Error is { PyType: "SystemExit", Message: "4" })
        {
            status = Loc.F("Live_Stopped", stamp);
            error = Loc.T("SLog_NoFirmware");
        }
        else if (stopping || end.Error is null)
            status = Loc.F("Live_Stopped", stamp);
        else
        {
            status = Loc.T("Live_Lost");
            error = end.Error.Record;
        }
        stopping = false;
        cancel.Dispose();
        cancel = null;
        Refresh();
    }

    private void OnStop(object sender, RoutedEventArgs e) => Stop();

    public void Stop()
    {
        if (cancel is null || stopping)
            return;
        stopping = true;
        cancel.Cancel();
        Refresh();
    }

    /// <summary>"Kayıtları temizle": after a confirmation, the table and this session's CSV file are emptied; a running
    /// log goes on in a new session file, counting only calls heard from now on.</summary>
    private void OnClear(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(Window.GetWindow(this), Loc.T("SLog_ClearAsk"), AppInfo.Name, MessageBoxButton.YesNo,
                MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        rows.Clear();
        book.Clear();
        try
        {
            foreach (var p in new[] { csvPath, csvPath is null ? null : SearchLogBook.SidePath(csvPath) })
                if (p is not null && File.Exists(p))
                    File.Delete(p);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            error = ex.Message;
        }
        csvPath = writtenPath = null;
        started = Running ? DateTime.Now : null;
        Refresh();
    }

    private void OnFolder(object sender, RoutedEventArgs e)
    {
        if (state is null)
            return;
        Directory.CreateDirectory(state.Paths.Logs);
        Process.Start(new ProcessStartInfo(state.Paths.Logs) { UseShellExecute = true });
    }

    private void OnLine(string line)
    {
        var s = line.Trim();
        var m = LiveData.PollMarker(s);
        if (m is ("poll", _))
        {
            block = [];
            return;
        }
        if (m is ("end", _))
        {
            if (block is not null)
                Take(block);
            block = null;
            return;
        }
        if (block is null)
        {
            if (s.StartsWith(DebugReaders.SearchLogWaiting, StringComparison.Ordinal) && state?.Port is { } port)
            {
                status = Loc.F("SLog_Waiting", port);
                Refresh();
            }
            else if (s.StartsWith("NO_SEARCHLOG", StringComparison.Ordinal))
                noFirmware = true;
            return;
        }
        block.Add(s);
    }

    private void Take(List<string> lines)
    {
        var now = DateTime.Now;
        if (book.Add(lines, now) && state is not null)
        {
            var groups = book.Groups;
            try
            {
                csvPath ??= SearchLogBook.SessionPath(state.Paths.Logs, started ?? now);
                var r = SearchLogBook.Write(csvPath, groups, SearchLogBook.ListSeparator());
                writtenPath = r.Path;
                error = r.Locked ? Loc.F("SLog_Locked", r.Path) : "";
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                error = ex.Message;
            }
            rows.Clear();
            foreach (var g in groups)
                rows.Add(Row(g));
        }
        status = Loc.F("SLog_Running", now.ToString("HH:mm:ss", CultureInfo.InvariantCulture)) +
                 (writtenPath is null ? "" : "  " + Loc.F("SLog_File", writtenPath));
        Refresh();
    }

    private static SearchRow Row(SearchGroup c) => new(
        c.First.ToString("dd.MM HH:mm:ss", CultureInfo.InvariantCulture),
        c.Last.ToString("dd.MM HH:mm:ss", CultureInfo.InvariantCulture), c.Mode, SearchLogBook.Mhz(c.Hz), c.Code, c.Slot,
        c.Group == 0 ? "-" : c.Group.ToString(CultureInfo.InvariantCulture),
        c.Caller.Length > 0 ? c.Caller : c.Source == 0 ? "-" : c.Source.ToString(CultureInfo.InvariantCulture) + (c.Encrypted ? " (enc)" : ""),
        c.Calls.ToString(CultureInfo.InvariantCulture));
}

/// <summary>Keeps Windows from sleeping while the search log runs all night (SetThreadExecutionState with
/// ES_SYSTEM_REQUIRED | ES_CONTINUOUS on the UI thread, which lives as long as the program; the screen may still go
/// off). Off gives the PC its normal sleep back.</summary>
internal static class KeepAwake
{
    private const uint EsContinuous = 0x80000000, EsSystemRequired = 0x00000001;

    [DllImport("kernel32.dll")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static extern uint SetThreadExecutionState(uint flags);

    public static void On() => SetThreadExecutionState(EsContinuous | EsSystemRequired);

    public static void Off() => SetThreadExecutionState(EsContinuous);
}
