using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

internal static partial class InstallDriver
{
    private const string DevicePrefix = @"USB\VID_05D8&PID_4002\";
    private const string HardwareId = @"USB\VID_05D8&PID_4002&REV_0100";
    private const string DriverInf = @"Driver\BearPaw2400CUWia.inf";
    private const string CertificateFile = @"Driver\BearPaw2400CUWia-test.cer";
    private static readonly string PackageDirectory = AppDomain.CurrentDomain.BaseDirectory;

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupCopyOEMInf(string sourceInf, string sourceDirectory, uint sourceType,
        uint copyStyle, StringBuilder destination, uint destinationSize, out uint requiredSize, IntPtr component);

    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr parent, string hardwareId, string sourceInf,
        uint flags, out bool rebootRequired);

    private static void VerifyFiles()
    {
        using (SHA256 sha = SHA256.Create())
        {
            foreach (var file in PackageHashes)
            {
                using (var stream = File.OpenRead(Path.Combine(PackageDirectory, file.Key)))
                {
                    string hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
                    if (hash != file.Value) throw new InvalidDataException("Неверная контрольная сумма: " + file.Key);
                }
            }
        }
    }

    private static string FindScanner()
    {
        var matches = new List<string>();
        using (var query = new ManagementObjectSearcher(
            "SELECT PNPDeviceID, HardwareID FROM Win32_PnPEntity WHERE Present = TRUE"))
        using (var devices = query.Get())
        {
            foreach (ManagementObject device in devices)
            using (device)
            {
                string instance = device["PNPDeviceID"] as string;
                string[] ids = device["HardwareID"] as string[];
                if (instance != null && instance.StartsWith(DevicePrefix, StringComparison.OrdinalIgnoreCase) &&
                    ids != null && Array.Exists(ids, id => String.Equals(id, HardwareId, StringComparison.OrdinalIgnoreCase)))
                    matches.Add(instance);
            }
        }
        if (matches.Count == 0) throw new InvalidOperationException("Подключите BearPaw 2400 CU по USB и запустите установщик снова.");
        if (matches.Count != 1) throw new InvalidOperationException("Найдено несколько устройств с тем же USB ID. Оставьте подключённым только 2400 CU.");
        return matches[0];
    }

    private static bool PackageIsActive(string instance, string publishedInf)
    {
        using (var device = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\" + instance))
        {
            string driverKey = device == null ? null : device.GetValue("Driver") as string;
            if (driverKey == null) return false;
            using (var driver = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\" + driverKey))
                return driver != null && String.Equals(driver.GetValue("InfPath") as string, publishedInf,
                    StringComparison.OrdinalIgnoreCase);
        }
    }

    private static void Install()
    {
        const uint SourceIsPath = 1;
        const uint ForceSelectedDriver = 1;
        string instance = FindScanner();
        Console.WriteLine("Устройство: " + instance);
        Console.WriteLine("Проверьте маркировку сканера на корпусе: Mustek BearPaw 2400 CU (без Plus).");
        Console.WriteLine("У других моделей Grandtech может быть тот же USB ID.");
        Console.WriteLine("Windows будет доверять тестовому сертификату установки этого пакета.");
        using (var certificate = new X509Certificate2(Path.Combine(PackageDirectory, CertificateFile)))
        {
            Console.WriteLine("Сертификат: " + certificate.GetNameInfo(X509NameType.SimpleName, false));
            Console.WriteLine("Действует до: " + certificate.NotAfter.ToString("yyyy-MM-dd"));
        }
        Console.WriteLine("Отпечаток: " + Thumbprint);
        Console.Write("Введите YES для установки: ");
        if (!String.Equals(Console.ReadLine(), "YES", StringComparison.OrdinalIgnoreCase))
            throw new OperationCanceledException("Установка отменена.");

        if (!String.Equals(FindScanner(), instance, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("USB-устройство изменилось во время подтверждения.");
        VerifyFiles();

        var added = new List<StoreName>();
        bool staged = false;
        using (var cert = new X509Certificate2(Path.Combine(PackageDirectory, CertificateFile)))
        {
            if (cert.Thumbprint != Thumbprint || DateTime.Now < cert.NotBefore || DateTime.Now > cert.NotAfter)
                throw new InvalidDataException("Сертификат пакета не совпадает или вышел из срока действия.");
            try
            {
                foreach (var name in new[] { StoreName.Root, StoreName.TrustedPublisher })
                using (var store = new X509Store(name, StoreLocation.LocalMachine))
                {
                    store.Open(OpenFlags.ReadWrite);
                    if (store.Certificates.Find(X509FindType.FindByThumbprint, Thumbprint, false).Count == 0)
                    {
                        store.Add(cert);
                        added.Add(name);
                    }
                }
                string inf = Path.Combine(PackageDirectory, DriverInf);
                var publishedPath = new StringBuilder(1024);
                uint requiredSize;
                if (!SetupCopyOEMInf(inf, Path.GetDirectoryName(inf), SourceIsPath, 0, publishedPath,
                    (uint)publishedPath.Capacity, out requiredSize, IntPtr.Zero))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                staged = true;
                string publishedInf = Path.GetFileName(publishedPath.ToString());
                Console.WriteLine("Пакет в хранилище Windows: " + publishedInf);
                if (!PackageIsActive(instance, publishedInf))
                {
                    if (!String.Equals(FindScanner(), instance, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("USB-устройство изменилось. Пакет добавлен, привязка отменена.");
                    bool rebootRequired;
                    if (!UpdateDriverForPlugAndPlayDevices(IntPtr.Zero, HardwareId, inf, ForceSelectedDriver, out rebootRequired))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    if (rebootRequired)
                    {
                        Console.WriteLine("Windows запросила перезагрузку для завершения привязки.");
                        return;
                    }
                }
                if (!PackageIsActive(instance, publishedInf))
                    throw new InvalidOperationException("Windows добавила пакет, но активная привязка ещё не подтверждена.");
                Console.WriteLine("Драйвер BearPaw 2400 CU установлен. Откройте VueScan и выберите BearPaw 2400 CU.");
            }
            finally
            {
                if (!staged)
                {
                    foreach (var name in added)
                    using (var store = new X509Store(name, StoreLocation.LocalMachine))
                    {
                        store.Open(OpenFlags.ReadWrite);
                        store.Remove(cert);
                    }
                }
            }
        }
    }

    private static int Main(string[] args)
    {
        bool verifyOnly = args.Length == 1 && args[0] == "--verify";
        bool elevated = args.Length == 1 && args[0] == "--elevated";
        if (args.Length != 0 && !verifyOnly && !elevated) return 2;
        try
        {
            if (!Environment.Is64BitOperatingSystem) throw new PlatformNotSupportedException("Требуется Windows x64.");
            VerifyFiles();
            if (verifyOnly)
            {
                Console.WriteLine("Контрольные суммы всех файлов пакета совпадают.");
                return 0;
            }
            using (var identity = WindowsIdentity.GetCurrent())
            {
                if (!new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator))
                {
                    if (elevated) throw new InvalidOperationException("Требуются права администратора.");
                    using (var child = Process.Start(new ProcessStartInfo
                    {
                        FileName = Path.Combine(PackageDirectory, "Install-BearPaw2400CU.exe"),
                        Arguments = "--elevated", WorkingDirectory = PackageDirectory,
                        UseShellExecute = true, Verb = "runas"
                    }))
                    {
                        child.WaitForExit();
                        return child.ExitCode;
                    }
                }
            }
            Install();
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error.Message);
            if (!verifyOnly && !Console.IsInputRedirected)
            {
                Console.WriteLine("Нажмите Enter для выхода.");
                Console.ReadLine();
            }
            return 1;
        }
        if (!Console.IsInputRedirected)
        {
            Console.WriteLine("Нажмите Enter для выхода.");
            Console.ReadLine();
        }
        return 0;
    }
}
