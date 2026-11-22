// SPDX-License-Identifier: GPL-3.0-or-later
namespace DDM32Plus.Core;

/// <summary>When the menu bar's entries are usable. Pages that hold the serial port are only reachable from the menu while
/// no radio step runs (as their own buttons are); file-only pages, folders and Help always are.</summary>
public static class MenuRules
{
    /// <summary>Page keys under the Radio menu: they read or write the radio through the cable (data packs write to it).</summary>
    public static readonly IReadOnlyList<string> RadioPages = ["update", "restore", "packs", "health", "live", "searchlog"];

    /// <summary>Page keys that never touch the port (home, the release list, backups on disk).</summary>
    public static readonly IReadOnlyList<string> FilePages = ["home", "versions", "channels"];

    public static bool PageEnabled(string key, bool anyRadioJobBusy) =>
        !anyRadioJobBusy || !RadioPages.Contains(key);
}
