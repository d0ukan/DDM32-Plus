// SPDX-License-Identifier: GPL-3.0-or-later
namespace DDM32Plus.Core;

/// <summary>The program's name, version and the fixed folder names (one place for all of them).</summary>
public static class AppInfo
{
    public const string Name = "DDM32 Plus";
    public const string ExeName = "DDM32Plus";
    public const string ExeFile = ExeName + ".exe";

    /// <summary>The Windows (.NET) program's version; Directory.Build.props carries the same value.</summary>
    public const string Version = "2.0.0-beta.5";

    public const string Author = "d0ukan · TA2DOU";
    public const string RepoUrl = "https://github.com/d0ukan/DDM32-Plus";

    /// <summary>The Python program's data folder names under %LOCALAPPDATA% (read only, never moved or deleted).</summary>
    public const string PythonDataName = "DDM32Plus";
    public const string LegacyDataName = "DDM25Plus";

    // Folders and files inside the install folder (English names since 2.0.0-beta.3, owner 2026-11-21)
    public const string BackupsFolder = "backups";
    public const string ReleasesFolder = "releases";
    public const string UpdatesFolder = "updates";
    public const string PacksFolder = "packs";
    /// <summary>The search log's CSV files (one a day; kept on uninstall).</summary>
    public const string LogsFolder = "logs";
    public const string SettingsFile = "settings.json";

    /// <summary>The Turkish folder names up to 2.0.0-beta.2, moved to the English ones at start-up
    /// (<see cref="Storage.AppPaths.Ensure"/>). The Python program's data folder keeps "yedekler".</summary>
    public const string LegacyBackupsFolder = "yedekler";
    public const string LegacyReleasesFolder = "surumler";
    public const string LegacyUpdatesFolder = "guncelleme";
    public const string LegacyPacksFolder = "paketler";
    public const string PythonBackupsFolder = LegacyBackupsFolder;

    /// <summary>(old, new) folder names in the install folder.</summary>
    public static readonly IReadOnlyList<(string Old, string New)> LegacyFolders =
    [
        (LegacyBackupsFolder, BackupsFolder), (LegacyReleasesFolder, ReleasesFolder),
        (LegacyUpdatesFolder, UpdatesFolder), (LegacyPacksFolder, PacksFolder),
    ];

    /// <summary>Names an update must never replace: the user's data in the install folder (old names too).</summary>
    public static readonly IReadOnlyList<string> ReservedNames =
        [BackupsFolder, ReleasesFolder, UpdatesFolder, PacksFolder, LogsFolder, SettingsFile, "unins000.exe", "unins000.dat",
         LegacyBackupsFolder, LegacyReleasesFolder, LegacyUpdatesFolder, LegacyPacksFolder];
}
