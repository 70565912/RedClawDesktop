param([Parameter(Mandatory=$true)][int]$ProcessId)
$ErrorActionPreference='Stop'
$process=Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId"
if($process.Name -ne 'powershell.exe' -or $process.CommandLine -notlike '*run-local-high-motion-scene.ps1*') {
    throw 'QA scene process identity mismatch.'
}
if(-not ('RedClawQaSceneObserver' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
public sealed class RedClawQaSceneObserver : IDisposable {
    public sealed class Sample {
        public long at_us;
        public uint pid;
        public int left, top, right, bottom;
        public bool visible, minimized;
        public string Identity { get { return pid+":"+left+","+top+","+right+","+bottom+":"+visible+":"+minimized; } }
    }
    public readonly long hwnd;
    public readonly Sample initial;
    public Sample last;
    public readonly List<Sample> changes = new List<Sample>();
    public long samples;
    public int overflow;
    private readonly IntPtr window;
    private readonly ManualResetEvent stop = new ManualResetEvent(false);
    private readonly Thread thread;
    private delegate bool Callback(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback c, IntPtr p);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int left,top,right,bottom; }
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr h,out Rect r);
    private Sample Read() {
        uint pid; Rect r; GetWindowThreadProcessId(window,out pid); GetWindowRect(window,out r);
        return new Sample { at_us=(long)(Stopwatch.GetTimestamp()*(1000000.0/Stopwatch.Frequency)),
            pid=pid,left=r.left,top=r.top,right=r.right,bottom=r.bottom,
            visible=IsWindowVisible(window),minimized=IsIconic(window) };
    }
    public RedClawQaSceneObserver(uint pid) {
        var windows=new List<IntPtr>();
        EnumWindows((h,p)=>{ uint owner; GetWindowThreadProcessId(h,out owner);
            var name=new StringBuilder(256); GetWindowText(h,name,256);
            if(owner==pid && name.ToString()=="RedClawDesktop Local Motion Scene") windows.Add(h);
            return true; },IntPtr.Zero);
        if(windows.Count!=1) throw new InvalidOperationException("Expected exactly one owned scene window.");
        window=windows[0]; hwnd=window.ToInt64();
        var previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
        try { initial=last=Read(); } finally { SetThreadDpiAwarenessContext(previous); }
        if(!initial.visible || initial.minimized) throw new InvalidOperationException("Scene must be visible.");
        thread=new Thread(()=>{
            SetThreadDpiAwarenessContext(new IntPtr(-4));
            do {
                var sample=Read(); ++samples;
                if(sample.Identity!=last.Identity) {
                    if(changes.Count<64) changes.Add(sample); else ++overflow;
                }
                last=sample;
            } while(!stop.WaitOne(25));
        });
        thread.IsBackground=true; thread.Name="RedClaw QA scene observer"; thread.Start();
    }
    public void Dispose() { stop.Set(); thread.Join(); stop.Dispose(); }
}
'@
}
return [RedClawQaSceneObserver]::new([uint32]$ProcessId)
