using System;
using System.Collections.Generic;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace OmegaTube.Util
{
    // Thin wrapper over .NET 8 System.Text.Json exposing a uniform untyped object graph:
    // objects as Dictionary<string, object>, arrays as List<object>, numbers as double, plus string/bool/null.
    public static class Json
    {
        private static readonly JsonSerializerOptions WriteOptions = new JsonSerializerOptions
        {
            WriteIndented = true,
            Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        };

        private static readonly JsonSerializerOptions CompactWriteOptions = new JsonSerializerOptions
        {
            WriteIndented = false,
            Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        };

        public static object Parse(string text)
        {
            using JsonDocument doc = JsonDocument.Parse(text);
            return Convert(doc.RootElement);
        }

        public static object Parse(ReadOnlySpan<byte> utf8)
        {
            var reader = new Utf8JsonReader(utf8);
            using JsonDocument doc = JsonDocument.ParseValue(ref reader);
            return Convert(doc.RootElement);
        }

        public static string Write(object value) => JsonSerializer.Serialize(value, WriteOptions);
        public static string WriteCompact(object value) => JsonSerializer.Serialize(value, CompactWriteOptions);

        // Typed accessors that tolerate missing keys / wrong types.
        public static Dictionary<string, object> Obj(object o, string key) =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out object v) ? v as Dictionary<string, object> : null;

        public static List<object> Arr(object o, string key) =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out object v) ? v as List<object> : null;

        public static string Str(object o, string key, string fallback = "") =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out object v) && v is string s ? s : fallback;

        public static double Num(object o, string key, double fallback = 0) =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out object v) && v is double n ? n : fallback;

        public static bool Bool(object o, string key, bool fallback = false) =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out object v) && v is bool b ? b : fallback;

        // Allocation-free JsonElement accessors for large / hot native JSON responses.
        public static bool TryObj(JsonElement el, string key, out JsonElement val)
        {
            if (el.ValueKind == JsonValueKind.Object && el.TryGetProperty(key, out val) && val.ValueKind == JsonValueKind.Object)
            {
                return true;
            }
            val = default;
            return false;
        }

        public static bool TryArr(JsonElement el, string key, out JsonElement val)
        {
            if (el.ValueKind == JsonValueKind.Object && el.TryGetProperty(key, out val) && val.ValueKind == JsonValueKind.Array)
            {
                return true;
            }
            val = default;
            return false;
        }

        public static string Str(JsonElement el, string key, string fallback = "") =>
            el.ValueKind == JsonValueKind.Object && el.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.String
                ? (v.GetString() ?? fallback) : fallback;

        public static double Num(JsonElement el, string key, double fallback = 0) =>
            el.ValueKind == JsonValueKind.Object && el.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number
                && v.TryGetDouble(out double d) ? d : fallback;

        public static long Long(JsonElement el, string key, long fallback = 0) =>
            el.ValueKind == JsonValueKind.Object && el.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number
                ? (v.TryGetInt64(out long l) ? l : (v.TryGetDouble(out double d) ? (long)d : fallback)) : fallback;

        public static bool Bool(JsonElement el, string key, bool fallback = false) =>
            el.ValueKind == JsonValueKind.Object && el.TryGetProperty(key, out JsonElement v)
                ? (v.ValueKind == JsonValueKind.True ? true : v.ValueKind == JsonValueKind.False ? false : fallback) : fallback;

        private static readonly object TrueBox = true;
        private static readonly object FalseBox = false;

        private static object Convert(JsonElement el)
        {
            switch (el.ValueKind)
            {
                case JsonValueKind.Object:
                {
                    var obj = new Dictionary<string, object>();
                    foreach (JsonProperty p in el.EnumerateObject())
                    {
                        obj[p.Name] = Convert(p.Value);
                    }
                    return obj;
                }
                case JsonValueKind.Array:
                {
                    var list = new List<object>(el.GetArrayLength());
                    foreach (JsonElement item in el.EnumerateArray())
                    {
                        list.Add(Convert(item));
                    }
                    return list;
                }
                case JsonValueKind.String:
                    return el.GetString();
                case JsonValueKind.Number:
                    return el.GetDouble();
                case JsonValueKind.True:
                    return TrueBox;
                case JsonValueKind.False:
                    return FalseBox;
                default:
                    return null;
            }
        }
    }
}
