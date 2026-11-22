// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using DDM32Plus.Core;
using DDM32Plus.Core.Update;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views;

/// <summary>A page of the main window (the tabs of pc/ddm32_app/main_window.py).</summary>
public interface IPage
{
    void Init(AppState state, MainWindow window);

    /// <summary>Redraws the code-made texts (after a language change or new data).</summary>
    void Refresh();
}

/// <summary>The main window: toolbar, the pages, the status bar.</summary>
public partial class MainWindow : Window
{
    private readonly AppState state;
    private readonly IPage[] pages;

    public MainWindow(AppState state)
    {
        this.state = state;
        InitializeComponent();
        pages = [Home, Update, Versions, Restore, Packs, Health, Live, SearchLog, Channels];
        foreach (var p in pages)
            p.Init(state, this);
        state.Changed += () => Dispatcher.Invoke(RefreshAll);
        ToolRunner.BusyChanged += () => Dispatcher.BeginInvoke(RefreshAll);
        Loc.Instance.Changed += RefreshAll;
        RestoreBounds_();
        RefreshAll();
    }

    public AppState State => state;

    private void RefreshAll()
    {
        Title = $"{AppInfo.Name} {AppState.ProgramVersion} - TA2DOU" + (!Core.Radio.BuildFlags.IsDeveloper ? "" :
            Core.Radio.RadioAccess.WritesOn ? Loc.T("Dev_TitleWrite") : Core.Radio.RadioAccess.IsOn ? Loc.T("Dev_Title") : "");
        LocalButton.Visibility = Core.Radio.BuildFlags.IsDeveloper ? Visibility.Visible : Visibility.Collapsed;
        MenuLocal.Visibility = LocalButton.Visibility;
        foreach (var m in PageMenuItems(MenuBar))
            m.IsEnabled = MenuRules.PageEnabled((string)m.Tag, ToolRunner.AnyBusy);
        MenuTr.IsChecked = Loc.Instance.Language == "tr";
        MenuEn.IsChecked = Loc.Instance.Language == "en";
        StatusCable.Text = Loc.F("Status_Cable", CableScanner.Short(state.Cable, state.Port));
        StatusInstalled.Text = Loc.F("Status_Installed",
            state.Installed()?.Version ?? state.Settings.GetString("installed_firmware") ?? Loc.T("Common_Unknown"));
        StatusData.Text = Loc.F("Status_Data", state.Paths.Root);
        foreach (var p in pages)
            p.Refresh();
    }

    /// <summary>Once the window is shown: the disclaimer first (when not accepted), the program update result, the
    /// start-up check (unless switched off).</summary>
    public async void FirstStart()
    {
        state.RefreshCable();
        if (!state.DisclaimerAccepted)
            ShowDisclaimer();
        // An update started by 2.0.0-beta.2 or older wrote its result into the old guncelleme folder
        var legacyUpdates = Path.Combine(state.Paths.Root, AppInfo.LegacyUpdatesFolder);
        var result = SelfUpdate.ReadResult(state.Paths.Updates) ??
                     (Directory.Exists(legacyUpdates) ? SelfUpdate.ReadResult(legacyUpdates) : null);
        if (result is not null)
        {
            var key = result switch
            {
                "ok" => "Home_UpdateResultOk",
                "rolled_back" => "Home_UpdateResultRolledBack",
                _ => "Home_UpdateResultAborted",
            };
            MessageBox.Show(this, Loc.T(key), AppInfo.Name, MessageBoxButton.OK, MessageBoxImage.Information);
        }
        if (AppState.CanSelfUpdate)
        {
            SelfUpdate.Cleanup(state.Paths.Updates, Path.GetDirectoryName(Environment.ProcessPath));
            SelfUpdate.RemoveLegacyUpdates(legacyUpdates, Path.GetDirectoryName(Environment.ProcessPath));
        }
        if (state.Settings.AutoCheck)
            await state.CheckAsync();
    }

    /// <summary>--show PAGE[:STEP[:BACKUP]] (screenshots, demos): open a page, a wizard step, and a verified backup as
    /// "Mevcut yedeği seç..." does; health:demo and live:demo show made-up readings, channels:FOLDER a backup's
    /// codeplug. Navigation and file reads only; a radio step still needs its own button.</summary>
    public void ShowFromArgs(string spec)
    {
        var parts = spec.Split(':', 3);
        OpenPage(parts[0]);
        if (parts[0] == "health" && parts.Length > 1 && parts[1] == "demo")
            Health.ShowDemo();                          // made-up views; no port
        if (parts[0] == "live" && parts.Length > 1 && parts[1] == "demo")
            Live.ShowDemo();                            // made-up polls; no port
        if (parts[0] == "channels" && parts.Length > 1)
            Channels.ShowBackup(spec[(spec.IndexOf(':', StringComparison.Ordinal) + 1)..]);   // files only
        if (parts[0] == "update" && parts.Length > 1 && int.TryParse(parts[1], out var step))
            Update.ShowStep(step);
        if (parts[0] == "update" && parts.Length > 2)
            Update.UseBackup(parts[2]);                 // as "Mevcut yedeği seç...": files only
    }

    public void OpenPage(string key)
    {
        TabItem? tab = key switch
        {
            "home" => TabHome,
            "update" => TabUpdate,
            "versions" => TabVersions,
            "restore" => TabRestore,
            "packs" => TabPacks,
            "health" => TabHealth,
            "live" => TabLive,
            "searchlog" => TabSearchLog,
            "channels" => TabChannels,
            _ => null,
        };
        if (tab is not null)
            Tabs.SelectedItem = tab;
    }

    public void ShowDisclaimer()
    {
        var d = new DisclaimerDialog { Owner = IsLoaded ? this : null };
        if (d.ShowDialog() == true)
            state.Settings.AcceptDisclaimer(AppState.DisclaimerVersion(), Core.Releases.ReleaseClient.NowUtc());
        RefreshAll();
    }

    private static IEnumerable<MenuItem> PageMenuItems(ItemsControl parent)
    {
        foreach (var item in parent.Items.OfType<MenuItem>())
        {
            if (item.Tag is string)
                yield return item;
            foreach (var sub in PageMenuItems(item))
                yield return sub;
        }
    }

    private void OnMenuPage(object sender, RoutedEventArgs e)
    {
        if (sender is MenuItem { Tag: string key })
            OpenPage(key);
    }

    private void OpenFolder(string folder)
    {
        Directory.CreateDirectory(folder);
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(folder) { UseShellExecute = true });
    }

    private void OnOpenBackups(object sender, RoutedEventArgs e) => OpenFolder(state.Paths.Backups);

    private void OnOpenLogs(object sender, RoutedEventArgs e) => OpenFolder(state.Paths.Logs);

    private void OnExit(object sender, RoutedEventArgs e) => Close();

    private void OnGitHub(object sender, RoutedEventArgs e) =>
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(AppInfo.RepoUrl) { UseShellExecute = true });

    private void OnHelp(object sender, RoutedEventArgs e) => InfoDialog.ShowHelp(this, state);

    private void OnDisclaimer(object sender, RoutedEventArgs e) => ShowDisclaimer();

    private void OnAbout(object sender, RoutedEventArgs e) => InfoDialog.ShowAbout(this);

    private void OnLocalRelease(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFolderDialog { Title = Loc.T("Menu_LocalRelease") };
        if (dlg.ShowDialog(this) == true)
            LoadLocalRelease(dlg.FolderName);
    }

    /// <summary>--release-folder PATH or the toolbar's local release entry: developer builds only.</summary>
    public async void LoadLocalRelease(string folder)
    {
        if (!Core.Radio.BuildFlags.IsDeveloper)
            return;
        var (error, versions) = await state.LoadLocalAsync(folder);
        MessageBox.Show(this, error is null ? Loc.F("Local_Loaded", string.Join(", ", versions)) : Loc.F("Local_Failed", error),
            AppInfo.Name, MessageBoxButton.OK, error is null ? MessageBoxImage.Information : MessageBoxImage.Warning);
    }

    private void SetLanguage(string lang)
    {
        state.Settings.Language = lang;
        Loc.Instance.SetLanguage(lang);
    }

    private void OnTurkish(object sender, RoutedEventArgs e) => SetLanguage("tr");

    private void OnEnglish(object sender, RoutedEventArgs e) => SetLanguage("en");

    private void OnToolBarLoaded(object sender, RoutedEventArgs e)
    {
        // No overflow arrow on the toolbar
        if (sender is ToolBar tb && tb.Template.FindName("OverflowGrid", tb) is FrameworkElement overflow)
            overflow.Visibility = Visibility.Collapsed;
    }

    // --- window size and place, kept in settings.json ---------------------------------------------------------
    private void RestoreBounds_()
    {
        var b = state.Settings.Load()["window"]?.AsObject();
        if (b is null)
            return;
        try
        {
            double x = (double)b["x"]!, y = (double)b["y"]!, w = (double)b["w"]!, h = (double)b["h"]!;
            var screen = new Rect(SystemParameters.VirtualScreenLeft, SystemParameters.VirtualScreenTop,
                SystemParameters.VirtualScreenWidth, SystemParameters.VirtualScreenHeight);
            if (w >= MinWidth && h >= MinHeight && screen.IntersectsWith(new Rect(x, y, w, h)))
            {
                WindowStartupLocation = WindowStartupLocation.Manual;
                (Left, Top, Width, Height) = (x, y, w, h);
            }
            if (b["max"] is { } m && (bool)m)
                WindowState = WindowState.Maximized;
        }
        catch (Exception e) when (e is InvalidOperationException or FormatException or NullReferenceException)
        {
        }
    }

    protected override void OnClosing(System.ComponentModel.CancelEventArgs e)
    {
        if (Update.Busy || Restore.Busy || Packs.Busy || Health.Busy)
        {
            // closing would end a running tool half way (main_window.busy_text)
            MessageBox.Show(this, Loc.T("Main_BusyClose"), AppInfo.Name, MessageBoxButton.OK, MessageBoxImage.Warning);
            e.Cancel = true;
            return;
        }
        Live.Stop();                                    // the live reader ends at its next step and closes the port
        SearchLog.Stop();                               // the search log too
        var r = WindowState == WindowState.Normal ? new Rect(Left, Top, Width, Height) : RestoreBounds;
        state.Settings.Save("window", new System.Text.Json.Nodes.JsonObject
        {
            ["x"] = r.Left, ["y"] = r.Top, ["w"] = r.Width, ["h"] = r.Height,
            ["max"] = WindowState == WindowState.Maximized,
        });
        base.OnClosing(e);
    }
}
