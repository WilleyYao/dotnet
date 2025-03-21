﻿// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#nullable disable

namespace System.Windows.Forms.Tests;

// NB: doesn't require thread affinity
public class RetrieveVirtualItemEventArgsTests
{
    [Theory]
    [InlineData(-1)]
    [InlineData(0)]
    [InlineData(1)]
    public void Ctor_Int(int itemIndex)
    {
        RetrieveVirtualItemEventArgs e = new(itemIndex);
        Assert.Equal(itemIndex, e.ItemIndex);
        Assert.Null(e.Item);
    }

    public static IEnumerable<object[]> Item_TestData()
    {
        yield return new object[] { null };
        yield return new object[] { new ListViewItem() };
    }

    [Theory]
    [MemberData(nameof(Item_TestData))]
    public void Item_Set_GetReturnsExpected(ListViewItem value)
    {
        RetrieveVirtualItemEventArgs e = new(1)
        {
            Item = value
        };
        Assert.Equal(value, e.Item);
    }
}
