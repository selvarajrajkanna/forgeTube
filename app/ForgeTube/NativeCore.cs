using System;
using System.Buffers;
using System.Runtime.InteropServices;
using System.Text;

namespace ForgeTube
{
    // Bindings to libomegacore.so (see core/include/omega/core.h).
    // Uses raw unmanaged function pointers (delegate* unmanaged[Cdecl]) and stack-encoded
    // UTF-8 argument buffers to avoid managed delegate wrappers and per-call marshaling heap allocs.
    public static unsafe class NativeCore
    {
        private const int StackUtf8Limit = 256;

        // "file path" or the NativeLibrary failure detail (shown in bug reports).
        public static string LoadRoute { get; private set; } = "not attempted";

        // Resolves an exported symbol of the loaded core.
        private static IntPtr libHandle;
        private static readonly object loadLock = new object();
        private static bool loaded;

        // Loads the core once via .NET 8 NativeLibrary, then binds the function pointers.
        public static void EnsureLoaded()
        {
            if (loaded)
            {
                return;
            }
            lock (loadLock)
            {
                if (loaded)
                {
                    return;
                }
                string path = CorePath();
                try
                {
                    libHandle = NativeLibrary.Load(path);
                    LoadRoute = "file path";
                    Bind();
                    loaded = true;
                }
                catch (Exception e)
                {
                    LoadRoute = "NativeLibrary.Load failed: " + e.Message;
                    Tizen.Log.Error("forgetube", LoadRoute);
                    throw;
                }
            }
        }

        private static void* Export(string name) => (void*)NativeLibrary.GetExport(libHandle, name);

        // app/ForgeTube/lib/libomegacore.so at runtime: a sibling of the resource folder.
        internal static string CorePath()
        {
            string res = Tizen.Applications.Application.Current.DirectoryInfo.Resource;
            return System.IO.Path.GetFullPath(System.IO.Path.Combine(res, "..", "lib", "libomegacore.so"));
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct PlayerStats
        {
            public int State;
            public int Eos;
            public long PositionMs;
            public long VideoQueuedMs;
            public long AudioQueuedMs;
            public long VideoSubmittedMs;
            public long AudioSubmittedMs;
            public uint Stalls;
            public uint Errors;
            public long DurationMs;
            public int Reconnecting;
            public uint DroppedFrames;
            public int DspBits;
            public int DspHf;
            public uint VideoBitrateKbps;
        }

        // --- Bound unmanaged function pointers (cdecl) -------------------------------------------------------
        private static delegate* unmanaged[Cdecl, SuppressGCTransition]<byte*> _version;
        private static delegate* unmanaged[Cdecl, SuppressGCTransition]<IntPtr, void> _free;
        private static delegate* unmanaged[Cdecl, SuppressGCTransition]<int, void> _setDefaultAudioDelay;
        private static delegate* unmanaged[Cdecl, SuppressGCTransition]<void> _playerCancelPending;

        private static delegate* unmanaged[Cdecl]<IntPtr> _playerLastError, _configRefresh, _authStatus, _authBegin, _authAccount, _authAccounts;
        private static delegate* unmanaged[Cdecl]<int> _reportAvailable, _btAudioDelayMs;
        private static delegate* unmanaged[Cdecl]<void> _authSignOut, _ytPrewarmDecipher;
        private static delegate* unmanaged[Cdecl]<byte*, int, int, int> _logTail;
        private static delegate* unmanaged[Cdecl]<byte*, int, int> _tlsInfo;
        private static delegate* unmanaged[Cdecl]<byte*, int, void> _logInitUdp;
        private static delegate* unmanaged[Cdecl]<byte*, void> _logLine, _init, _ytPreload;
        private static delegate* unmanaged[Cdecl]<byte*, byte*, void> _setLocale;
        private static delegate* unmanaged[Cdecl]<IntPtr, int, int, byte*, int, byte*, long, byte*, IntPtr> _playerCreateYouTube;
        private static delegate* unmanaged[Cdecl]<IntPtr, void> _playerTogglePause, _playerDestroy;
        private static delegate* unmanaged[Cdecl]<IntPtr, IntPtr> _playerFormats;
        private static delegate* unmanaged[Cdecl]<IntPtr, long, int> _playerSeek;
        private static delegate* unmanaged[Cdecl]<IntPtr, double, int> _playerSetRate;
        private static delegate* unmanaged[Cdecl]<IntPtr, int, void> _playerSetAudioDelay;
        private static delegate* unmanaged[Cdecl]<IntPtr, PlayerStats*, void> _playerGetStats;
        private static delegate* unmanaged[Cdecl]<byte*, IntPtr> _ytSuggest, _reportUpload, _authPoll, _ytDetails, _sbSegments, _authSwitch;
        private static delegate* unmanaged[Cdecl]<byte*, byte*, IntPtr> _ytPostComment;
        private static delegate* unmanaged[Cdecl]<byte*, byte*, byte*, IntPtr> _ytPostReply, _ytCommentAction;
        private static delegate* unmanaged[Cdecl]<byte*, int, IntPtr> _ytSubscribe;
        private static delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr> _ytSearch, _ytBrowse, _ytPlaylist, _ytPlaylistPage, _ytRelated, _ytChannel, _ytComments, _ytCaptions;

        private static void Bind()
        {
            _version = (delegate* unmanaged[Cdecl, SuppressGCTransition]<byte*>)Export("omega_version");
            _free = (delegate* unmanaged[Cdecl, SuppressGCTransition]<IntPtr, void>)Export("omega_free");
            _setDefaultAudioDelay = (delegate* unmanaged[Cdecl, SuppressGCTransition]<int, void>)Export("omega_set_default_audio_delay");
            _playerCancelPending = (delegate* unmanaged[Cdecl, SuppressGCTransition]<void>)Export("omega_player_cancel_pending");

            _logInitUdp = (delegate* unmanaged[Cdecl]<byte*, int, void>)Export("omega_log_init_udp");
            _logLine = (delegate* unmanaged[Cdecl]<byte*, void>)Export("omega_log_line");
            _logTail = (delegate* unmanaged[Cdecl]<byte*, int, int, int>)Export("omega_log_tail");
            _tlsInfo = (delegate* unmanaged[Cdecl]<byte*, int, int>)Export("omega_tls_info");
            _playerCreateYouTube = (delegate* unmanaged[Cdecl]<IntPtr, int, int, byte*, int, byte*, long, byte*, IntPtr>)Export("omega_player_create_youtube");
            _ytPreload = (delegate* unmanaged[Cdecl]<byte*, void>)Export("omega_yt_preload");
            _ytPrewarmDecipher = (delegate* unmanaged[Cdecl]<void>)Export("omega_yt_prewarm_decipher");
            _playerLastError = (delegate* unmanaged[Cdecl]<IntPtr>)Export("omega_player_last_error");
            _playerTogglePause = (delegate* unmanaged[Cdecl]<IntPtr, void>)Export("omega_player_toggle_pause");
            _playerSeek = (delegate* unmanaged[Cdecl]<IntPtr, long, int>)Export("omega_player_seek");
            _playerGetStats = (delegate* unmanaged[Cdecl]<IntPtr, PlayerStats*, void>)Export("omega_player_get_stats");
            _playerDestroy = (delegate* unmanaged[Cdecl]<IntPtr, void>)Export("omega_player_destroy");
            _ytSearch = (delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr>)Export("omega_yt_search");
            _ytSuggest = (delegate* unmanaged[Cdecl]<byte*, IntPtr>)Export("omega_yt_suggest");
            _ytBrowse = (delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr>)Export("omega_yt_browse");
            _init = (delegate* unmanaged[Cdecl]<byte*, void>)Export("omega_init");
            _configRefresh = (delegate* unmanaged[Cdecl]<IntPtr>)Export("omega_config_refresh");
            _setLocale = (delegate* unmanaged[Cdecl]<byte*, byte*, void>)Export("omega_set_locale");
            _reportAvailable = (delegate* unmanaged[Cdecl]<int>)Export("omega_report_available");
            _reportUpload = (delegate* unmanaged[Cdecl]<byte*, IntPtr>)Export("omega_report_upload");
            _authStatus = (delegate* unmanaged[Cdecl]<IntPtr>)Export("omega_auth_status");
            _authBegin = (delegate* unmanaged[Cdecl]<IntPtr>)Export("omega_auth_begin");
            _authPoll = (delegate* unmanaged[Cdecl]<byte*, IntPtr>)Export("omega_auth_poll");
            _authSignOut = (delegate* unmanaged[Cdecl]<void>)Export("omega_auth_sign_out");
            _ytPlaylist = (delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr>)Export("omega_yt_playlist");
            _ytPlaylistPage = (delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr>)Export("omega_yt_playlist_page");
            _ytRelated = (delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr>)Export("omega_yt_related");
            _playerFormats = (delegate* unmanaged[Cdecl]<IntPtr, IntPtr>)Export("omega_player_formats");
            _playerSetRate = (delegate* unmanaged[Cdecl]<IntPtr, double, int>)Export("omega_player_set_rate");
            _playerSetAudioDelay = (delegate* unmanaged[Cdecl]<IntPtr, int, void>)Export("omega_player_set_audio_delay");
            _btAudioDelayMs = (delegate* unmanaged[Cdecl]<int>)Export("omega_bt_audio_delay_ms");
            _ytCaptions = (delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr>)Export("omega_yt_captions");
            _ytDetails = (delegate* unmanaged[Cdecl]<byte*, IntPtr>)Export("omega_yt_details");
            _ytSubscribe = (delegate* unmanaged[Cdecl]<byte*, int, IntPtr>)Export("omega_yt_subscribe");
            _ytChannel = (delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr>)Export("omega_yt_channel");
            _ytComments = (delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr>)Export("omega_yt_comments");
            _ytPostComment = (delegate* unmanaged[Cdecl]<byte*, byte*, IntPtr>)Export("omega_yt_post_comment");
            _ytPostReply = (delegate* unmanaged[Cdecl]<byte*, byte*, byte*, IntPtr>)Export("omega_yt_post_reply");
            _ytCommentAction = (delegate* unmanaged[Cdecl]<byte*, byte*, byte*, IntPtr>)Export("omega_yt_comment_action");
            _authAccount = (delegate* unmanaged[Cdecl]<IntPtr>)Export("omega_auth_account");
            _authAccounts = (delegate* unmanaged[Cdecl]<IntPtr>)Export("omega_auth_accounts");
            _authSwitch = (delegate* unmanaged[Cdecl]<byte*, IntPtr>)Export("omega_auth_switch");
            _sbSegments = (delegate* unmanaged[Cdecl]<byte*, IntPtr>)Export("omega_sb_segments");
        }

        // --- Zero-alloc stack-first UTF-8 marshaling helpers -------------------------------------------------
        private static byte[] EncodeUtf8(string s, Span<byte> stackBuf, out Span<byte> written)
        {
            s ??= "";
            int maxBytes = Encoding.UTF8.GetMaxByteCount(s.Length) + 1;
            if (maxBytes <= stackBuf.Length)
            {
                int n = Encoding.UTF8.GetBytes(s.AsSpan(), stackBuf);
                stackBuf[n] = 0;
                written = stackBuf.Slice(0, n + 1);
                return null;
            }
            byte[] rented = ArrayPool<byte>.Shared.Rent(maxBytes);
            int len = Encoding.UTF8.GetBytes(s, 0, s.Length, rented, 0);
            rented[len] = 0;
            written = rented.AsSpan(0, len + 1);
            return rented;
        }

        private static void CallVoidS(delegate* unmanaged[Cdecl]<byte*, void> fn, string a)
        {
            Span<byte> sa = stackalloc byte[StackUtf8Limit];
            byte[] ra = EncodeUtf8(a, sa, out Span<byte> wa);
            try
            {
                fixed (byte* pa = wa) { fn(pa); }
            }
            finally
            {
                if (ra != null) ArrayPool<byte>.Shared.Return(ra);
            }
        }

        private static void CallVoidSS(delegate* unmanaged[Cdecl]<byte*, byte*, void> fn, string a, string b)
        {
            Span<byte> sa = stackalloc byte[StackUtf8Limit];
            Span<byte> sb = stackalloc byte[StackUtf8Limit];
            byte[] ra = EncodeUtf8(a, sa, out Span<byte> wa);
            byte[] rb = null;
            try
            {
                rb = EncodeUtf8(b, sb, out Span<byte> wb);
                fixed (byte* pa = wa)
                fixed (byte* pb = wb) { fn(pa, pb); }
            }
            finally
            {
                if (ra != null) ArrayPool<byte>.Shared.Return(ra);
                if (rb != null) ArrayPool<byte>.Shared.Return(rb);
            }
        }

        private static void CallVoidSI(delegate* unmanaged[Cdecl]<byte*, int, void> fn, string a, int b)
        {
            Span<byte> sa = stackalloc byte[StackUtf8Limit];
            byte[] ra = EncodeUtf8(a, sa, out Span<byte> wa);
            try
            {
                fixed (byte* pa = wa) { fn(pa, b); }
            }
            finally
            {
                if (ra != null) ArrayPool<byte>.Shared.Return(ra);
            }
        }

        private static IntPtr CallPtrS(delegate* unmanaged[Cdecl]<byte*, IntPtr> fn, string a)
        {
            Span<byte> sa = stackalloc byte[StackUtf8Limit];
            byte[] ra = EncodeUtf8(a, sa, out Span<byte> wa);
            try
            {
                fixed (byte* pa = wa) { return fn(pa); }
            }
            finally
            {
                if (ra != null) ArrayPool<byte>.Shared.Return(ra);
            }
        }

        private static IntPtr CallPtrSI(delegate* unmanaged[Cdecl]<byte*, int, IntPtr> fn, string a, int b)
        {
            Span<byte> sa = stackalloc byte[StackUtf8Limit];
            byte[] ra = EncodeUtf8(a, sa, out Span<byte> wa);
            try
            {
                fixed (byte* pa = wa) { return fn(pa, b); }
            }
            finally
            {
                if (ra != null) ArrayPool<byte>.Shared.Return(ra);
            }
        }

        private static IntPtr CallPtrSS(delegate* unmanaged[Cdecl]<byte*, byte*, IntPtr> fn, string a, string b)
        {
            Span<byte> sa = stackalloc byte[StackUtf8Limit];
            Span<byte> sb = stackalloc byte[StackUtf8Limit];
            byte[] ra = EncodeUtf8(a, sa, out Span<byte> wa);
            byte[] rb = null;
            try
            {
                rb = EncodeUtf8(b, sb, out Span<byte> wb);
                fixed (byte* pa = wa)
                fixed (byte* pb = wb) { return fn(pa, pb); }
            }
            finally
            {
                if (ra != null) ArrayPool<byte>.Shared.Return(ra);
                if (rb != null) ArrayPool<byte>.Shared.Return(rb);
            }
        }

        private static IntPtr CallPtrSSS(delegate* unmanaged[Cdecl]<byte*, byte*, byte*, IntPtr> fn, string a, string b, string c)
        {
            Span<byte> sa = stackalloc byte[StackUtf8Limit];
            Span<byte> sb = stackalloc byte[StackUtf8Limit];
            Span<byte> sc = stackalloc byte[StackUtf8Limit];
            byte[] ra = EncodeUtf8(a, sa, out Span<byte> wa);
            byte[] rb = null;
            byte[] rc = null;
            try
            {
                rb = EncodeUtf8(b, sb, out Span<byte> wb);
                rc = EncodeUtf8(c, sc, out Span<byte> wc);
                fixed (byte* pa = wa)
                fixed (byte* pb = wb)
                fixed (byte* pc = wc) { return fn(pa, pb, pc); }
            }
            finally
            {
                if (ra != null) ArrayPool<byte>.Shared.Return(ra);
                if (rb != null) ArrayPool<byte>.Shared.Return(rb);
                if (rc != null) ArrayPool<byte>.Shared.Return(rc);
            }
        }

        private static IntPtr CallBinSS(delegate* unmanaged[Cdecl]<byte*, byte*, uint*, IntPtr> fn, string a, string b, out uint len)
        {
            Span<byte> sa = stackalloc byte[StackUtf8Limit];
            Span<byte> sb = stackalloc byte[StackUtf8Limit];
            byte[] ra = EncodeUtf8(a, sa, out Span<byte> wa);
            byte[] rb = null;
            try
            {
                rb = EncodeUtf8(b, sb, out Span<byte> wb);
                uint outLen = 0;
                fixed (byte* pa = wa)
                fixed (byte* pb = wb)
                {
                    IntPtr ptr = fn(pa, pb, &outLen);
                    len = outLen;
                    return ptr;
                }
            }
            finally
            {
                if (ra != null) ArrayPool<byte>.Shared.Return(ra);
                if (rb != null) ArrayPool<byte>.Shared.Return(rb);
            }
        }

        // --- Public API ---------------------------------------------------------------------------------------
        public static void LogInitUdp(string host, int port) => CallVoidSI(_logInitUdp, host, port);
        public static void Log(string text) => CallVoidS(_logLine, text);

        public static IntPtr PlayerCreateYouTube(IntPtr ecoreWindow, int width, int height, string videoId, int maxHeight, string codecs, long startMs, string audioTrack)
        {
            Span<byte> sv = stackalloc byte[StackUtf8Limit];
            Span<byte> sc = stackalloc byte[StackUtf8Limit];
            Span<byte> sa = stackalloc byte[StackUtf8Limit];
            byte[] rv = EncodeUtf8(videoId, sv, out Span<byte> wv);
            byte[] rc = null;
            byte[] ra = null;
            try
            {
                rc = EncodeUtf8(codecs, sc, out Span<byte> wc);
                ra = EncodeUtf8(audioTrack, sa, out Span<byte> wa);
                fixed (byte* pv = wv)
                fixed (byte* pc = wc)
                fixed (byte* pa = wa)
                {
                    return _playerCreateYouTube(ecoreWindow, width, height, pv, maxHeight, pc, startMs, pa);
                }
            }
            finally
            {
                if (rv != null) ArrayPool<byte>.Shared.Return(rv);
                if (rc != null) ArrayPool<byte>.Shared.Return(rc);
                if (ra != null) ArrayPool<byte>.Shared.Return(ra);
            }
        }

        public static void YtPreload(string videoId) => CallVoidS(_ytPreload, videoId);
        public static void PlayerCancelPending() => _playerCancelPending();
        public static void YtPrewarmDecipher() => _ytPrewarmDecipher();
        public static IntPtr PlayerLastError() => _playerLastError();
        public static void PlayerTogglePause(IntPtr player) => _playerTogglePause(player);
        public static int PlayerSeek(IntPtr player, long positionMs) => _playerSeek(player, positionMs);

        public static void PlayerGetStats(IntPtr player, out PlayerStats stats)
        {
            stats = default;
            fixed (PlayerStats* p = &stats)
            {
                _playerGetStats(player, p);
            }
        }

        public static void PlayerDestroy(IntPtr player) => _playerDestroy(player);
        public static IntPtr PlayerFormats(IntPtr player) => _playerFormats(player);
        public static int PlayerSetRate(IntPtr player, double rate) => _playerSetRate(player, rate);
        public static void SetDefaultAudioDelay(int delayMs) => _setDefaultAudioDelay(delayMs);
        public static void PlayerSetAudioDelay(IntPtr player, int delayMs) => _playerSetAudioDelay(player, delayMs);
        public static int BtAudioDelayMs() => _btAudioDelayMs();

        public static IntPtr YtSearch(string query, string continuation, out uint len) => CallBinSS(_ytSearch, query, continuation, out len);
        public static IntPtr YtSuggest(string query) => CallPtrS(_ytSuggest, query);
        public static IntPtr YtBrowse(string browseId, string continuation, out uint len) => CallBinSS(_ytBrowse, browseId, continuation, out len);
        public static void Init(string dataDir) { EnsureLoaded(); CallVoidS(_init, dataDir); }
        public static IntPtr ConfigRefresh() => _configRefresh();
        public static void SetLocale(string language, string region) => CallVoidSS(_setLocale, language, region);
        public static int ReportAvailable() => _reportAvailable();
        public static IntPtr ReportUpload(string text) => CallPtrS(_reportUpload, text);
        public static IntPtr AuthStatus() => _authStatus();
        public static IntPtr AuthBegin() => _authBegin();
        public static IntPtr AuthPoll(string deviceCode) => CallPtrS(_authPoll, deviceCode);
        public static void AuthSignOut() => _authSignOut();
        public static IntPtr YtPlaylist(string videoId, string playlistId, out uint len) => CallBinSS(_ytPlaylist, videoId, playlistId, out len);
        public static IntPtr YtPlaylistPage(string playlistId, string continuation, out uint len) => CallBinSS(_ytPlaylistPage, playlistId, continuation, out len);
        public static IntPtr YtRelated(string videoId, string continuation, out uint len) => CallBinSS(_ytRelated, videoId, continuation, out len);
        public static IntPtr YtCaptions(string url, string translateTo, out uint len) => CallBinSS(_ytCaptions, url, translateTo, out len);
        public static IntPtr YtDetails(string videoId) => CallPtrS(_ytDetails, videoId);
        public static IntPtr YtSubscribe(string channelId, bool subscribe) => CallPtrSI(_ytSubscribe, channelId, subscribe ? 1 : 0);
        public static IntPtr YtComments(string videoId, string continuation, out uint len) => CallBinSS(_ytComments, videoId, continuation, out len);
        public static IntPtr YtPostComment(string createParams, string text) => CallPtrSS(_ytPostComment, createParams, text);
        public static IntPtr YtPostReply(string videoId, string commentId, string text) => CallPtrSSS(_ytPostReply, videoId, commentId, text);
        public static IntPtr YtCommentAction(string videoId, string commentId, string action) => CallPtrSSS(_ytCommentAction, videoId, commentId, action);
        public static IntPtr YtChannel(string channelId, string continuation, out uint len) => CallBinSS(_ytChannel, channelId, continuation, out len);
        public static IntPtr AuthAccount() => _authAccount();
        public static IntPtr AuthAccounts() => _authAccounts();
        public static IntPtr AuthSwitch(string id) => CallPtrS(_authSwitch, id);
        public static IntPtr SbSegments(string videoId) => CallPtrS(_sbSegments, videoId);
        public static void Free(IntPtr p) => _free(p);

        public static string Version => Marshal.PtrToStringUTF8((IntPtr)_version()) ?? "";

        public static string TlsInfo()
        {
            if (_tlsInfo == null) return "";
            Span<byte> buf = stackalloc byte[128];
            fixed (byte* p = buf)
            {
                int n = _tlsInfo(p, buf.Length);
                return n > 0 ? Encoding.UTF8.GetString(buf.Slice(0, n)) : "";
            }
        }

        public static string RecentLog(int maxLines)
        {
            byte[] buf = ArrayPool<byte>.Shared.Rent(8192);
            try
            {
                int n;
                fixed (byte* p = buf)
                {
                    n = _logTail(p, 8192, maxLines);
                }
                return n > 0 ? Encoding.UTF8.GetString(buf, 0, n) : "";
            }
            finally
            {
                ArrayPool<byte>.Shared.Return(buf);
            }
        }
    }
}
