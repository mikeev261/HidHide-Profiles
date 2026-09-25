using System.Diagnostics;
using System.Security.Principal;
using HidHide.Installer;

// The shipped custom actions run on .NET Framework inside a DTF AppDomain that
// is unloaded when PrepareUser returns, while the ordinary-user helper stays
// alive until ApplyDriver releases it. Nothing started for the helper's
// diagnostics may outlive the readiness wait, or the unload stalls and fails.
static class CustomActionHostChecks
{
    public const string LiveHelper = "--maintenance-live-helper";

    public static int Run()
    {
        using var identity = WindowsIdentity.GetCurrent();
        if (MaintenanceUser.TokenNumber(identity.AccessToken, 20) != 0)
        {
            Console.WriteLine("Custom action host checks skipped: they need an ordinary (non-elevated) test process.");
            return 0;
        }
        var domain = AppDomain.CreateDomain("HidHide custom action host check");
        int helper = 0;
        try
        {
            var runner = (HelperDomainRunner)domain.CreateInstanceAndUnwrap(typeof(HelperDomainRunner).Assembly.FullName, typeof(HelperDomainRunner).FullName);
            helper = runner.PrepareLiveHelper(Process.GetCurrentProcess().MainModule!.FileName, identity.User!.Value);
            var clock = Stopwatch.StartNew();
            AppDomain.Unload(domain);
            if (clock.ElapsedMilliseconds > 5000) throw new Exception("Custom action AppDomain unload stalled for " + clock.ElapsedMilliseconds + " ms.");
            domain = null;
            using var alive = Process.GetProcessById(helper);
            if (alive.HasExited) throw new Exception("The ready helper did not stay alive across the custom action unload.");
            return 2;
        }
        catch (CannotUnloadAppDomainException error)
        {
            domain = null;
            throw new Exception("Custom action AppDomain could not unload after helper readiness: " + error.Message);
        }
        finally
        {
            if (domain != null) try { AppDomain.Unload(domain); } catch (CannotUnloadAppDomainException) { }
            if (helper != 0) try { using var process = Process.GetProcessById(helper); process.Kill(); process.WaitForExit(5000); } catch (ArgumentException) { } catch (InvalidOperationException) { }
        }
    }

    // Stand-in for HidHideCLI --maintenance-msi: it becomes ready and then
    // waits for release with its stderr pipe still open.
    public static void RunLiveHelper()
    {
        Console.Error.WriteLine("maintenance helper waiting for release");
        Thread.Sleep(TimeSpan.FromMinutes(2));
    }
}

// Mirrors PrepareUser inside a separate AppDomain: start, wait for readiness
// and release the diagnostics capture while the helper is still running.
sealed class HelperDomainRunner : MarshalByRefObject
{
    public int PrepareLiveHelper(string executable, string sid)
    {
        using var user = new MaintenanceUser(sid);
        using var helper = user.Start(executable, CustomActionHostChecks.LiveHelper, out var errors);
        var started = Stopwatch.StartNew();
        try { HelperReadiness.Wait(helper, errors, () => { Thread.Sleep(50); return started.ElapsedMilliseconds > 500; }, TimeSpan.FromSeconds(20)); }
        finally { ((object)errors as IDisposable)?.Dispose(); }
        if (helper.HasExited) throw new Exception("Live helper exited before readiness.");
        return helper.Id;
    }
}
