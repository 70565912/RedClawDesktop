param(
    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 20,

    [ValidateRange(1, 240)]
    [int]$TargetFps = 60,

    [ValidateRange(320, 7680)]
    [int]$Width = 1600,

    [ValidateRange(240, 4320)]
    [int]$Height = 900,

    [ValidateRange(4, 128)]
    [int]$SpriteCount = 28,

    [int]$RandomSeed = 2700,

    [ValidateSet('Static', 'Dynamic')]
    [string]$Scene = 'Dynamic',

    [switch]$Fullscreen,

    [switch]$TopMost,

    [switch]$VerifyOnly,

    [string]$ReportFile,

    [string]$ReadyFile
)

$ErrorActionPreference = "Stop"

# Windows Forms Add-Type resolution is not reliable under PowerShell 7's .NET
# runtime on every developer machine. Keep one implementation of the scene and
# synchronously relay Core invocations to the inbox Windows PowerShell runtime.
if ($PSVersionTable.PSEdition -eq "Core") {
    $windowsDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::Windows)
    $windowsPowerShell = Join-Path $windowsDirectory "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $windowsPowerShell -PathType Leaf)) {
        throw "Windows PowerShell is required to run the local high-motion scene."
    }

    $forwardedArguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $PSCommandPath,
        "-DurationSeconds", $DurationSeconds,
        "-TargetFps", $TargetFps,
        "-Width", $Width,
        "-Height", $Height,
        "-SpriteCount", $SpriteCount,
        "-RandomSeed", $RandomSeed,
        "-Scene", $Scene
    )
    if ($Fullscreen.IsPresent) {
        $forwardedArguments += "-Fullscreen"
    }
    if ($TopMost.IsPresent) {
        $forwardedArguments += "-TopMost"
    }
    if ($VerifyOnly.IsPresent) { $forwardedArguments += '-VerifyOnly' }
    if (-not [string]::IsNullOrWhiteSpace($ReportFile)) {
        $forwardedArguments += @("-ReportFile", $ReportFile)
    }
    if ($ReadyFile) { $forwardedArguments += @('-ReadyFile', $ReadyFile) }

    & $windowsPowerShell @forwardedArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Windows PowerShell high-motion scene failed with exit code $LASTEXITCODE."
    }
    return
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

if (-not $ReportFile) {
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $ReportFile = Join-Path $PWD "build/reports/local-high-motion-scene-$timestamp/scene-report.json"
}

$reportFilePath = if ([System.IO.Path]::IsPathRooted($ReportFile)) {
    $ReportFile
} else {
    Join-Path $PWD $ReportFile
}

$reportDir = Split-Path -Parent $reportFilePath
if ($reportDir -and -not (Test-Path $reportDir)) {
    New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
}

$motionSceneType = @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

public sealed class MotionSprite {
    public float OriginX;
    public float OriginY;
    public float X;
    public float Y;
    public float Width;
    public float Height;
    public float VelocityX;
    public float VelocityY;
    public Color Fill;
    public Color Accent;
    public bool Ellipse;
}

public sealed class HighMotionSceneForm : Form {
    private readonly Timer timer_ = new Timer();
    private readonly Stopwatch stopwatch_ = new Stopwatch();
    private readonly List<MotionSprite> sprites_ = new List<MotionSprite>();
    private Random random_;
    private readonly int seed_;
    private readonly bool static_;
    private readonly Font titleFont_ = new Font("Consolas", 30.0f, FontStyle.Bold, GraphicsUnit.Pixel);
    private readonly Font detailFont_ = new Font("Consolas", 16.0f, FontStyle.Bold, GraphicsUnit.Pixel);
    private readonly int targetFps_;
    private readonly double autoCloseSeconds_;
    private readonly int spriteCount_;
    private readonly bool fullscreen_;
    private long sceneFrame_;
    private double lastFrameSeconds_;
    private long renderedFrameCount_;
    private long updateTickCount_;
    private double maxFrameDeltaMs_;
    private double totalFrameDeltaMs_;

    public HighMotionSceneForm(
        int width,
        int height,
        int targetFps,
        int spriteCount,
        int seed,
        bool staticScene,
        double durationSeconds,
        bool fullscreen,
        bool topMost) {
        targetFps_ = Math.Max(1, targetFps);
        autoCloseSeconds_ = Math.Max(1.0, durationSeconds);
        spriteCount_ = Math.Max(4, spriteCount);
        seed_ = seed;
        static_ = staticScene;
        fullscreen_ = fullscreen;

        Text = "RedClawDesktop Local Motion Scene";
        BackColor = Color.Black;
        ForeColor = Color.White;
        TopMost = topMost;
        KeyPreview = true;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(Math.Max(320, width), Math.Max(240, height));
        MinimumSize = new Size(640, 360);
        FormBorderStyle = fullscreen ? FormBorderStyle.None : FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        WindowState = fullscreen ? FormWindowState.Maximized : FormWindowState.Normal;
        DoubleBuffered = true;
        SetStyle(
            ControlStyles.AllPaintingInWmPaint
            | ControlStyles.UserPaint
            | ControlStyles.OptimizedDoubleBuffer,
            true);
        UpdateStyles();

        timer_.Interval = Math.Max(1, (int)Math.Round(1000.0 / targetFps_));
        timer_.Tick += OnTick;
        Shown += OnShown;
        FormClosed += OnClosed;
        KeyDown += OnKeyDown;
        Resize += OnResize;

        ResetSprites();
    }

    public long RenderedFrameCount {
        get { return renderedFrameCount_; }
    }

    public long SceneFrame { get { return sceneFrame_; } }

    public string RenderHash(long frame) {
        sceneFrame_ = static_ ? 0 : frame;
        UpdateSprites((double)sceneFrame_ / targetFps_);
        using (Bitmap bitmap = new Bitmap(ClientSize.Width, ClientSize.Height))
        using (Graphics graphics = Graphics.FromImage(bitmap))
        using (System.IO.MemoryStream bytes = new System.IO.MemoryStream())
        using (System.Security.Cryptography.SHA256 hash = System.Security.Cryptography.SHA256.Create()) {
            OnPaint(new PaintEventArgs(graphics, ClientRectangle));
            bitmap.Save(bytes, System.Drawing.Imaging.ImageFormat.Png);
            return BitConverter.ToString(hash.ComputeHash(bytes.ToArray())).Replace("-", "").ToLowerInvariant();
        }
    }

    public long UpdateTickCount {
        get { return updateTickCount_; }
    }

    public double ElapsedSeconds {
        get { return stopwatch_.Elapsed.TotalSeconds; }
    }

    public double AverageFps {
        get {
            double elapsed = ElapsedSeconds;
            return elapsed > 0.0 ? renderedFrameCount_ / elapsed : 0.0;
        }
    }

    public double AverageFrameDeltaMs {
        get {
            return renderedFrameCount_ > 1 ? totalFrameDeltaMs_ / Math.Max(1, renderedFrameCount_ - 1) : 0.0;
        }
    }

    public double MaxFrameDeltaMs {
        get { return maxFrameDeltaMs_; }
    }

    public int TargetFps {
        get { return targetFps_; }
    }

    public int SpriteCount {
        get { return spriteCount_; }
    }

    public bool Fullscreen {
        get { return fullscreen_; }
    }

    private void OnShown(object sender, EventArgs e) {
        stopwatch_.Restart();
        sceneFrame_ = 0;
        lastFrameSeconds_ = 0.0;
        timer_.Start();
    }

    private void OnClosed(object sender, FormClosedEventArgs e) {
        timer_.Stop();
        stopwatch_.Stop();
    }

    private void OnKeyDown(object sender, KeyEventArgs e) {
        if (e.KeyCode == Keys.Escape) {
            Close();
        }
    }

    private void OnResize(object sender, EventArgs e) {
        if (!Visible) {
            return;
        }

        if (ClientSize.Width < 320 || ClientSize.Height < 240) {
            return;
        }

        ResetSprites();
    }

    private void OnTick(object sender, EventArgs e) {
        double elapsedSeconds = stopwatch_.Elapsed.TotalSeconds;
        // Absolute frame time skips late timer ticks without changing trajectories.
        // Every rendering of frame N has identical content for this seed/geometry.
        sceneFrame_ = static_ ? 0 : (long)Math.Floor(elapsedSeconds * targetFps_);
        UpdateSprites((double)sceneFrame_ / targetFps_);
        ++updateTickCount_;
        if (!static_) Invalidate();
        if (elapsedSeconds >= autoCloseSeconds_) {
            Close();
        }
    }

    protected override void OnPaint(PaintEventArgs e) {
        base.OnPaint(e);

        Graphics graphics = e.Graphics;
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        graphics.CompositingQuality = CompositingQuality.HighSpeed;
        graphics.InterpolationMode = InterpolationMode.Low;
        graphics.PixelOffsetMode = PixelOffsetMode.HighSpeed;

        Rectangle bounds = ClientRectangle;
        if (bounds.Width <= 0 || bounds.Height <= 0) {
            return;
        }

        double elapsedSeconds = stopwatch_.Elapsed.TotalSeconds;
        double sceneSeconds = (double)sceneFrame_ / targetFps_;
        DrawBackground(graphics, bounds, sceneSeconds);
        DrawStripes(graphics, bounds, sceneSeconds);
        DrawSweep(graphics, bounds, sceneSeconds);
        DrawSprites(graphics);
        DrawHud(graphics, bounds, sceneSeconds);

        if (lastFrameSeconds_ > 0.0) {
            double frameDeltaMs = Math.Max(0.0, (elapsedSeconds - lastFrameSeconds_) * 1000.0);
            totalFrameDeltaMs_ += frameDeltaMs;
            if (frameDeltaMs > maxFrameDeltaMs_) {
                maxFrameDeltaMs_ = frameDeltaMs;
            }
        }
        lastFrameSeconds_ = elapsedSeconds;
        ++renderedFrameCount_;
    }

    private void ResetSprites() {
        sprites_.Clear();
        random_ = new Random(seed_);
        int width = Math.Max(1, ClientSize.Width);
        int height = Math.Max(1, ClientSize.Height);
        for (int index = 0; index < spriteCount_; ++index) {
            float spriteWidth = random_.Next(32, 140);
            float spriteHeight = random_.Next(32, 140);
            MotionSprite sprite = new MotionSprite();
            sprite.X = random_.Next(0, Math.Max(1, width - (int)spriteWidth));
            sprite.Y = random_.Next(0, Math.Max(1, height - (int)spriteHeight));
            sprite.OriginX = sprite.X;
            sprite.OriginY = sprite.Y;
            sprite.Width = spriteWidth;
            sprite.Height = spriteHeight;
            sprite.VelocityX = (float)(random_.NextDouble() * 900.0 + 240.0) * (index % 2 == 0 ? 1.0f : -1.0f);
            sprite.VelocityY = (float)(random_.NextDouble() * 700.0 + 180.0) * (index % 3 == 0 ? 1.0f : -1.0f);
            sprite.Fill = Color.FromArgb(
                180,
                random_.Next(32, 256),
                random_.Next(32, 256),
                random_.Next(32, 256));
            sprite.Accent = Color.FromArgb(
                255,
                Math.Min(255, sprite.Fill.R + 40),
                Math.Min(255, sprite.Fill.G + 40),
                Math.Min(255, sprite.Fill.B + 40));
            sprite.Ellipse = index % 3 == 0;
            sprites_.Add(sprite);
        }
        UpdateSprites((double)sceneFrame_ / targetFps_);
    }

    private static float ReflectedPosition(double position, double span) {
        if (span <= 0.0) return 0.0f;
        double phase = ((position % (2.0 * span)) + 2.0 * span) % (2.0 * span);
        return (float)(phase <= span ? phase : 2.0 * span - phase);
    }

    private void UpdateSprites(double sceneSeconds) {
        float width = Math.Max(1, ClientSize.Width);
        float height = Math.Max(1, ClientSize.Height);
        foreach (MotionSprite sprite in sprites_) {
            sprite.X = ReflectedPosition(sprite.OriginX + sprite.VelocityX * sceneSeconds, width - sprite.Width);
            sprite.Y = ReflectedPosition(sprite.OriginY + sprite.VelocityY * sceneSeconds, height - sprite.Height);
        }
    }

    private void DrawBackground(Graphics graphics, Rectangle bounds, double elapsedSeconds) {
        using (LinearGradientBrush background = new LinearGradientBrush(
            bounds,
            Color.FromArgb(12, 18, 28),
            Color.FromArgb(12, 12, 16),
            35.0f)) {
            graphics.FillRectangle(background, bounds);
        }

        int gridSize = 48;
        int offsetX = (int)((elapsedSeconds * 240.0) % gridSize);
        int offsetY = (int)((elapsedSeconds * 160.0) % gridSize);
        using (Pen gridPen = new Pen(Color.FromArgb(55, 90, 180, 220), 1.0f)) {
            for (int x = -gridSize; x < bounds.Width + gridSize; x += gridSize) {
                graphics.DrawLine(gridPen, x + offsetX, 0, x + offsetX, bounds.Height);
            }
            for (int y = -gridSize; y < bounds.Height + gridSize; y += gridSize) {
                graphics.DrawLine(gridPen, 0, y + offsetY, bounds.Width, y + offsetY);
            }
        }
    }

    private void DrawStripes(Graphics graphics, Rectangle bounds, double elapsedSeconds) {
        int stripeWidth = 120;
        for (int stripe = -2; stripe < (bounds.Width / stripeWidth) + 3; ++stripe) {
            int x = (int)((stripe * stripeWidth) + ((elapsedSeconds * 420.0) % (stripeWidth * 3)) - stripeWidth);
            using (SolidBrush brush = new SolidBrush(Color.FromArgb(
                55,
                200,
                (40 + stripe * 35) & 255,
                (140 + stripe * 20) & 255))) {
                graphics.FillRectangle(brush, x, 0, stripeWidth / 2, bounds.Height);
            }
        }
    }

    private void DrawSweep(Graphics graphics, Rectangle bounds, double elapsedSeconds) {
        float centerX = bounds.Width / 2.0f;
        float centerY = bounds.Height / 2.0f;
        float radius = (float)Math.Sqrt((bounds.Width * bounds.Width) + (bounds.Height * bounds.Height)) / 2.0f;
        float sweepX = (float)((elapsedSeconds * 600.0) % (bounds.Width + 300.0)) - 150.0f;
        using (LinearGradientBrush brush = new LinearGradientBrush(
            new RectangleF(sweepX - 180.0f, 0.0f, 360.0f, bounds.Height),
            Color.FromArgb(0, 255, 255, 255),
            Color.FromArgb(120, 255, 255, 255),
            0.0f)) {
            graphics.FillRectangle(brush, sweepX - 180.0f, 0.0f, 360.0f, bounds.Height);
        }

        using (Pen pulsePen = new Pen(Color.FromArgb(90, 255, 180, 80), 6.0f)) {
            float pulseRadius = 80.0f + (float)((Math.Sin(elapsedSeconds * 3.4) + 1.0) * 0.5 * radius * 0.55);
            graphics.DrawEllipse(
                pulsePen,
                centerX - pulseRadius,
                centerY - pulseRadius,
                pulseRadius * 2.0f,
                pulseRadius * 2.0f);
        }
    }

    private void DrawSprites(Graphics graphics) {
        foreach (MotionSprite sprite in sprites_) {
            RectangleF rectangle = new RectangleF(sprite.X, sprite.Y, sprite.Width, sprite.Height);
            using (SolidBrush fill = new SolidBrush(sprite.Fill)) {
                if (sprite.Ellipse) {
                    graphics.FillEllipse(fill, rectangle);
                } else {
                    graphics.FillRectangle(fill, rectangle);
                }
            }

            using (Pen outline = new Pen(sprite.Accent, 3.0f)) {
                if (sprite.Ellipse) {
                    graphics.DrawEllipse(outline, rectangle);
                } else {
                    graphics.DrawRectangle(outline, rectangle.X, rectangle.Y, rectangle.Width, rectangle.Height);
                }
            }

            using (Pen cross = new Pen(Color.FromArgb(180, 255, 255, 255), 2.0f)) {
                graphics.DrawLine(cross, rectangle.Left, rectangle.Top, rectangle.Right, rectangle.Bottom);
                graphics.DrawLine(cross, rectangle.Right, rectangle.Top, rectangle.Left, rectangle.Bottom);
            }
        }
    }

    private void DrawHud(Graphics graphics, Rectangle bounds, double elapsedSeconds) {
        string timecode = string.Format(
            "LOCAL SCENE  SEED {0}  FRAME {1}  SCENE TIME {2:0.000}s",
            seed_,
            sceneFrame_,
            elapsedSeconds);
        string details = string.Format(
            "TARGET {0}  SPRITES {1}  {2}  TEXT AaBb 0123456789  ESC=EXIT",
            TargetFps,
            SpriteCount,
            static_ ? "STATIC" : "DYNAMIC");

        Rectangle titleRect = new Rectangle(24, 20, bounds.Width - 48, 42);
        Rectangle detailRect = new Rectangle(24, 64, bounds.Width - 48, 28);
        using (SolidBrush shadow = new SolidBrush(Color.FromArgb(160, 0, 0, 0))) {
            graphics.DrawString(timecode, titleFont_, shadow, titleRect.X + 2, titleRect.Y + 2);
            graphics.DrawString(details, detailFont_, shadow, detailRect.X + 2, detailRect.Y + 2);
        }
        using (SolidBrush text = new SolidBrush(Color.FromArgb(255, 255, 244, 214))) {
            graphics.DrawString(timecode, titleFont_, text, titleRect.Location);
            graphics.DrawString(details, detailFont_, text, detailRect.Location);
        }

        int barHeight = 18;
        int bottomY = bounds.Height - 32;
        int segmentWidth = Math.Max(24, bounds.Width / 12);
        for (int index = 0; index < 12; ++index) {
            int pulse = (int)((Math.Sin((elapsedSeconds * 5.0) + index * 0.5) + 1.0) * 0.5 * 180.0) + 40;
            using (SolidBrush brush = new SolidBrush(Color.FromArgb(220, pulse, 40 + (index * 12), 255 - (index * 14)))) {
                graphics.FillRectangle(brush, 24 + (index * segmentWidth), bottomY, segmentWidth - 8, barHeight);
            }
        }
    }
}
"@

Add-Type -ReferencedAssemblies @(
    "System.Windows.Forms",
    "System.Drawing"
) -TypeDefinition $motionSceneType

$form = [HighMotionSceneForm]::new(
    $Width,
    $Height,
    $TargetFps,
    $SpriteCount,
    $RandomSeed,
    ($Scene -eq 'Static'),
    [double]$DurationSeconds,
    $Fullscreen.IsPresent,
    $TopMost.IsPresent)

if ($VerifyOnly.IsPresent) {
    $zero = $form.RenderHash(0)
    $later = $form.RenderHash(120)
    $repeat = $form.RenderHash(0)
    $passed = $zero -eq $repeat -and (($Scene -eq 'Static') -eq ($zero -eq $later))
    [pscustomobject]@{
        schema_version = 2; ok = $passed; verification_only = $true
        random_seed = $RandomSeed; scene = $Scene
        frame_zero_sha256 = $zero; frame_120_sha256 = $later; repeat_zero_sha256 = $repeat
        width = $form.ClientSize.Width; height = $form.ClientSize.Height
    } | ConvertTo-Json | Set-Content -LiteralPath $reportFilePath -Encoding UTF8
    $form.Dispose()
    if (-not $passed) { throw 'Deterministic scene verification failed.' }
    Write-Host 'Deterministic offscreen scene verification passed; no desktop load was started.'
    return
}

if ($ReadyFile) {
    $form.add_Shown({
        [pscustomobject]@{schema_version=2; ready=$true; width=$form.ClientSize.Width; height=$form.ClientSize.Height} |
            ConvertTo-Json | Set-Content -LiteralPath $ReadyFile -Encoding UTF8
    })
}
[void]$form.ShowDialog()

$report = [pscustomobject]@{
    schema_version = 2
    ok = $true
    scene = $Scene
    random_seed = $RandomSeed
    trajectory = 'absolute-frame-reflection-v1'
    scene_frame = $form.SceneFrame
    elapsed_seconds = $form.ElapsedSeconds
    duration_seconds = $DurationSeconds
    target_fps = $TargetFps
    measured_fps = [Math]::Round($form.AverageFps, 2)
    average_frame_delta_ms = [Math]::Round($form.AverageFrameDeltaMs, 2)
    max_frame_delta_ms = [Math]::Round($form.MaxFrameDeltaMs, 2)
    rendered_frames = $form.RenderedFrameCount
    update_ticks = $form.UpdateTickCount
    width = $form.ClientSize.Width
    height = $form.ClientSize.Height
    sprite_count = $SpriteCount
    fullscreen = $Fullscreen.IsPresent
    top_most = $TopMost.IsPresent
    report_generated_at = (Get-Date).ToString("o")
}

$report | ConvertTo-Json -Depth 4 | Set-Content -Path $reportFilePath -Encoding ASCII

Write-Host "Local high-motion scene completed."
Write-Host "Report: $reportFilePath"
Write-Host "Measured FPS: $($report.measured_fps)"
$form.Dispose()
