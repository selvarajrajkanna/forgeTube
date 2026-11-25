using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using ForgeTube.Util;

namespace ForgeTube.Data
{
    // Small JSON files in the app data folder: first page of each home row (instant start-up) and the
    // playback position of recently watched videos (resume + progress bars). Failures are non-fatal.
    public static class LocalStore
    {
        private const int MaxPositions = 300;
        private const long PeriodicFlushIntervalMs = 60_000;

        private static string dir;
        private static readonly object posLock = new object();
        private static Dictionary<string, object> positions;
        private static bool dirty;
        private static bool writing;
        private static long lastWriteTick;

        public static void Init(string dataDir)
        {
            dir = dataDir;
            Task.Run(() =>
            {
                var loaded = ReadJson("positions.json") as Dictionary<string, object> ?? new Dictionary<string, object>();
                bool needSave = false;
                lock (posLock)
                {
                    if (positions == null)
                    {
                        positions = loaded;
                    }
                    else
                    {
                        // Merge loaded history without overwriting newer entries saved before init completed.
                        foreach (var kv in loaded)
                        {
                            if (!positions.ContainsKey(kv.Key))
                            {
                                positions[kv.Key] = kv.Value;
                            }
                        }
                        TrimPositionsLocked();
                        dirty = true;
                        if (!writing)
                        {
                            writing = true;
                            needSave = true;
                        }
                    }
                }
                if (needSave)
                {
                    DrainWrites();
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

        public static bool HasPosition(string videoId)
        {
            if (string.IsNullOrEmpty(videoId))
            {
                return false;
            }
            lock (posLock)
            {
                return positions != null && positions.ContainsKey(videoId);
            }
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

        public static void SavePosition(string videoId, long posMs, long durMs, bool flush = true)
        {
            if (string.IsNullOrEmpty(videoId) || durMs <= 0)
            {
                return;
            }
            bool startWriter = false;
            long now = Environment.TickCount64;
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
                TrimPositionsLocked();
                dirty = true;
                bool due = flush || lastWriteTick == 0 || (now - lastWriteTick) >= PeriodicFlushIntervalMs;
                if (due && !writing)
                {
                    writing = true;
                    lastWriteTick = now;
                    startWriter = true;
                }
            }
            if (startWriter)
            {
                Task.Run(DrainWrites);
            }
        }

        // Flushes any pending in-memory playback positions to flash (app pause / exit).
        public static void Flush(bool sync = false)
        {
            bool startWriter = false;
            lock (posLock)
            {
                if (dirty && !writing)
                {
                    writing = true;
                    lastWriteTick = Environment.TickCount64;
                    startWriter = true;
                }
            }
            if (!startWriter)
            {
                return;
            }
            if (sync)
            {
                DrainWrites();
            }
            else
            {
                Task.Run(DrainWrites);
            }
        }

        private static void TrimPositionsLocked()
        {
            while (positions.Count > MaxPositions)
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
                if (oldest == null)
                {
                    break;
                }
                positions.Remove(oldest);
            }
        }

        private static void DrainWrites()
        {
            for (;;)
            {
                Dictionary<string, object> snapshot;
                lock (posLock)
                {
                    if (!dirty)
                    {
                        writing = false;
                        return;
                    }
                    dirty = false;
                    snapshot = new Dictionary<string, object>(positions);
                }
                WriteJson("positions.json", snapshot);
            }
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
                string json = Json.WriteCompact(value);
                string path = Path.Combine(dir, name);
                string tmp = path + ".tmp";
                File.WriteAllText(tmp, json);
                File.Move(tmp, path, overwrite: true);
            }
            catch (Exception e)
            {
                Tizen.Log.Error("forgetube", name + ": " + e.Message);
            }
        }
    }
}
