using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using OmegaTube.Util;

namespace OmegaTube.Data
{
    // Small JSON files in the app data folder: first page of each home row (instant start-up) and the
    // playback position of recently watched videos (resume + progress bars). Failures are non-fatal.
    public static class LocalStore
    {
        private const int MaxPositions = 300;

        private static string dir;
        private static readonly object posLock = new object();
        private static readonly object writeLock = new object();
        private static Dictionary<string, object> positions;

        public static void Init(string dataDir)
        {
            dir = dataDir;
            Task.Run(() =>
            {
                var loaded = ReadJson("positions.json") as Dictionary<string, object> ?? new Dictionary<string, object>();
                lock (posLock)
                {
                    if (positions == null)
                    {
                        positions = loaded;
                    }
                }
            });
        }

        // ---- playback positions ----

        // Fraction watched (0..1), or 0 when unknown.
        public static double Progress(string videoId)
        {
            object e;
            lock (posLock)
            {
                e = positions != null && positions.TryGetValue(videoId, out object v) ? v : null;
            }
            double dur = Json.Num(e, "dur");
            return dur > 0 ? Math.Min(1, Json.Num(e, "pos") / dur) : 0;
        }

        public static long ResumePosition(string videoId)
        {
            object e;
            lock (posLock)
            {
                e = positions != null && positions.TryGetValue(videoId, out object v) ? v : null;
            }
            return (long)Json.Num(e, "pos");
        }

        public static void SavePosition(string videoId, long posMs, long durMs)
        {
            if (string.IsNullOrEmpty(videoId) || durMs <= 0)
            {
                return;
            }
            Dictionary<string, object> snapshot;
            lock (posLock)
            {
                positions ??= new Dictionary<string, object>();
                // Nearly finished (or barely started) counts as "start from the beginning" next time,
                // but finished videos keep a full bar.
                positions[videoId] = new Dictionary<string, object>
                {
                    ["pos"] = (double)(posMs > durMs - 30000 ? durMs : posMs),
                    ["dur"] = (double)durMs,
                    ["at"] = (double)DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                };
                if (positions.Count > MaxPositions)
                {
                    string oldest = null;
                    double oldestAt = double.MaxValue;
                    foreach (var kv in positions)
                    {
                        double at = Json.Num(kv.Value, "at");
                        if (at < oldestAt)
                        {
                            oldestAt = at;
                            oldest = kv.Key;
                        }
                    }
                    if (oldest != null)
                    {
                        positions.Remove(oldest);
                    }
                }
                snapshot = new Dictionary<string, object>(positions);
            }
            System.Threading.Tasks.Task.Run(() => WriteJson("positions.json", snapshot));
        }

        private static object ReadJson(string name)
        {
            try
            {
                string path = Path.Combine(dir, name);
                return File.Exists(path) ? Json.Parse(File.ReadAllText(path)) : null;
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static void WriteJson(string name, object value)
        {
            try
            {
                string json = Json.Write(value);
                lock (writeLock)
                {
                    File.WriteAllText(Path.Combine(dir, name), json);
                }
            }
            catch (Exception e)
            {
                Tizen.Log.Error("omegatube-modern", name + ": " + e.Message);
            }
        }
    }
}
