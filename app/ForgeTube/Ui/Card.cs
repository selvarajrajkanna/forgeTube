using System;
using System.Collections.Generic;
using ForgeTube.Data;
using Tizen.NUI;
using Tizen.NUI.BaseComponents;
using static ForgeTube.I18n;

namespace ForgeTube.Ui
{
    // A video card: rounded 16:9 thumbnail with an info box under it that lights up when focused,
    // duration badge bottom right, and a red progress bar for started videos.
    // Built in two steps for smooth scrolling: the constructor only makes the cheap frame (thumbnail, info box);
    // Fill() adds the texts and starts the thumbnail, when the card is about to be seen (UiWork).
    public class Card : View
    {
        public VideoItem Item { get; private set; }
        public bool Filled { get; private set; }
        public bool Shown { get; private set; } = true;   // managed mirror of Visibility (rows show / hide far cards)

        private readonly int width, thumbH;
        private static readonly Shadow FocusShadow = new Shadow(24f, new Color(0f, 0f, 0f, 0.7f), new Vector2(0f, 8f));
        private readonly ImageView thumb;
        private readonly ImageView info;
        private readonly View infoTopPatch;
        private readonly View progress;
        private TextLabel duration, title, meta;
        private Animation zoom;
        private bool focused;
        private bool thumbLoaded;
        private string loadedThumbUrl;

        public Card(VideoItem item, int width = Theme.CardWidth)
        {
            Item = item;
            this.width = width;
            thumbH = Theme.ThumbHeight(width);
            Size = new Size(width, Theme.CardHeight(width));
            // Scale around the centre so neighbours are overlapped evenly.
            PivotPoint = Tizen.NUI.PivotPoint.Center;
            PositionUsesPivotPoint = false;  // positions stay top-left based

            // Native Tizen 10 GPU-rounded placeholder surface while the thumbnail is deferred or loading.
            thumb = new ImageView { Size = new Size(width, thumbH + Theme.CardRadius), BackgroundColor = Theme.Surface };
            Widgets.Round(thumb, Theme.CardRadius);
            Add(thumb);

            info = new ImageView
            {
                Position = new Position(0, thumbH),
                Size = new Size(width, Theme.CardInfoHeight),
                ResourceUrl = Theme.Image("round9.9"),
                Color = Theme.CardInfo,
            };
            Add(info);
            infoTopPatch = Widgets.Box(Theme.CardInfo, width, Theme.CardRadius);
            info.Add(infoTopPatch);

            // On top of the info box's edge: the watched-fraction bar sits on the seam.
            progress = Widgets.Box(Theme.Accent, 0, 5);
            progress.Position = new Position(0, thumbH - 5);
            Add(progress);
            UpdateProgress();
        }

        // Called by Show / Hide of the row: remembers the state without a native read.
        public void SetShown(bool shown)
        {
            if (shown != Shown)
            {
                Shown = shown;
                Widgets.SetVisible(this, shown);
            }
        }

        private static (int w, int h) ThumbDecodeSize(string url, int displayW, int displayH)
        {
            // Never upscale 320x180 mqdefault.jpg thumbnails into larger RGBA GPU textures.
            if (url != null && url.IndexOf("mqdefault", StringComparison.Ordinal) >= 0)
            {
                return (Math.Min(displayW, 320), Math.Min(displayH, 180));
            }
            return (displayW, displayH);
        }

        // Texts, duration badge and thumbnail. Text labels are what costs (shaping on the UI thread).
        public void Fill()
        {
            if (Filled)
            {
                return;
            }
            Filled = true;
            string badge = Item.Live ? T("LIVE") : Item.Duration;
            if (!string.IsNullOrEmpty(badge))
            {
                duration = Widgets.Label(" " + badge + " ", Theme.SmallSize - 2, Theme.Text);
                duration.BackgroundColor = Item.Live ? Theme.Accent : new Color(0, 0, 0, 0.82f);
                Widgets.Round(duration, 4);
                duration.ParentOrigin = Tizen.NUI.ParentOrigin.TopLeft;
                duration.PivotPoint = Tizen.NUI.PivotPoint.BottomRight;
                duration.PositionUsesPivotPoint = true;
                duration.Position = new Position(width - 8, thumbH - 10);
                thumb.Add(duration);
            }

            title = Widgets.Label(Item.Title, Theme.BodySize, Theme.Text, width - 24, 62);
            title.MultiLine = true;
            title.Position = new Position(12, 8);
            info.Add(title);

            meta = Widgets.Label(MetaLine(Item), Theme.SmallSize, Theme.TextDim, width - 24);
            meta.Position = new Position(12, 70);
            info.Add(meta);
            ApplyFocusColors();

            if (!string.IsNullOrEmpty(Item.Thumbnail))
            {
                LoadThumb(Item.Thumbnail);
            }
        }

        private void LoadThumb(string url)
        {
            thumbLoaded = true;
            loadedThumbUrl = url;
            (int dw, int dh) = ThumbDecodeSize(url, width, (int)thumb.SizeHeight);
            Widgets.RoundedImage(thumb, url, Theme.CardRadius, dw, dh);
        }

        // Releases GPU thumbnail texture and rendered text surfaces while keeping the lightweight card frame.
        public void Unload(bool force = false)
        {
            if (!Filled || (focused && !force))
            {
                return;
            }
            Filled = false;
            if (thumbLoaded)
            {
                thumbLoaded = false;
                loadedThumbUrl = null;
                thumb.ResourceUrl = "";
            }
            if (duration != null)
            {
                thumb.Remove(duration);
                duration.Dispose();
                duration = null;
            }
            if (title != null)
            {
                info.Remove(title);
                title.Dispose();
                title = null;
            }
            if (meta != null)
            {
                info.Remove(meta);
                meta.Dispose();
                meta = null;
            }
        }

        // The same video in a fresher page (or a recycled card bound to another video): keeps or updates texts & thumb.
        public void Rebind(VideoItem item)
        {
            bool changed = Item?.Id != item?.Id || Item?.Thumbnail != item?.Thumbnail;
            Item = item;
            if (Filled)
            {
                title.Text = item.Title;
                meta.Text = MetaLine(item);
                string badge = item.Live ? T("LIVE") : item.Duration;
                if (!string.IsNullOrEmpty(badge))
                {
                    if (duration == null)
                    {
                        duration = Widgets.Label(" " + badge + " ", Theme.SmallSize - 2, Theme.Text);
                        Widgets.Round(duration, 4);
                        duration.ParentOrigin = Tizen.NUI.ParentOrigin.TopLeft;
                        duration.PivotPoint = Tizen.NUI.PivotPoint.BottomRight;
                        duration.PositionUsesPivotPoint = true;
                        duration.Position = new Position(width - 8, thumbH - 10);
                        thumb.Add(duration);
                    }
                    else
                    {
                        duration.Text = " " + badge + " ";
                    }
                    duration.BackgroundColor = item.Live ? Theme.Accent : new Color(0, 0, 0, 0.82f);
                }
                else if (duration != null)
                {
                    thumb.Remove(duration);
                    duration.Dispose();
                    duration = null;
                }
                if (changed || (!thumbLoaded && !string.IsNullOrEmpty(item.Thumbnail)))
                {
                    if (string.IsNullOrEmpty(item.Thumbnail))
                    {
                        thumbLoaded = false;
                        loadedThumbUrl = null;
                        thumb.ResourceUrl = "";
                    }
                    else if (loadedThumbUrl != item.Thumbnail)
                    {
                        thumb.ResourceUrl = "";
                        LoadThumb(item.Thumbnail);
                    }
                }
            }
            UpdateProgress();
        }

        private static string MetaLine(VideoItem item)
        {
            string line = item.Channel;
            if (!string.IsNullOrEmpty(item.Views))
            {
                line += " • " + item.Views;
            }
            if (!string.IsNullOrEmpty(item.Published))
            {
                line += " • " + item.Published;
            }
            return line;
        }

        public void UpdateProgress()
        {
            // Prefer our local position when played here (reflects rewinds accurately); fall back to YouTube's.
            double p = LocalStore.HasPosition(Item.Id) ? LocalStore.Progress(Item.Id) : Item.WatchedPercent / 100.0;
            progress.SizeWidth = (float)(width * p);
            Widgets.SetVisible(progress, p > 0.01);
        }

        // ------------------------------------------------------------------ focus

        private static readonly Vector3 FocusedScale = new Vector3(Theme.FocusScale, Theme.FocusScale, 1f);

        public void SetFocused(bool focused)
        {
            if (focused == this.focused)
            {
                return;
            }
            this.focused = focused;
            ApplyFocusColors();
            BoxShadow = focused ? FocusShadow : null;
            if (zoom == null)
            {
                zoom = new Animation(140);
            }
            else
            {
                zoom.Stop();
                zoom.Clear();
            }
            zoom.AnimateTo(this, "Scale", focused ? FocusedScale : Vector3.One, Widgets.EaseOut);
            zoom.Play();
            if (focused)
            {
                RaiseToTop();
            }
        }

        protected override void Dispose(DisposeTypes type)
        {
            if (zoom != null)
            {
                zoom.Stop();
                zoom.Dispose();
                zoom = null;
            }
            base.Dispose(type);
        }

        private void ApplyFocusColors()
        {
            Color infoColor = focused ? Theme.CardInfoFocused : Theme.CardInfo;
            info.Color = infoColor;   // tints the white nine-patch
            infoTopPatch.BackgroundColor = infoColor;
            if (Filled)
            {
                title.TextColor = focused ? Theme.CardTextFocused : Theme.Text;
                meta.TextColor = focused ? Theme.CardTextFocused : Theme.TextDim;
            }
        }

        // Card-recycling helpers for FeedGrid and FeedRow: keep cards whose video is still in the new feed.
        internal static Dictionary<string, Card> IndexById(IEnumerable<Card> list)
        {
            var d = new Dictionary<string, Card>();
            foreach (Card c in list)
            {
                string id = c.Item.Id ?? "";
                if (d.TryGetValue(id, out Card twin))
                {
                    twin.GetParent()?.Remove(twin);
                    twin.Dispose();
                }
                d[id] = c;
            }
            return d;
        }

        internal static Card Take(Dictionary<string, Card> pool, VideoItem item)
        {
            string id = item.Id ?? "";
            if (!pool.TryGetValue(id, out Card card))
            {
                return null;
            }
            pool.Remove(id);
            card.Rebind(item);
            return card;
        }

        internal static void Drop(Dictionary<string, Card> pool)
        {
            foreach (Card c in pool.Values)
            {
                c.GetParent()?.Remove(c);
                c.Dispose();
            }
        }
    }
}
