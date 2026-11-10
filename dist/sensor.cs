// Donanım sensörlerini LibreHardwareMonitor ile okur, her ölçümü tek satır JSON olarak yazar.
// Ana program bunu arka planda açar; stdin kapanınca kendiliğinden çıkar, yani yetim kalmaz.
// Komutlar (stdin): "wanted cpu gpu ram disk battery" ve "interval 1000"
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Management;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using LibreHardwareMonitor.Hardware;

class Reading {
    public string Type, Name;
    public float? Value;
}

static class Program {
    static readonly Regex CoreName = new Regex(@"^(?:CPU )?Core #(\d+)$");
    static readonly Regex CcdName = new Regex(@"^CCD ?(\d+)");
    static int interval = 1000;
    static volatile HashSet<string> wanted = new HashSet<string> { "cpu", "gpu", "ram", "disk", "battery" };
    static StreamWriter output;

    static int Main() {
        string lib = AppDomain.CurrentDomain.BaseDirectory;
        // Bağımlı DLL'ler bu klasörden yüklensin (sürüm uyuşmazlığında da)
        AppDomain.CurrentDomain.AssemblyResolve += delegate(object s, ResolveEventArgs a) {
            string path = Path.Combine(lib, new AssemblyName(a.Name).Name + ".dll");
            return File.Exists(path) ? Assembly.LoadFrom(path) : null;
        };
        output = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false));
        output.AutoFlush = true;
        Thread commands = new Thread(ReadCommands);
        commands.IsBackground = true;
        commands.Start();
        try {
            Run();
        } catch (Exception e) {
            string text = e.InnerException != null ? e.InnerException.Message : e.Message;
            output.WriteLine("{\"error\":" + Quote(text.Split('\n')[0].Trim()) + "}");
            return 1;
        }
        return 0;
    }

    static void ReadCommands() {
        string line;
        while ((line = Console.In.ReadLine()) != null) {
            string[] parts = line.Trim().Split(' ');
            if (parts[0] == "interval" && parts.Length > 1) {
                int value;
                if (int.TryParse(parts[1], out value)) interval = Math.Max(value, 250);
            } else if (parts[0] == "wanted") {
                HashSet<string> next = new HashSet<string>();
                for (int i = 1; i < parts.Length; i++) next.Add(parts[i]);
                wanted = next;
            }
        }
        Environment.Exit(0);
    }

    static void Run() {
        Computer computer = new Computer();
        computer.IsCpuEnabled = computer.IsGpuEnabled = computer.IsMemoryEnabled = true;
        computer.IsStorageEnabled = computer.IsBatteryEnabled = true;
        computer.Open();
        Dictionary<string, string> types = DiskTypes();
        while (true) {
            try {
                output.WriteLine(Read(computer, types));
            } catch (Exception) {
                // Tek seferlik okuma hatası (cihaz çıkarıldı vb.): son değerler kalsın
            }
            Thread.Sleep(interval);
        }
    }

    static string Category(string kind) {
        switch (kind) {
            case "Cpu": return "cpu";
            case "GpuNvidia": case "GpuAmd": case "GpuIntel": return "gpu";
            case "Memory": return "ram";
            case "Storage": return "disk";
            case "Battery": return "battery";
        }
        return null;
    }

    static string Read(Computer computer, Dictionary<string, string> types) {
        string cpu = "null", ram = "null", battery = "null";
        List<string> gpus = new List<string>(), disks = new List<string>();
        foreach (IHardware hw in computer.Hardware) {
            string kind = hw.HardwareType.ToString();
            string category = Category(kind);
            if (category == null || !wanted.Contains(category)) continue;
            string id = hw.Identifier.ToString();
            if (kind == "Memory" && id != "/ram") continue;   // Sanal bellek değil, fiziksel RAM
            hw.Update();
            List<Reading> sensors = new List<Reading>();
            foreach (ISensor s in hw.Sensors)
                sensors.Add(new Reading { Type = s.SensorType.ToString(), Name = s.Name, Value = s.Value });
            if (kind == "Cpu" && cpu == "null") cpu = ReadCpu(sensors);
            else if (category == "gpu") gpus.Add(ReadGpu(hw.Name, kind, sensors));
            else if (kind == "Memory")
                ram = "{\"used\":" + Num(Find(sensors, "Data", "Memory Used")) + ",\"load\":" + Num(Find(sensors, "Load", "Memory")) + "}";
            else if (kind == "Storage")
                disks.Add("{\"name\":" + Quote(hw.Name) + ",\"type\":" + Quote(DiskType(hw.Name, id, types)) +
                          ",\"temp\":" + Num(First(sensors, "Temperature")) + "}");
            else if (kind == "Battery") {
                float? rate = Find(sensors, "Power", "Charge/Discharge Rate", "Discharge Rate", "Charge Rate");
                battery = "{\"level\":" + Num(Find(sensors, "Level", "Charge Level")) +
                          ",\"rate\":" + Num(rate.HasValue ? Math.Abs(rate.Value) : (float?)null) + "}";
            }
        }
        return "{\"cpu\":" + cpu + ",\"gpus\":[" + string.Join(",", gpus) + "],\"ram\":" + ram +
               ",\"disks\":[" + string.Join(",", disks) + "],\"battery\":" + battery + "}";
    }

    static string ReadCpu(List<Reading> sensors) {
        List<string> cores = new List<string>(), ccds = new List<string>();
        float clocks = 0;
        int clock_count = 0;
        foreach (Reading r in sensors) {
            if (r.Type == "Temperature" && r.Value.HasValue) {
                Match m = CoreName.Match(r.Name);
                if (m.Success) cores.Add("[" + Quote("C" + m.Groups[1].Value) + "," + Num(r.Value) + "]");
                m = CcdName.Match(r.Name);
                if (m.Success) ccds.Add("[" + Quote("CCD" + m.Groups[1].Value) + "," + Num(r.Value) + "]");
            }
            if (r.Type == "Clock" && r.Value.HasValue && r.Value.Value > 0 && CoreName.IsMatch(r.Name)) {
                clocks += r.Value.Value;
                clock_count++;
            }
        }
        if (cores.Count == 0) cores = ccds;   // AMD'de çekirdek yerine CCD sıcaklıkları var
        return "{\"temp\":" + Num(Find(sensors, "Temperature", "CPU Package", "Core (Tctl/Tdie)", "Core Max", "Core Average")) +
               ",\"load\":" + Num(Find(sensors, "Load", "CPU Total")) +
               ",\"clock\":" + Num(clock_count > 0 ? clocks / clock_count : (float?)null) +
               ",\"power\":" + Num(Positive(Find(sensors, "Power", "CPU Package", "Package"))) +
               ",\"cores\":[" + string.Join(",", cores) + "]}";
    }

    static string ReadGpu(string name, string kind, List<Reading> sensors) {
        return "{\"name\":" + Quote(name) + ",\"discrete\":" + (kind != "GpuIntel" ? "true" : "false") +
               ",\"temp\":" + Num(Find(sensors, "Temperature", "GPU Core")) +
               ",\"hotspot\":" + Num(Find(sensors, "Temperature", "GPU Hot Spot")) +
               ",\"load\":" + Num(Find(sensors, "Load", "GPU Core", "D3D 3D")) +
               ",\"clock\":" + Num(Positive(Find(sensors, "Clock", "GPU Core"))) +
               ",\"vram\":" + Num(Find(sensors, "SmallData", "GPU Memory Used", "D3D Dedicated Memory Used")) +
               ",\"power\":" + Num(Positive(Find(sensors, "Power", "GPU Package", "GPU Power", "GPU Core"))) +
               ",\"fan\":" + Num(Positive(First(sensors, "Fan"))) + "}";
    }

    static float? Find(List<Reading> sensors, string type, params string[] names) {
        foreach (string name in names)
            foreach (Reading r in sensors)
                if (r.Type == type && r.Name == name && r.Value.HasValue) return r.Value;
        return null;
    }

    static float? First(List<Reading> sensors, string type) {
        foreach (Reading r in sensors)
            if (r.Type == type && r.Value.HasValue) return r.Value;
        return null;
    }

    // Yetki yokken güç sensörleri 0 döner; bu gerçek bir ölçüm değil
    static float? Positive(float? value) {
        return value.HasValue && value.Value > 0 ? value : null;
    }

    // Disk modeli -> SSD / HDD (Windows depolama bilgisinden, yönetici gerekmez)
    static Dictionary<string, string> DiskTypes() {
        Dictionary<string, string> types = new Dictionary<string, string>();
        try {
            ManagementObjectSearcher query = new ManagementObjectSearcher(@"root\Microsoft\Windows\Storage",
                "SELECT FriendlyName, MediaType FROM MSFT_PhysicalDisk");
            foreach (ManagementBaseObject disk in query.Get()) {
                int media = Convert.ToInt32(disk["MediaType"]);
                types[disk["FriendlyName"].ToString().Trim().ToLowerInvariant()] = media == 3 ? "HDD" : media == 4 || media == 5 ? "SSD" : "";
            }
        } catch (Exception) {
        }
        return types;
    }

    static string DiskType(string name, string identifier, Dictionary<string, string> types) {
        name = name.Trim().ToLowerInvariant();
        foreach (KeyValuePair<string, string> t in types)
            if (t.Value != "" && (t.Key == name || t.Key.Contains(name) || name.Contains(t.Key))) return t.Value;
        return identifier.ToLowerInvariant().Contains("nvme") ? "SSD" : "DISK";
    }

    // Türkçe Windows'ta ondalık ayraç virgül olmasın diye sabit kültür
    static string Num(float? value) {
        if (!value.HasValue || float.IsNaN(value.Value) || float.IsInfinity(value.Value)) return "null";
        return value.Value.ToString("0.###", CultureInfo.InvariantCulture);
    }

    static string Quote(string text) {
        StringBuilder b = new StringBuilder("\"");
        foreach (char c in text ?? "") {
            if (c == '"' || c == '\\') b.Append('\\').Append(c);
            else if (c < 32) b.Append(' ');
            else b.Append(c);
        }
        return b.Append('"').ToString();
    }
}
