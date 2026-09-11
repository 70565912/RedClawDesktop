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

    [switch]$Fullscreen,

    [switch]$TopMost,

    [string]$ReportFile
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
        "-SpriteCount", $SpriteCount
    )
    if ($Fullscreen.IsPresent) {
        $forwardedArguments += "-Fullscreen"
    }
    if ($TopMost.IsPresent) {
        $forwardedArguments += "-TopMost"
    }
    if (-not [string]::IsNullOrWhiteSpace($ReportFile)) {
        $forwardedArguments += @("-ReportFile", $ReportFile)
    }

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
    private readonly Random random_ = new Random();
    private readonly Font titleFont_ = new Font("Consolas", 30.0f, FontStyle.Bold, GraphicsUnit.Pixel);
    private readonly Font detailFont_ = new Font("Consolas", 16.0f, FontStyle.Bold, GraphicsUnit.Pixel);
    private readonly int targetFps_;
    private readonly double autoCloseSeconds_;
    private readonly int spriteCount_;
    private readonly bool fullscreen_;
    private double lastUpdateSeconds_;
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
        double durationSeconds,
        bool fullscreen,
        bool topMost) {
        targetFps_ = Math.Max(1, targetFps);
        autoCloseSeconds_ = Math.Max(1.0, durationSeconds);
        spriteCount_ = Math.Max(4, spriteCount);
        fullscreen_ = fullscreen;

        Text = "RedClawDesktop Local Motion Scene";
        BackColor = Color.Black;
        ForeColor = Color.White;
        TopMost = topMost;
        KeyPreview = true;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(Math.Max(320, width), Math.Max(240, height));
        MinimumSize = new Size(640, 360);
        FormBorderStyle = fullscreen ? FormBorderStyle.None : FormBorderStyle.Sizable;
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
        lastUpdateSeconds_ = 0.0;
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
        double deltaSeconds = lastUpdateSeconds_ > 0.0
            ? Math.Max(0.001, elapsedSeconds - lastUpdateSeconds_)
            : (1.0 / targetFps_);
        lastUpdateSeconds_ = elapsedSeconds;
        UpdateSprites(deltaSeconds);
        ++updateTickCount_;
        Invalidate();
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
        DrawBackground(graphics, bounds, elapsedSeconds);
        DrawStripes(graphics, bounds, elapsedSeconds);
        DrawSweep(graphics, bounds, elapsedSeconds);
        DrawSprites(graphics);
        DrawHud(graphics, bounds, elapsedSeconds);

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
        int width = Math.Max(1, ClientSize.Width);
        int height = Math.Max(1, ClientSize.Height);
        for (int index = 0; index < spriteCount_; ++index) {
            float spriteWidth = random_.Next(32, 140);
            float spriteHeight = random_.Next(32, 140);
            MotionSprite sprite = new MotionSprite();
            sprite.X = random_.Next(0, Math.Max(1, width - (int)spriteWidth));
            sprite.Y = random_.Next(0, Math.Max(1, height - (int)spriteHeight));
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
    }

    private void UpdateSprites(double deltaSeconds) {
        float width = Math.Max(1, ClientSize.Width);
        float height = Math.Max(1, ClientSize.Height);
        foreach (MotionSprite sprite in sprites_) {
            sprite.X += sprite.VelocityX * (float)deltaSeconds;
            sprite.Y += sprite.VelocityY * (float)deltaSeconds;

            if (sprite.X < 0.0f) {
                sprite.X = 0.0f;
                sprite.VelocityX = Math.Abs(sprite.VelocityX);
            } else if (sprite.X + sprite.Width > width) {
                sprite.X = width - sprite.Width;
                sprite.VelocityX = -Math.Abs(sprite.VelocityX);
            }

            if (sprite.Y < 0.0f) {
                sprite.Y = 0.0f;
                sprite.VelocityY = Math.Abs(sprite.VelocityY);
            } else if (sprite.Y + sprite.Height > height) {
                sprite.Y = height - sprite.Height;
                sprite.VelocityY = -Math.Abs(sprite.VelocityY);
            }
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
            "LOCAL MOTION SCENE  FPS {0:0.0}  FRAMES {1}  ELAPSED {2:0.0}s",
            AverageFps,
            RenderedFrameCount,
            elapsedSeconds);
        string details = string.Format(
            "TARGET {0}  SPRITES {1}  MAX_FRAME_DELTA {2:0.0}ms  AVG_FRAME_DELTA {3:0.0}ms  ESC=EXIT",
            TargetFps,
            SpriteCount,
            MaxFrameDeltaMs,
            AverageFrameDeltaMs);

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
    [double]$DurationSeconds,
    $Fullscreen.IsPresent,
    $TopMost.IsPresent)

[void]$form.ShowDialog()

$report = [pscustomobject]@{
    ok = $true
    duration_seconds = $DurationSeconds
    target_fps = $TargetFps
    measured_fps = [Math]::Round($form.AverageFps, 2)
    average_frame_delta_ms = [Math]::Round($form.AverageFrameDeltaMs, 2)
    max_frame_delta_ms = [Math]::Round($form.MaxFrameDeltaMs, 2)
    rendered_frames = $form.RenderedFrameCount
    update_ticks = $form.UpdateTickCount
    width = $Width
    height = $Height
    sprite_count = $SpriteCount
    fullscreen = $Fullscreen.IsPresent
    top_most = $TopMost.IsPresent
    report_generated_at = (Get-Date).ToString("o")
}

$report | ConvertTo-Json -Depth 4 | Set-Content -Path $reportFilePath -Encoding ASCII

Write-Host "Local high-motion scene completed."
Write-Host "Report: $reportFilePath"
Write-Host "Measured FPS: $($report.measured_fps)"
