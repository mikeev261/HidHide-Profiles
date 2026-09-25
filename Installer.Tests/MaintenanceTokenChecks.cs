using HidHide.Installer;
using System.Diagnostics;
using System.Security.Principal;

internal static class MaintenanceTokenChecks
{
    public static void Run(int clientProcessId = 0)
    {
        using var identity = WindowsIdentity.GetCurrent();
        string sid = identity.User!.Value;
        bool rejected = false;
        try { using var wrong = new MaintenanceUser("S-1-5-18"); }
        catch (UnauthorizedAccessException) { rejected = true; }
        if (!rejected) throw new Exception("Different user token was accepted.");
        rejected = false;
        try { MaintenanceUser.Validate(identity.AccessToken, sid, 0); }
        catch (UnauthorizedAccessException) { rejected = true; }
        if (!rejected) throw new Exception("Session zero was accepted.");
        rejected = false;
        try { MaintenanceUser.Validate(identity.AccessToken, "S-1-5-18", Process.GetCurrentProcess().SessionId); }
        catch (UnauthorizedAccessException) { rejected = true; }
        if (!rejected) throw new Exception("Mismatched helper SID was accepted.");
        rejected = false;
        try { MaintenanceUser.Validate(identity.AccessToken, sid, Process.GetCurrentProcess().SessionId + 1); }
        catch (UnauthorizedAccessException) { rejected = true; }
        if (!rejected) throw new Exception("Mismatched helper session was accepted.");

        string evidence = Path.Combine(Path.GetTempPath(), "HidHide-token-test-" + Guid.NewGuid().ToString("N") + ".txt");
        string? previous = Environment.GetEnvironmentVariable("HIDHIDE_TEST_PARENT_ONLY");
        Environment.SetEnvironmentVariable("HIDHIDE_TEST_PARENT_ONLY", "must-not-leak");
        try
        {
            using var user = new MaintenanceUser(sid, clientProcessId);
            if (user.Sid != sid || user.SessionId != Process.GetCurrentProcess().SessionId) throw new Exception("Selected token identity/session changed.");
            using var child = user.Start(Path.Combine(AppContext.BaseDirectory, "Installer.Tests.exe"),
                $"--maintenance-user-child {sid} {user.SessionId} \"{evidence}\"");
            if (!child.WaitForExit(15000) || child.ExitCode != 0) throw new Exception("Ordinary child token probe failed.");
            string expected = "True|True|True|" + Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (File.ReadAllText(evidence) != expected) throw new Exception("Child identity, environment or profile directory differs.");
            // A helper that refuses maintenance reports why on stderr; setup
            // must surface that reason and must not tell the user to open the app.
            using var refused = user.Start(Path.Combine(AppContext.BaseDirectory, "Installer.Tests.exe"),
                "--maintenance-user-failing-child", out var refusedErrors);
            string failure = ReadinessFailure(refused, refusedErrors);
            if (!failure.Contains("(code 1627)") || !failure.Contains("Close the HidHide Profiles editor before running setup. Apply or discard its unsaved changes first"))
                throw new Exception("Helper refusal reason was not reported: " + failure);
            if (failure.Contains("Open HidHide Profiles") || !failure.Contains("Close the HidHide Profiles editor and any application launched from it"))
                throw new Exception("Helper failure guidance is misleading: " + failure);
            // Unbounded helper output neither blocks the helper nor the report.
            using var flooding = user.Start(Path.Combine(AppContext.BaseDirectory, "Installer.Tests.exe"),
                "--maintenance-user-failing-child flood", out var floodingErrors);
            failure = ReadinessFailure(flooding, floodingErrors);
            if (!failure.Contains("(code 1627)") || failure.Length > 2048) throw new Exception("Flooding helper output was not bounded: " + failure.Length);
            if (HelperErrorOutput.Sanitize("a\u0007[ProgramFilesFolder]\r\n{b}\t\tc") != "a (ProgramFilesFolder) (b) c")
                throw new Exception("Helper reason is not reduced to plain MSI-safe text.");
            Console.WriteLine("11 maintenance token checks passed (parent elevated=" + (MaintenanceUser.TokenNumber(identity.AccessToken, 20) != 0) + ").");
        }
        finally
        {
            Environment.SetEnvironmentVariable("HIDHIDE_TEST_PARENT_ONLY", previous);
            if (File.Exists(evidence)) File.Delete(evidence);
        }
    }

    static string ReadinessFailure(Process helper, HelperErrorOutput errors)
    {
        if (!helper.WaitForExit(15000)) throw new Exception("Refusing helper did not exit.");
        try { HelperReadiness.Wait(helper, errors, () => false, TimeSpan.FromSeconds(15)); }
        catch (InvalidOperationException error) { return error.Message; }
        throw new Exception("Exited helper was treated as ready.");
    }
}
