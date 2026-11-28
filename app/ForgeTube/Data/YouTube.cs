using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using ForgeTube.Util;

namespace ForgeTube.Data
{
    public class VideoItem
    {
        public string Id, Title, Channel, Duration, Views, Published, Thumbnail;
        public bool Live;
        public int WatchedPercent;   // account progress reported by YouTube (0 when unknown)
        // Playlist / mix card: plays Id within PlaylistId. Videos of a playlist keep Kind "video" + PlaylistId.
        public string PlaylistId = "";
        public string Kind = "video";   // video | playlist | mix
        public bool IsList => Kind == "playlist" || Kind == "mix";
    }

    public class SkipSegment
    {
        public string Category;
        public long StartMs, EndMs;
        public bool Done;   // skipped once (or passed): never re-triggered
    }

    // One entry of the quality panel: the format a height cap selects.
    public class VideoQuality
    {
        public int Width, Height, Fps, FpsNum, FpsDen;
        public string Codec;   // avc1 | vp9 | av01
        public long Bitrate;
        public bool Hdr;
        public string HdrMode; // HDR10 | HLG | None
    }

    public class AudioTrack
    {
        public string Id, Name;
        public bool Default, Original, AutoDubbed, Descriptive;
    }

    public class CaptionTrack
    {
        public string Url, Lang, Name;
        public bool Auto, Translatable;
    }

    public class CaptionCue
    {
        public long StartMs, EndMs;
        public string Text;
    }

    // What the core resolved for the playing video: formats, audio languages, subtitles, details.
    public class PlaybackInfo
    {
        public List<VideoQuality> Qualities = new List<VideoQuality>();   // highest first
        public VideoQuality Current;
        public bool Live;
        public string Client = "", Container = "";
        public bool ContainerHdr;
        public int ColorPrimaries, ColorTransfer, ColorBits, ColorRange;
        public string Title = "", Description = "", Views = "", ChannelId = "";
        public List<AudioTrack> AudioTracks = new List<AudioTrack>();     // empty: single-language video
        public string AudioTrack = "";                                    // playing track id
        public string AudioCodec = "";                                    // e.g. mp4a.40.2, opus
        public long AudioBitrate;
        public List<CaptionTrack> Captions = new List<CaptionTrack>();
        public Dictionary<string, string> Translations = new Dictionary<string, string>();  // code -> name
    }

    // Watch-page details and the account's state for the playing video.
    public class VideoDetails
    {
        public string ChannelId = "", Channel = "", ChannelAvatar = "", Subscribers = "", Date = "";
        public string LikeCount = "";              // display count ("12K"), empty when unavailable
        public int Subscribed = -1;                // 1 / 0 / -1 unknown
        public string Error;
    }

    // A top-level comment (texts as YouTube displays them).
    public class Comment
    {
        public string Id = "", Author = "", Avatar = "", Text = "", Published = "", Likes = "", Replies = "", ReplyToken = "";
        public bool Pinned, Creator, Liked, Disliked;
    }

    // A page of comments; the first one also carries the header (count, sort tokens, posting params).
    public class CommentPage
    {
        public List<Comment> Items = new List<Comment>();
        public string Continuation = "", Count = "", SortTop = "", SortNewest = "", CreateParams = "";
        public bool Disabled;
        public string Error;
    }

    public class AccountInfo
    {
        public string Name, Handle, Photo, Error;
    }

    // One signed-in account for the profile picker / switcher.
    public class ProfileAccount
    {
        public string Id, Name, Handle, Photo;
        public bool Active;
    }

    // Why a video could not start (see omega_player_last_error).
    public class PlayError
    {
        public string Kind = "other";   // login | age | upcoming | unplayable | unavailable | rate_limited | http | network | formats | player | other
        public string Status = "";      // YouTube's playabilityStatus or "HTTP_<code>"
        public string Reason = "";      // YouTube's own message, in the app language
        public string Detail = "";      // technical error
        public bool SignedIn;
    }

    public class FeedPage
    {
        public List<VideoItem> Items = new List<VideoItem>();
        public List<VideoItem> Highlights = new List<VideoItem>();  // subscriptions: YouTube's "most relevant" shelf
        public string Continuation = "";
        public string Title = "";   // playlists, channels
        public string Avatar = "", Subscribers = "";   // channels
        public List<Shelf> Shelves = new List<Shelf>();   // home: rows by topic (Items then empty)
        public string Error;
    }

    // A titled row of the home; Continuation gives more items of that row (YouTube.Browse("", token)).
    public class Shelf
    {
        public string Title = "";
        public List<VideoItem> Items = new List<VideoItem>();
        public string Continuation = "";
    }

    // Async wrappers over the native core's YouTube data calls (network runs on the thread pool,
    // continuations come back on the UI thread through the Tizen synchronization context).
    public static class YouTube
    {
        public static Task<FeedPage> Search(string query, string continuation = "")
        {
            return Task.Run(() =>
            {
                IntPtr ptr = NativeCore.YtSearch(query, continuation ?? "", out uint len);
                return ParsePageBin(ptr, len);
            });
        }

        // The videos after videoId in a playlist / mix (page.Title: the list's name).
        public static Task<FeedPage> Playlist(string videoId, string playlistId)
        {
            return Task.Run(() =>
            {
                IntPtr ptr = NativeCore.YtPlaylist(videoId, playlistId, out uint len);
                return ParsePageBin(ptr, len);
            });
        }

        // A whole playlist, in order and paged (page.Title on the first page).
        public static Task<FeedPage> PlaylistPage(string playlistId, string continuation = "")
        {
            return Task.Run(() =>
            {
                IntPtr ptr = NativeCore.YtPlaylistPage(playlistId, continuation ?? "", out uint len);
                return ParsePageBin(ptr, len);
            });
        }

        public static Task<FeedPage> Channel(string channelId, string continuation = "")
        {
            return Task.Run(() =>
            {
                IntPtr ptr = NativeCore.YtChannel(channelId, continuation ?? "", out uint len);
                return ParsePageBin(ptr, len);
            });
        }

        public static Task<VideoDetails> Details(string videoId)
        {
            return Task.Run(() => ParseNative(
                NativeCore.YtDetails(videoId),
                root => new VideoDetails
                {
                    ChannelId = Json.Str(root, "channelId"), Channel = Json.Str(root, "channel"),
                    ChannelAvatar = Json.Str(root, "channelAvatar"), Subscribers = Json.Str(root, "subscribers"),
                    Date = Json.Str(root, "date"), LikeCount = Json.Str(root, "likeCount"),
                    Subscribed = (int)Json.Long(root, "subscribed", -1), Error = Json.Str(root, "error", null),
                },
                err => new VideoDetails { Error = err }));
        }

        public static Task<string> Subscribe(string channelId, bool subscribe) =>
            Task.Run(() => ParseNative(NativeCore.YtSubscribe(channelId, subscribe), root => Json.Str(root, "error", null), err => err));

        // continuation: "" = first page, else a page / sort continuation.
        public static Task<CommentPage> Comments(string videoId, string continuation = "")
        {
            return Task.Run(() =>
            {
                IntPtr ptr = NativeCore.YtComments(videoId, continuation ?? "", out uint len);
                return ParseCommentsBin(ptr, len);
            });
        }

        // Returns (the created comment or null, error or null).
        public static Task<(Comment, string)> PostComment(string createParams, string text)
        {
            return Task.Run(() => ParseNative(
                NativeCore.YtPostComment(createParams, text),
                root =>
                {
                    string error = Json.Str(root, "error", null);
                    return (error == null && Json.TryObj(root, "comment", out JsonElement c) ? ToComment(c) : null, error);
                },
                err => ((Comment)null, err)));
        }

        // A reply in the thread of commentId. Returns (the created reply or null, error or null).
        public static Task<(Comment, string)> PostReply(string videoId, string commentId, string text)
        {
            return Task.Run(() => ParseNative(
                NativeCore.YtPostReply(videoId, commentId, text),
                root =>
                {
                    string error = Json.Str(root, "error", null);
                    return (error == null && Json.TryObj(root, "comment", out JsonElement c) ? ToComment(c) : null, error);
                },
                err => ((Comment)null, err)));
        }

        // action: like, unlike, dislike, undislike. Returns the error or null.
        public static Task<string> CommentAction(string videoId, string commentId, string action) =>
            Task.Run(() => ParseNative(NativeCore.YtCommentAction(videoId, commentId, action), root => Json.Str(root, "error", null), err => err));

        private static Comment ToComment(JsonElement o)
        {
            return new Comment
            {
                Id = Json.Str(o, "id"), Author = Json.Str(o, "author"), Avatar = Json.Str(o, "avatar"),
                Text = Json.Str(o, "text"), Published = Json.Str(o, "published"), Likes = Json.Str(o, "likes"),
                Replies = Json.Str(o, "replies"), Pinned = Json.Bool(o, "pinned"), Creator = Json.Bool(o, "creator"),
                ReplyToken = Json.Str(o, "replyToken"),
                Liked = Json.Bool(o, "liked"), Disliked = Json.Bool(o, "disliked"),
            };
        }

        public static Task<FeedPage> Related(string videoId, string continuation = "")
        {
            return Task.Run(() =>
            {
                IntPtr ptr = NativeCore.YtRelated(videoId, continuation ?? "", out uint len);
                return ParsePageBin(ptr, len);
            });
        }

        // Signed-in feeds: FEwhat_to_watch (recommended), FEsubscriptions, FEhistory.
        public static Task<FeedPage> Browse(string browseId, string continuation = "")
        {
            return Task.Run(() =>
            {
                IntPtr ptr = NativeCore.YtBrowse(browseId, continuation ?? "", out uint len);
                return ParsePageBin(ptr, len);
            });
        }

        public static Task<List<string>> Suggest(string query)
        {
            return Task.Run(() => ParseNative(
                NativeCore.YtSuggest(query),
                root =>
                {
                    var list = new List<string>();
                    if (Json.TryArr(root, "suggestions", out JsonElement arr))
                    {
                        list.Capacity = arr.GetArrayLength();
                        foreach (JsonElement o in arr.EnumerateArray())
                        {
                            if (o.ValueKind == JsonValueKind.String)
                            {
                                string s = o.GetString();
                                if (s != null) list.Add(s);
                            }
                        }
                    }
                    return list;
                },
                _ => new List<string>()));
        }

        public static Task<List<SkipSegment>> SponsorSegments(string videoId)
        {
            return Task.Run(() => ParseNative(
                NativeCore.SbSegments(videoId),
                root =>
                {
                    var list = new List<SkipSegment>();
                    if (Json.TryArr(root, "segments", out JsonElement arr))
                    {
                        foreach (JsonElement o in arr.EnumerateArray())
                        {
                            list.Add(new SkipSegment
                            {
                                Category = Json.Str(o, "category"),
                                StartMs = (long)(Json.Num(o, "start") * 1000),
                                EndMs = (long)(Json.Num(o, "end") * 1000),
                            });
                        }
                    }
                    list.Sort((a, b) => a.StartMs.CompareTo(b.StartMs));
                    return list;
                },
                _ => new List<SkipSegment>()));
        }

        public static bool SignedIn() =>
            ParseNative(NativeCore.AuthStatus(), root => Json.Bool(root, "signedIn"), _ => false);

        // {deviceCode, userCode, verificationUrl, qrUrl, interval, expiresIn} or {error}
        public static Task<object> BeginSignIn() => Task.Run(() => Call(NativeCore.AuthBegin()));

        // "pending" | "slow_down" | "done" | "denied" | "expired" | "failed"
        public static Task<string> PollSignIn(string deviceCode) =>
            Task.Run(() => ParseNative(NativeCore.AuthPoll(deviceCode), root => Json.Str(root, "status", "failed"), _ => "failed"));

        public static void SignOut() => NativeCore.AuthSignOut();

        // Why the last PlayerCreateYouTube failed. No network call.
        public static PlayError LastPlayError()
        {
            return ParseNative(
                NativeCore.PlayerLastError(),
                root => new PlayError
                {
                    Kind = Json.Str(root, "kind", "other"), Status = Json.Str(root, "status"), Reason = Json.Str(root, "reason"),
                    Detail = Json.Str(root, "detail"), SignedIn = Json.Bool(root, "signedIn"),
                },
                err => new PlayError { Detail = err });
        }

        // What the player resolved (qualities, audio tracks, captions...). No network call.
        public static PlaybackInfo Playback(IntPtr player)
        {
            return ParseNative(
                NativeCore.PlayerFormats(player),
                root =>
                {
                    var info = new PlaybackInfo
                    {
                        Live = Json.Bool(root, "live", false),
                        Client = Json.Str(root, "client"),
                        Container = Json.Str(root, "container"),
                        ColorPrimaries = (int)Json.Long(root, "colorPrimaries"),
                        ColorTransfer = (int)Json.Long(root, "colorTransfer"),
                        ColorBits = (int)Json.Long(root, "colorBits"),
                        ColorRange = (int)Json.Long(root, "colorRange"),
                        ContainerHdr = Json.Bool(root, "containerHdr", false),
                        Title = Json.Str(root, "title"),
                        Description = Json.Str(root, "description"),
                        Views = Json.Str(root, "views"),
                        ChannelId = Json.Str(root, "channelId"),
                        AudioTrack = Json.Str(root, "audioTrack"),
                        AudioCodec = Json.Str(root, "audioCodec"),
                        AudioBitrate = Json.Long(root, "audioBitrate"),
                    };
                    if (Json.TryObj(root, "current", out JsonElement cur))
                    {
                        info.Current = ToQuality(cur);
                    }
                    if (Json.TryArr(root, "available", out JsonElement avail))
                    {
                        foreach (JsonElement o in avail.EnumerateArray())
                        {
                            info.Qualities.Add(ToQuality(o));
                        }
                    }
                    if (Json.TryArr(root, "audioTracks", out JsonElement tracks))
                    {
                        foreach (JsonElement o in tracks.EnumerateArray())
                        {
                            bool orig = Json.Bool(o, "original", Json.Bool(o, "default"));
                            info.AudioTracks.Add(new AudioTrack
                            {
                                Id = Json.Str(o, "id"),
                                Name = Json.Str(o, "name"),
                                Default = orig,
                                Original = orig,
                                AutoDubbed = Json.Bool(o, "autoDubbed"),
                                Descriptive = Json.Bool(o, "descriptive"),
                            });
                        }
                    }
                    if (Json.TryArr(root, "captions", out JsonElement caps))
                    {
                        foreach (JsonElement o in caps.EnumerateArray())
                        {
                            info.Captions.Add(new CaptionTrack
                            {
                                Url = Json.Str(o, "url"), Lang = Json.Str(o, "lang"), Name = Json.Str(o, "name"),
                                Auto = Json.Bool(o, "auto"), Translatable = Json.Bool(o, "translatable"),
                            });
                        }
                    }
                    if (Json.TryArr(root, "translations", out JsonElement trans))
                    {
                        foreach (JsonElement o in trans.EnumerateArray())
                        {
                            info.Translations[Json.Str(o, "code")] = Json.Str(o, "name");
                        }
                    }
                    return info;
                },
                _ => new PlaybackInfo());
        }

        // Timed subtitle lines of a caption track, machine-translated when translateTo is set. Null on error.
        public static Task<List<CaptionCue>> Captions(string url, string translateTo = "")
        {
            return Task.Run(() =>
            {
                IntPtr ptr = NativeCore.YtCaptions(url, translateTo ?? "", out uint len);
                return ParseCaptionsBin(ptr, len);
            });
        }

        public static Task<AccountInfo> Account()
        {
            return Task.Run(() => ParseNative(
                NativeCore.AuthAccount(),
                root => new AccountInfo
                {
                    Name = Json.Str(root, "name"), Handle = Json.Str(root, "handle"), Photo = Json.Str(root, "photo"),
                    Error = Json.Str(root, "error", null),
                },
                err => new AccountInfo { Error = err }));
        }

        // All signed-in accounts (cached profiles; no network call). The active one has Active == true.
        public static List<ProfileAccount> Accounts()
        {
            return ParseNative(
                NativeCore.AuthAccounts(),
                root =>
                {
                    var list = new List<ProfileAccount>();
                    if (Json.TryArr(root, "accounts", out JsonElement arr))
                    {
                        list.Capacity = arr.GetArrayLength();
                        foreach (JsonElement o in arr.EnumerateArray())
                        {
                            list.Add(new ProfileAccount
                            {
                                Id = Json.Str(o, "id"), Name = Json.Str(o, "name"), Handle = Json.Str(o, "handle"),
                                Photo = Json.Str(o, "photo"), Active = Json.Bool(o, "active"),
                            });
                        }
                    }
                    return list;
                },
                _ => new List<ProfileAccount>());
        }

        // Makes the account with this id active. True on success.
        public static bool SwitchAccount(string id) =>
            ParseNative(NativeCore.AuthSwitch(id), root => Json.Str(root, "error", null) == null, _ => false);

        public static string ActiveAccountId() => Accounts().Find(a => a.Active)?.Id ?? "";

        private static VideoQuality ToQuality(JsonElement o)
        {
            return new VideoQuality
            {
                Width = (int)Json.Long(o, "width"),
                Height = (int)Json.Long(o, "height"),
                Fps = (int)Json.Long(o, "fps"),
                FpsNum = (int)Json.Long(o, "fpsNum"),
                FpsDen = (int)Json.Long(o, "fpsDen"),
                Codec = Json.Str(o, "codec"),
                Bitrate = Json.Long(o, "bitrate"),
                Hdr = Json.Bool(o, "hdr"),
                HdrMode = Json.Str(o, "hdr_mode")
            };
        }

        // Zero-copy little-endian binary reader matching BinWriter in core/src/core.cpp.
        private ref struct BinReader
        {
            private readonly ReadOnlySpan<byte> buf;
            private int pos;

            public BinReader(ReadOnlySpan<byte> data)
            {
                buf = data;
                pos = 0;
            }

            public byte U8()
            {
                if (pos >= buf.Length) throw new InvalidOperationException("truncated binary payload");
                return buf[pos++];
            }

            public uint U32()
            {
                if (buf.Length - pos < 4) throw new InvalidOperationException("truncated binary payload");
                uint v = BinaryPrimitives.ReadUInt32LittleEndian(buf.Slice(pos, 4));
                pos += 4;
                return v;
            }

            public int I32()
            {
                if (buf.Length - pos < 4) throw new InvalidOperationException("truncated binary payload");
                int v = BinaryPrimitives.ReadInt32LittleEndian(buf.Slice(pos, 4));
                pos += 4;
                return v;
            }

            public long I64()
            {
                if (buf.Length - pos < 8) throw new InvalidOperationException("truncated binary payload");
                long v = BinaryPrimitives.ReadInt64LittleEndian(buf.Slice(pos, 8));
                pos += 8;
                return v;
            }

            public string Str()
            {
                int len = checked((int)U32());
                if (len == 0) return "";
                if (len < 0 || buf.Length - pos < len) throw new InvalidOperationException("truncated binary string");
                string s = Encoding.UTF8.GetString(buf.Slice(pos, len));
                pos += len;
                return s;
            }

            public string KindStr()
            {
                int len = checked((int)U32());
                if (len == 0) return "video";
                if (len < 0 || buf.Length - pos < len) throw new InvalidOperationException("truncated binary string");
                ReadOnlySpan<byte> slice = buf.Slice(pos, len);
                pos += len;
                if (slice.SequenceEqual("video"u8)) return "video";
                if (slice.SequenceEqual("playlist"u8)) return "playlist";
                if (slice.SequenceEqual("mix"u8)) return "mix";
                return Encoding.UTF8.GetString(slice);
            }
        }

        private static List<VideoItem> ReadFeedItems(ref BinReader r)
        {
            int count = checked((int)r.U32());
            var items = new List<VideoItem>(count);
            for (int i = 0; i < count; i++)
            {
                items.Add(new VideoItem
                {
                    Id = r.Str(),
                    Title = r.Str(),
                    Channel = r.Str(),
                    Duration = r.Str(),
                    Views = r.Str(),
                    Published = r.Str(),
                    Thumbnail = r.Str(),
                    PlaylistId = r.Str(),
                    Kind = r.KindStr(),
                    Live = r.U8() != 0,
                    WatchedPercent = r.I32(),
                });
            }
            return items;
        }

        private static unsafe FeedPage ParsePageBin(IntPtr ptr, uint len)
        {
            if (ptr == IntPtr.Zero)
            {
                return new FeedPage { Error = "null response" };
            }
            try
            {
                if (len == 0)
                {
                    return new FeedPage { Error = "empty response" };
                }
                var r = new BinReader(new ReadOnlySpan<byte>((void*)ptr, checked((int)len)));
                if (r.U8() == 0)
                {
                    return new FeedPage { Error = r.Str() };
                }
                var page = new FeedPage
                {
                    Continuation = r.Str(),
                    Title = r.Str(),
                    Avatar = r.Str(),
                    Subscribers = r.Str(),
                    Items = ReadFeedItems(ref r),
                    Highlights = ReadFeedItems(ref r),
                };
                int shelfCount = checked((int)r.U32());
                if (shelfCount > 0)
                {
                    page.Shelves.Capacity = shelfCount;
                    for (int i = 0; i < shelfCount; i++)
                    {
                        page.Shelves.Add(new Shelf
                        {
                            Title = r.Str(),
                            Continuation = r.Str(),
                            Items = ReadFeedItems(ref r),
                        });
                    }
                }
                return page;
            }
            catch (Exception e)
            {
                return new FeedPage { Error = e.Message };
            }
            finally
            {
                NativeCore.Free(ptr);
            }
        }

        private static unsafe CommentPage ParseCommentsBin(IntPtr ptr, uint len)
        {
            if (ptr == IntPtr.Zero)
            {
                return new CommentPage { Error = "null response" };
            }
            try
            {
                if (len == 0)
                {
                    return new CommentPage { Error = "empty response" };
                }
                var r = new BinReader(new ReadOnlySpan<byte>((void*)ptr, checked((int)len)));
                if (r.U8() == 0)
                {
                    return new CommentPage { Error = r.Str() };
                }
                var page = new CommentPage
                {
                    Continuation = r.Str(),
                    Count = r.Str(),
                    SortTop = r.Str(),
                    SortNewest = r.Str(),
                    CreateParams = r.Str(),
                    Disabled = r.U8() != 0,
                };
                int count = checked((int)r.U32());
                if (count > 0)
                {
                    page.Items.Capacity = count;
                    for (int i = 0; i < count; i++)
                    {
                        var c = new Comment
                        {
                            Id = r.Str(),
                            Author = r.Str(),
                            Avatar = r.Str(),
                            Text = r.Str(),
                            Published = r.Str(),
                            Likes = r.Str(),
                            Replies = r.Str(),
                            ReplyToken = r.Str(),
                        };
                        byte flags = r.U8();
                        c.Pinned = (flags & 1) != 0;
                        c.Creator = (flags & 2) != 0;
                        c.Liked = (flags & 4) != 0;
                        c.Disliked = (flags & 8) != 0;
                        page.Items.Add(c);
                    }
                }
                return page;
            }
            catch (Exception e)
            {
                return new CommentPage { Error = e.Message };
            }
            finally
            {
                NativeCore.Free(ptr);
            }
        }

        private static unsafe List<CaptionCue> ParseCaptionsBin(IntPtr ptr, uint len)
        {
            if (ptr == IntPtr.Zero)
            {
                return null;
            }
            try
            {
                if (len == 0)
                {
                    return null;
                }
                var r = new BinReader(new ReadOnlySpan<byte>((void*)ptr, checked((int)len)));
                if (r.U8() == 0)
                {
                    return null;
                }
                int count = checked((int)r.U32());
                var list = new List<CaptionCue>(count);
                for (int i = 0; i < count; i++)
                {
                    list.Add(new CaptionCue
                    {
                        StartMs = r.I64(),
                        EndMs = r.I64(),
                        Text = r.Str(),
                    });
                }
                list.Sort((a, b) => a.StartMs.CompareTo(b.StartMs));
                return list;
            }
            catch
            {
                return null;
            }
            finally
            {
                NativeCore.Free(ptr);
            }
        }

        private static unsafe T ParseNative<T>(IntPtr json, Func<JsonElement, T> read, Func<string, T> onError)
        {
            if (json == IntPtr.Zero)
            {
                return onError("null response");
            }
            try
            {
                ReadOnlySpan<byte> utf8 = MemoryMarshal.CreateReadOnlySpanFromNullTerminated((byte*)json);
                var reader = new Utf8JsonReader(utf8);
                using JsonDocument doc = JsonDocument.ParseValue(ref reader);
                return read(doc.RootElement);
            }
            catch (Exception e)
            {
                return onError(e.Message);
            }
            finally
            {
                NativeCore.Free(json);
            }
        }

        public static unsafe object Call(IntPtr json)
        {
            if (json == IntPtr.Zero)
            {
                return new Dictionary<string, object> { ["error"] = "null response" };
            }
            try
            {
                ReadOnlySpan<byte> utf8 = MemoryMarshal.CreateReadOnlySpanFromNullTerminated((byte*)json);
                return Json.Parse(utf8);
            }
            catch (Exception e)
            {
                return new Dictionary<string, object> { ["error"] = e.Message };
            }
            finally
            {
                NativeCore.Free(json);
            }
        }
    }
}
