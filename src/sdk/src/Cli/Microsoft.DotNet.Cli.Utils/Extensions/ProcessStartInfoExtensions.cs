﻿// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Diagnostics;

namespace Microsoft.DotNet.Cli.Utils.Extensions;

internal static class ProcessStartInfoExtensions
{
    public static int Execute(this ProcessStartInfo startInfo)
    {
        if (startInfo == null)
        {
            throw new ArgumentNullException(nameof(startInfo));
        }

        var process = new Process
        {
            StartInfo = startInfo
        };

        using (var reaper = new ProcessReaper(process))
        {
            process.Start();
            reaper.NotifyProcessStarted();
            process.WaitForExit();
        }

        return process.ExitCode;
    }

    public static int ExecuteAndCaptureOutput(this ProcessStartInfo startInfo, out string? stdOut, out string? stdErr)
    {
        var outStream = new StreamForwarder().Capture();
        var errStream = new StreamForwarder().Capture();

        startInfo.RedirectStandardOutput = true;
        startInfo.RedirectStandardError = true;

        var process = new Process
        {
            StartInfo = startInfo,
            EnableRaisingEvents = true
        };

        using (var reaper = new ProcessReaper(process))
        {
            process.Start();
            reaper.NotifyProcessStarted();

            var taskOut = outStream.BeginRead(process.StandardOutput);
            var taskErr = errStream.BeginRead(process.StandardError);

            process.WaitForExit();

            taskOut.Wait();
            taskErr.Wait();

            stdOut = outStream.CapturedOutput;
            stdErr = errStream.CapturedOutput;
        }

        return process.ExitCode;
    }
}
