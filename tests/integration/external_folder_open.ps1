param(
    [string]$SinanExecutable
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Collections.Concurrent;
using System.Threading;
using System.Runtime.InteropServices;
using System.Text;

public static class ExternalFolderOpen {
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    static extern int SHParseDisplayName(string path, IntPtr binding, out IntPtr pidl,
        uint mask, IntPtr attributes);

    [DllImport("shell32.dll")]
    static extern IntPtr ILFindLastID(IntPtr pidl);

    [DllImport("shell32.dll")]
    static extern int SHOpenFolderAndSelectItems(IntPtr parent, uint count, IntPtr[] children,
        uint flags);

    delegate bool WindowCallback(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    static extern bool EnumWindows(WindowCallback callback, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassName(IntPtr window, StringBuilder name, int size);

    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr window);

    [DllImport("user32.dll")]
    static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);

    public static bool PrioritizeFixture(long frame) {
        return SetWindowPos(new IntPtr(frame), new IntPtr(-1), 0, 0, 0, 0, 0x13);
    }

    [DllImport("user32.dll")]
    static extern bool PostMessage(IntPtr window, uint msg, IntPtr wParam, IntPtr lParam);

    public static void CloseFixture(long frame) {
        SetWindowPos(new IntPtr(frame), new IntPtr(-2), 0, 0, 0, 0, 0x13);
        PostMessage(new IntPtr(frame), 0x10, IntPtr.Zero, IntPtr.Zero);
    }

    [DllImport("user32.dll")]
    static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string name);

    public static long ActiveBrowser(long frame) {
        var window = FindWindowEx(new IntPtr(frame), IntPtr.Zero, "ShellTabWindowClass", null);
        return IsWindowVisible(window) ? window.ToInt64() : 0;
    }

    [DllImport("dwmapi.dll")]
    static extern int DwmGetWindowAttribute(IntPtr window, int attribute, out int value, int size);

    delegate void WindowEvent(IntPtr hook, uint type, IntPtr window, int objectId, int childId, uint thread, uint time);

    [StructLayout(LayoutKind.Sequential)]
    struct Message {
        public IntPtr window;
        public uint message;
        public IntPtr wParam, lParam;
        public uint time;
        public int x, y;
        public uint reserved;
    }

    [DllImport("user32.dll")]
    static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr module, WindowEvent callback, uint pid, uint tid, uint flags);

    [DllImport("user32.dll")]
    static extern bool UnhookWinEvent(IntPtr hook);

    [DllImport("user32.dll")]
    static extern int GetMessage(out Message message, IntPtr window, uint first, uint last);

    [DllImport("user32.dll")]
    static extern bool PeekMessage(out Message message, IntPtr window, uint first, uint last, uint remove);

    [DllImport("user32.dll")]
    static extern bool PostThreadMessage(uint thread, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll")]
    static extern uint GetCurrentThreadId();

    static readonly ConcurrentQueue<long> shown = new ConcurrentQueue<long>();
    static readonly ManualResetEvent watching = new ManualResetEvent(false);
    static Thread observer;
    static uint observerId;
    static IntPtr eventHook;

    public static void StartWatching() {
        observer = new Thread(() => {
            Message message;
            PeekMessage(out message, IntPtr.Zero, 0, 0, 0);
            observerId = GetCurrentThreadId();
            WindowEvent callback = (hook, type, window, objectId, childId, thread, time) => {
                if (objectId != 0 || childId != 0 || !IsWindowVisible(window)) return;
                var name = new StringBuilder(32);
                int cloak;
                if (GetClassName(window, name, name.Capacity) > 0 && name.ToString() == "CabinetWClass" &&
                    DwmGetWindowAttribute(window, 14, out cloak, sizeof(int)) == 0 && cloak == 0)
                    shown.Enqueue(window.ToInt64());
            };
            eventHook = SetWinEventHook(0x8002, 0x8002, IntPtr.Zero, callback, 0, 0, 0);
            watching.Set();
            if (eventHook == IntPtr.Zero) return;
            while (GetMessage(out message, IntPtr.Zero, 0, 0) > 0) {}
            UnhookWinEvent(eventHook);
            GC.KeepAlive(callback);
        });
        observer.IsBackground = true;
        observer.Start();
        if (!watching.WaitOne(3000) || eventHook == IntPtr.Zero)
            throw new InvalidOperationException("Could not observe Explorer window show events.");
    }

    public static long[] DrainShown() {
        var windows = new System.Collections.Generic.List<long>();
        long window;
        while (shown.TryDequeue(out window)) windows.Add(window);
        return windows.ToArray();
    }

    public static void StopWatching() {
        if (observer == null || !observer.IsAlive) return;
        PostThreadMessage(observerId, 0x0012, IntPtr.Zero, IntPtr.Zero);
        observer.Join(3000);
    }

    public static long[] Windows() {
        var windows = new System.Collections.Generic.List<long>();
        EnumWindows((window, parameter) => {
            var name = new StringBuilder(64);
            if (GetClassName(window, name, name.Capacity) > 0 && name.ToString() == "CabinetWClass")
                windows.Add(window.ToInt64());
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }

    public static int Reveal(string directory, string[] files) {
        IntPtr parent = IntPtr.Zero;
        var absolute = new IntPtr[files.Length];
        var children = new IntPtr[files.Length];
        try {
            Marshal.ThrowExceptionForHR(SHParseDisplayName(directory, IntPtr.Zero, out parent, 0, IntPtr.Zero));
            for (int i = 0; i < files.Length; ++i) {
                Marshal.ThrowExceptionForHR(SHParseDisplayName(files[i], IntPtr.Zero, out absolute[i], 0, IntPtr.Zero));
                children[i] = ILFindLastID(absolute[i]);
            }
            return SHOpenFolderAndSelectItems(parent, (uint)children.Length, children, 0);
        } finally {
            foreach (IntPtr item in absolute) Marshal.FreeCoTaskMem(item);
            Marshal.FreeCoTaskMem(parent);
        }
    }
}
'@

if ($SinanExecutable) {
    $SinanExecutable = (Resolve-Path -LiteralPath $SinanExecutable).Path
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$root = Join-Path $repo ('build\external-folder-open-' + [Guid]::NewGuid().ToString('N'))
$data = Join-Path $root 'data'
$shell = New-Object -ComObject Shell.Application
$originalWindows = @([ExternalFolderOpen]::Windows())
$testWindows = [System.Collections.Generic.HashSet[long]]::new()
$failures = [System.Collections.Generic.List[string]]::new()
$ownsAgent = $false

function Get-FolderView([string]$Path, [long]$ExpectedWindow = 0) {
    foreach ($window in @($shell.Windows())) {
        try {
            $document = $window.Document
            if ($document.Folder.Self.Path -eq $Path -and (!$ExpectedWindow -or [long]$window.HWND -eq $ExpectedWindow)) {
                return [pscustomobject]@{
                    Window = [long]$window.HWND
                    Selected = @($document.SelectedItems() | ForEach-Object { $_.Path } | Sort-Object)
                }
            }
        } catch {
            # Entries can disappear while Explorer registers or closes a view.
        }
    }
}

function Invoke-Sinan {
    $output = & $SinanExecutable --data-dir $data @args
    if ($LASTEXITCODE -ne 0) {
        throw "Sinan $args failed with exit code $LASTEXITCODE. $output"
    }
    return $output
}

function New-Fixture([string]$Name) {
    $path = Join-Path $root $Name
    [IO.Directory]::CreateDirectory($path) | Out-Null
    return $path
}

function Wait-Folder([string]$Path, [long]$ExpectedWindow = 0, [string[]]$Selected = @()) {
    $end = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $view = Get-FolderView $Path $ExpectedWindow
        if ($view -and $view.Window -notin $originalWindows) {
            [void]$testWindows.Add($view.Window)
        }
        if ($Selected.Count) {
            if ($view -and (!$ExpectedWindow -or $view.Window -eq $ExpectedWindow) -and
                ($view.Selected -join "`n") -ceq (($Selected | Sort-Object) -join "`n")) {
                return $view.Window
            }
        } elseif ($ExpectedWindow) {
            $result = Invoke-Sinan tabs list --json | ConvertFrom-Json
            $tab = $result.tabs | Where-Object { $_.path -eq $Path -and [long]$_.window -eq $ExpectedWindow } | Select-Object -First 1
            if ($tab) {
                $handle = [long]$tab.window
                if ($handle -notin $originalWindows) { [void]$testWindows.Add($handle) }
                return $handle
            }
        } elseif ($view) {
            return $view.Window
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $end)
    throw "The expected folder or selection did not appear in $Path."
}

function Test-Open([string]$Name, [string]$Verb, [string[]]$Files, [long]$ExpectedTabWindow = 0,
    [long]$ExistingBrowser = 0, [string]$Fixture = $Name) {
    $path = New-Fixture $Fixture
    $before = @([ExternalFolderOpen]::Windows())
    try {
        if ($ExpectedTabWindow) {
            if (![ExternalFolderOpen]::BringWindowToTop([IntPtr]$ExpectedTabWindow)) {
                throw 'Could not activate the target test window.'
            }
            $redirected = (Invoke-Sinan explorer external-tabs status --json | ConvertFrom-Json).listener.redirected
        }
        [void][ExternalFolderOpen]::DrainShown()
        $selected = @($Files | ForEach-Object {
            $file = Join-Path $path $_
            [IO.File]::WriteAllText($file, 'fixture')
            $file
        })
        if ($selected.Count) {
            $hr = [ExternalFolderOpen]::Reveal($path, $selected)
            if ($hr -ne 0) { throw ('SHOpenFolderAndSelectItems returned 0x{0:X8}.' -f $hr) }
        } else {
            $shell.ShellExecute($path, '', '', $Verb, 1)
        }

        $window = Wait-Folder $path $ExpectedTabWindow $selected
        if ($ExpectedTabWindow) {
            if ($window -ne $ExpectedTabWindow) {
                throw "Expected tab in window $ExpectedTabWindow, got $window."
            }
            $deadline = [DateTime]::UtcNow.AddSeconds(4)
            do {
                $status = Invoke-Sinan explorer external-tabs status --json | ConvertFrom-Json
                $added = @([ExternalFolderOpen]::Windows() | Where-Object { $_ -notin $before })
                if ($ExistingBrowser) {
                    if (!$added.Count -and [ExternalFolderOpen]::ActiveBrowser($ExpectedTabWindow) -eq $ExistingBrowser) { break }
                } elseif ($status.listener.redirected -gt $redirected -and !$added.Count) { break }
                Start-Sleep -Milliseconds 80
            } while ([DateTime]::UtcNow -lt $deadline)
            if ((!$ExistingBrowser -and $status.listener.redirected -le $redirected) -or $added.Count) {
                throw "Source window transfer was not committed. Remaining top-level windows: $added."
            }
            $tabs = (Invoke-Sinan tabs list --json | ConvertFrom-Json).tabs
            $matches = @($tabs | Where-Object { $_.path -eq $path -and [long]$_.window -eq $ExpectedTabWindow })
            $destination = $matches | Select-Object -First 1
            $active = [ExternalFolderOpen]::ActiveBrowser($ExpectedTabWindow)
            if (!$destination -or !$active -or $active -ne [long]$destination.browser_window) {
                throw "Expected $path to be the active view, got browser window $active."
            }
            if ($ExistingBrowser -and ($matches.Count -ne 1 -or $active -ne $ExistingBrowser)) {
                throw "Expected the existing browser $ExistingBrowser with no duplicate, got $($matches.Count) matches and active browser $active."
            }
            $flashes = @([ExternalFolderOpen]::DrainShown() | Where-Object { $_ -notin $before })
            if ($flashes.Count) { throw "New Explorer windows became visible: $flashes." }
        } elseif ($window -in $before) {
            throw "Expected a new top-level window, got existing window $window."
        }
        Write-Host "PASS $Name"
    } catch {
        $failures.Add("$Name - $_")
        Write-Host "FAIL $Name - $_"
        $actual = Get-FolderView $path
        if ($actual) { Write-Host ('View ' + ($actual | ConvertTo-Json -Compress)) }
        if ($SinanExecutable) {
            try {
                $rows = (Invoke-Sinan tabs list --json | ConvertFrom-Json).tabs |
                    Where-Object path -eq $path | Select-Object window, browser_window, path
                Write-Host ('Native tabs ' + (ConvertTo-Json -InputObject @($rows) -Compress))
                Write-Host ('Routing ' + (Invoke-Sinan explorer external-tabs status --json | ConvertFrom-Json | ConvertTo-Json -Compress))
            } catch {
                Write-Host "Could not read routing diagnostics. $_"
            }
        }
    }
}

try {
    [ExternalFolderOpen]::StartWatching()
    $anchor = New-Fixture 'anchor'
    $shell.ShellExecute($anchor, '', '', 'opennewwindow', 1)
    $anchorWindow = Wait-Folder $anchor
    if ($anchorWindow -in $originalWindows) {
        throw 'The explicit test anchor did not open in a new Explorer window.'
    }

    $target = 0
    if ($SinanExecutable) {
        if (![ExternalFolderOpen]::PrioritizeFixture($anchorWindow)) { throw 'Could not prioritize the test window.' }
        $ownsAgent = $true
        $ready = Invoke-Sinan explorer external-tabs on --json | ConvertFrom-Json
        if (!$ready.listener.active -or !$ready.listener.activation.active) {
            throw ('External folder routing did not attach. ' + ($ready.listener | ConvertTo-Json -Compress))
        }
        $target = $anchorWindow
    }

    Test-Open 'Downloads' 'open' @() $target
    Test-Open 'select-one' '' @('selected file.txt') $target
    if ($target -and !$failures.Count) {
        $downloads = Join-Path $root 'Downloads'
        $tabs = (Invoke-Sinan tabs list --json | ConvertFrom-Json).tabs
        $existing = $tabs | Where-Object { $_.path -eq $downloads -and [long]$_.window -eq $target } | Select-Object -First 1
        if (!$existing -or [ExternalFolderOpen]::ActiveBrowser($target) -eq [long]$existing.browser_window) {
            throw 'The Downloads fixture must already exist in a background tab.'
        }
        Test-Open 'reuse-downloads' 'open' @() $target $existing.browser_window 'Downloads'
        $anchorTab = $tabs | Where-Object { $_.path -eq $anchor -and [long]$_.window -eq $target } | Select-Object -First 1
        if (!$anchorTab) { throw 'Lost the original test anchor tab.' }
        $locate = @{ tab_id = $anchorTab.tab_id } | ConvertTo-Json -Compress
        Invoke-Sinan actions run tabs.locate --parameters $locate | Out-Null
        if ([ExternalFolderOpen]::ActiveBrowser($target) -ne [long]$anchorTab.browser_window) {
            throw 'The test anchor must be active before revealing a file in Downloads.'
        }
        Test-Open 'reveal-in-existing-downloads' '' @('browser download.txt') $target $existing.browser_window 'Downloads'
    }
    Test-Open 'select-many' '' @('first.txt', 'second file.txt') $target
    Test-Open 'open-new-window' 'opennewwindow' @()

    if ($ownsAgent) {
        Invoke-Sinan explorer external-tabs off --json | Out-Null
        Test-Open 'disabled-open-directory' 'open' @()
    }
    if ($failures.Count) { throw ($failures -join "`n") }
} finally {
    [ExternalFolderOpen]::StopWatching()
    try {
        if ($ownsAgent) { Invoke-Sinan agent stop --json | Out-Null }
    } finally {
        foreach ($window in $testWindows) { [ExternalFolderOpen]::CloseFixture($window) }
    }
}
