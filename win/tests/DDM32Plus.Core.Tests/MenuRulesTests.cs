// SPDX-License-Identifier: GPL-3.0-or-later
using DDM32Plus.Core;

namespace DDM32Plus.Core.Tests;

public class MenuRulesTests
{
    [Theory]
    [InlineData("update")]
    [InlineData("restore")]
    [InlineData("health")]
    [InlineData("live")]
    [InlineData("searchlog")]
    public void RadioPagesAreOffWhileAJobHoldsThePort(string key)
    {
        Assert.False(MenuRules.PageEnabled(key, true));
        Assert.True(MenuRules.PageEnabled(key, false));
    }

    [Theory]
    [InlineData("home")]
    [InlineData("versions")]
    [InlineData("packs")]
    [InlineData("channels")]
    public void FilePagesStayOn(string key)
    {
        Assert.True(MenuRules.PageEnabled(key, true));
        Assert.True(MenuRules.PageEnabled(key, false));
    }

    [Fact]
    public void EveryPageIsInOneList() =>
        Assert.Empty(MenuRules.RadioPages.Intersect(MenuRules.FilePages));
}
