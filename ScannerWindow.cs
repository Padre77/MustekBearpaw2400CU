using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Forms;

internal sealed class ScannerWindow : Form
{
    private static readonly Size Bed = new Size(827, 1169);
    private readonly Button fullPageButton = new Button { Text = "Сканировать A4", AutoSize = true };
    private readonly Button previewButton = new Button { Text = "Предпросмотр", AutoSize = true };
    private readonly Button scanButton = new Button { Text = "Сканировать выделенное", AutoSize = true, Enabled = false };
    private readonly Button saveButton = new Button { Text = "Сохранить PNG", AutoSize = true, Enabled = false };
    private readonly Label status = new Label { AutoSize = true, Padding = new Padding(0, 4, 0, 4) };
    private readonly PictureBox preview = new PictureBox
    {
        Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.FromArgb(34, 39, 44)
    };
    private Bitmap result;
    private Rectangle selection;
    private Point dragStart;
    private bool dragging;
    private bool busy;

    private ScannerWindow(string initialImage)
    {
        Text = "Mustek BearPaw 2400 CU";
        ClientSize = new Size(660, 750);
        MinimumSize = new Size(480, 440);
        var commands = new FlowLayoutPanel
        {
            Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Padding = new Padding(8, 6, 8, 6)
        };
        commands.Controls.AddRange(new Control[] { fullPageButton, previewButton, scanButton, saveButton });
        status.Dock = DockStyle.Bottom;
        status.Text = "Сканируйте A4 сразу или откройте предпросмотр для выбора области.";
        Controls.Add(preview);
        Controls.Add(status);
        Controls.Add(commands);
        fullPageButton.Click += async delegate
        {
            await AcquireImage(new Rectangle(Point.Empty, Bed), true, "Лист A4 готов. Нажмите «Сохранить PNG».");
        };
        previewButton.Click += async delegate
        {
            await AcquireImage(new Rectangle(Point.Empty, Bed), true, "Выделите рамкой нужную область или сохраните весь лист.");
        };
        scanButton.Click += async delegate
        {
            var area = selection;
            await AcquireImage(area, false, String.Format("Фрагмент готов. {0} × {1} пикселей, 100 dpi.", area.Width, area.Height));
        };
        saveButton.Click += delegate { Save(); };
        preview.MouseDown += StartSelection;
        preview.MouseMove += MoveSelection;
        preview.MouseUp += EndSelection;
        preview.Paint += DrawSelection;
        FormClosing += delegate(object sender, FormClosingEventArgs e) { e.Cancel = busy; };
        FormClosed += delegate
        {
            if (preview.Image != null) preview.Image.Dispose();
            if (result != null) result.Dispose();
        };
        if (!String.IsNullOrEmpty(initialImage))
            SetImage(initialImage, true);
    }

    private Rectangle ImageBounds()
    {
        if (preview.Image == null) return Rectangle.Empty;
        double scale = Math.Min((double)preview.ClientSize.Width / Bed.Width, (double)preview.ClientSize.Height / Bed.Height);
        int width = (int)(Bed.Width * scale), height = (int)(Bed.Height * scale);
        return new Rectangle((preview.ClientSize.Width - width) / 2, (preview.ClientSize.Height - height) / 2, width, height);
    }

    private Point ImagePoint(Point point)
    {
        Rectangle bounds = ImageBounds();
        return new Point(
            Math.Max(0, Math.Min(Bed.Width, (point.X - bounds.Left) * Bed.Width / bounds.Width)),
            Math.Max(0, Math.Min(Bed.Height, (point.Y - bounds.Top) * Bed.Height / bounds.Height)));
    }

    private void StartSelection(object sender, MouseEventArgs e)
    {
        if (busy || e.Button != MouseButtons.Left || !ImageBounds().Contains(e.Location)) return;
        dragStart = ImagePoint(e.Location);
        dragging = true;
        preview.Capture = true;
    }

    private void MoveSelection(object sender, MouseEventArgs e)
    {
        if (!dragging) return;
        Point end = ImagePoint(e.Location);
        selection = Rectangle.FromLTRB(Math.Min(dragStart.X, end.X), Math.Min(dragStart.Y, end.Y),
                                       Math.Max(dragStart.X, end.X), Math.Max(dragStart.Y, end.Y));
        preview.Invalidate();
    }

    private void EndSelection(object sender, MouseEventArgs e)
    {
        if (!dragging) return;
        MoveSelection(sender, e);
        dragging = false;
        preview.Capture = false;
        scanButton.Enabled = selection.Width > 0 && selection.Height > 0;
        if (scanButton.Enabled) status.Text = String.Format("Выделено {0} × {1} пикселей.", selection.Width, selection.Height);
    }

    private void DrawSelection(object sender, PaintEventArgs e)
    {
        if (selection.Width <= 0 || selection.Height <= 0) return;
        Rectangle bounds = ImageBounds();
        using (var pen = new Pen(Color.DeepSkyBlue, 2))
            e.Graphics.DrawRectangle(pen, bounds.Left + selection.X * bounds.Width / Bed.Width,
                bounds.Top + selection.Y * bounds.Height / Bed.Height, selection.Width * bounds.Width / Bed.Width,
                selection.Height * bounds.Height / Bed.Height);
    }

    private void SetImage(string path, bool fullPage)
    {
        using (var source = new Bitmap(path))
        {
            if (result != null) result.Dispose();
            result = new Bitmap(source);
            if (fullPage)
            {
                if (preview.Image != null) preview.Image.Dispose();
                preview.Image = new Bitmap(source);
                selection = Rectangle.Empty;
            }
        }
        preview.Invalidate();
        saveButton.Enabled = true;
    }

    private async Task AcquireImage(Rectangle area, bool fullPage, string successMessage)
    {
        if (busy) return;
        busy = true;
        fullPageButton.Enabled = previewButton.Enabled = scanButton.Enabled = saveButton.Enabled = false;
        status.Text = "Сканирование…";
        string baseDir = AppDomain.CurrentDomain.BaseDirectory;
        string output = Path.Combine(Path.GetTempPath(), "bearpaw-" + Guid.NewGuid().ToString("N") + ".bmp");
        try
        {
            string firmware = Path.Combine(baseDir, "..", "Driver", "PS2fw.usb");
            string args = String.Format(CultureInfo.InvariantCulture, "--scan-region {0} {1} {2} {3} \"{4}\" \"{5}\"",
                area.X, area.Y, area.Width, area.Height, firmware, output);
            var options = new ProcessStartInfo(Path.Combine(baseDir, "BearPaw2400CUStatus.exe"), args)
            {
                CreateNoWindow = true, UseShellExecute = false, RedirectStandardError = true, RedirectStandardOutput = true
            };
            using (Process process = Process.Start(options))
            {
                Task<string> stdout = process.StandardOutput.ReadToEndAsync();
                Task<string> stderr = process.StandardError.ReadToEndAsync();
                await Task.WhenAll(stdout, stderr);
                process.WaitForExit();
                if (process.ExitCode != 0)
                    throw new InvalidOperationException(String.IsNullOrWhiteSpace(stderr.Result) ? stdout.Result : stderr.Result);
            }
            SetImage(output, fullPage);
            status.Text = successMessage;
        }
        catch (Exception error)
        {
            status.Text = "Сканирование не выполнено.";
            MessageBox.Show(this, error.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            try { File.Delete(output); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            busy = false;
            fullPageButton.Enabled = previewButton.Enabled = true;
            scanButton.Enabled = selection.Width > 0 && selection.Height > 0;
            saveButton.Enabled = result != null;
        }
    }

    private void Save()
    {
        using (var dialog = new SaveFileDialog { Filter = "PNG (*.png)|*.png", DefaultExt = "png", FileName = "scan.png" })
        {
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            try { result.Save(dialog.FileName, ImageFormat.Png); }
            catch (Exception error) { MessageBox.Show(this, error.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }
    }

    [STAThread]
    private static int Main(string[] args)
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        if (args.Length == 2 && args[0] == "--check-preview")
        {
            using (var window = new ScannerWindow(args[1]))
                return window.result.Size == Bed && window.saveButton.Enabled ? 0 : 1;
        }
        Application.Run(new ScannerWindow(null));
        return 0;
    }
}
