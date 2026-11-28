using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using ForgeTube.Data;
using Tizen.NUI;
using Tizen.NUI.BaseComponents;
using static ForgeTube.I18n;

namespace ForgeTube.Ui
{
    // A titled horizontal strip of cards backed by a paged feed. The focused card stays at the left edge.
    // Lazy for smooth scrolling: cards exist only up to a little past the visible ones, and are filled
    // (texts, thumbnail) by UiWork nearest the focus first.
    public class FeedRow : View
    {
        private const int VisibleAhead = 6;
        private const int StripY = 64;

        public string Title { get; }
        public string Query { get; }   // search rows only (used by "add to home")
        public int Count => items.Count;
        public int FocusIndex { get; private set; }
        public VideoItem FocusedItem => items.Count > 0 ? items[FocusIndex] : null;
        public VideoItem FirstItem => items.Count > 0 ? items[0] : null;

        // Asks UiWork to call BuildNext. A ShelfList replaces it to order the work of all its rows.
        public Action RequestWork;

        private readonly Func<string, Task<FeedPage>> loader;
        private readonly Func<bool> pump;
        private readonly TextLabel status;
        private readonly View strip;
        private readonly List<VideoItem> items = new List<VideoItem>();
        private readonly Dictionary<int, Card> activeCards = new Dictionary<int, Card>();
        private readonly Stack<Card> cardPool = new Stack<Card>();
        private readonly List<int> recycleScratch = new List<int>();
        private const int MaxPoolSize = 14;
        private string continuation = "";
        private bool fresh;        // first network page received
        private bool loading;
        private bool focused;
        private Animation slide;
        private readonly int cardWidth;
        private readonly int cardGap;
        private readonly int leftInset;   // x of the focused card; earlier cards slide left of it (under the nav)
        public int RowHeight { get; }    // as built (a managed copy: the list lays rows out without native reads)
        public bool Shown { get; private set; } = true;   // managed mirror of Visibility
        private int SlideX => leftInset - FocusIndex * Step;

        private int Step => cardWidth + cardGap;

        public FeedRow(string title, string query, int width) : this(title, c => YouTube.Search(query, c), width)
        {
            Query = query;
        }

        public FeedRow(string title, Func<string, Task<FeedPage>> loader, int width,
                       int cardWidth = Theme.CardWidth, int leftInset = Theme.FocusMargin, int rowHeight = Theme.RowHeight,
                       int cardGap = Theme.CardGap)
        {
            Title = title;
            this.loader = loader;
            this.cardWidth = cardWidth;
            this.cardGap = cardGap;
            this.leftInset = leftInset;
            RowHeight = rowHeight;
            pump = BuildNext;
            RequestWork = () => UiWork.Request(pump);
            Size = new Size(width, rowHeight);

            var header = Widgets.Label(title, Theme.TitleSize - 4, Theme.Text, width - leftInset - 40);
            header.PositionX = leftInset;
            Add(header);

            status = Widgets.Label(T("Loading…"), Theme.BodySize, Theme.TextDim, width - leftInset - 40);
            status.Position = new Position(leftInset, 140);
            Add(status);

            strip = new View { Position = new Position(leftInset, StripY), Size = new Size(20000, Theme.CardHeight(cardWidth) + 40) };
            Add(strip);
        }

        protected override void Dispose(DisposeTypes type)
        {
            UiWork.Cancel(pump);
            if (slide != null) { slide.Stop(); slide.Dispose(); slide = null; }
            if (type == DisposeTypes.Explicit)
            {
                foreach (Card c in activeCards.Values)
                {
                    c.Unload(force: true);
                    strip.Remove(c);
                    c.Dispose();
                }
                activeCards.Clear();
                while (cardPool.Count > 0)
                {
                    Card c = cardPool.Pop();
                    strip.Remove(c);
                    c.Dispose();
                }
            }
            base.Dispose(type);
        }

        // Items already fetched (a home shelf): no first request; the loader only gives the next pages.
        // Called again with the network version of a cached shelf: cards of the same videos are kept.
        public void Seed(List<VideoItem> newItems, string nextContinuation)
        {
            fresh = true;
            continuation = nextContinuation ?? "";
            ReplaceItems(newItems);
            status.Text = items.Count == 0 ? T("No videos") : "";
            UpdateVisible();
        }

        // New items in place of the current ones. Existing active cards of the same videos are reused (their
        // thumbnail is already there), the others return to the pool; the focus index stays.
        private void ReplaceItems(List<VideoItem> newItems)
        {
            if (activeCards.TryGetValue(FocusIndex, out Card prev))
            {
                prev.SetFocused(false);   // its video may land elsewhere; UpdateVisible refocuses
            }
            Dictionary<string, Card> old = Card.IndexById(activeCards.Values);
            activeCards.Clear();
            items.Clear();
            items.AddRange(newItems);
            FocusIndex = Math.Min(FocusIndex, Math.Max(0, items.Count - 1));
            int winStart = Math.Max(0, FocusIndex - 2);
            int winEnd = Math.Min(items.Count - 1, FocusIndex + VisibleAhead + 1);
            for (int i = winStart; i <= winEnd; i++)
            {
                Card card = Card.Take(old, items[i]);
                if (card == null)
                {
                    card = TakeFromPoolOrNew(items[i]);
                }
                card.PositionX = i * Step;
                card.PositionY = 16;
                card.SetShown(true);
                activeCards[i] = card;
            }
            foreach (Card leftover in old.Values)
            {
                ReturnToPool(leftover);
            }
            slide = Widgets.Slide(strip, SlideX, StripY, slide);
        }

        private Card TakeFromPoolOrNew(VideoItem item)
        {
            if (cardPool.Count > 0)
            {
                Card recycled = cardPool.Pop();
                recycled.SetFocused(false);
                recycled.Unload(force: true);
                recycled.Rebind(item);
                return recycled;
            }
            var card = new Card(item, cardWidth) { PositionY = 16 };
            strip.Add(card);
            return card;
        }

        private void ReturnToPool(Card card)
        {
            card.SetFocused(false);
            card.Unload(force: true);
            card.SetShown(false);
            if (cardPool.Count < MaxPoolSize)
            {
                cardPool.Push(card);
            }
            else
            {
                strip.Remove(card);
                card.Dispose();
            }
        }

        private Card AcquireCard(int index)
        {
            if (activeCards.TryGetValue(index, out Card existing))
            {
                return existing;
            }
            Card card = TakeFromPoolOrNew(items[index]);
            card.PositionX = index * Step;
            card.PositionY = 16;
            activeCards[index] = card;
            return card;
        }

        public async Task Load()
        {
            if (loading)
            {
                return;
            }
            loading = true;
            FeedPage page;
            try
            {
                page = await loader(fresh ? continuation : "");
            }
            finally
            {
                loading = false;
            }
            if (Disposed)
            {
                return;
            }
            if (page.Error != null)
            {
                if (items.Count == 0)
                {
                    status.Text = T("Error: {0}", page.Error);
                }
                return;
            }
            if (!fresh)
            {
                fresh = true;
                ReplaceItems(page.Items);
            }
            else
            {
                items.AddRange(page.Items);
            }
            continuation = page.Continuation;
            status.Text = items.Count == 0 ? T("No videos") : "";
            UpdateVisible();
        }

        public void SetFocused(bool value)
        {
            if (focused == value)
            {
                return;
            }
            focused = value;
            if (items.Count > 0)
            {
                AcquireCard(FocusIndex).SetFocused(value);
            }
        }

        // Back to the first card (the list refreshes from the top).
        public void ResetFocus()
        {
            if (FocusIndex == 0 || items.Count == 0)
            {
                return;
            }
            if (activeCards.TryGetValue(FocusIndex, out Card prev))
            {
                prev.SetFocused(false);
            }
            FocusIndex = 0;
            slide = Widgets.Slide(strip, SlideX, StripY, slide);
            UpdateVisible();
        }

        // Returns false when the move would leave the row (so the caller can move focus elsewhere).
        public bool Move(int delta)
        {
            int next = FocusIndex + delta;
            if (items.Count == 0 || next < 0 || next >= items.Count)
            {
                return false;
            }
            if (activeCards.TryGetValue(FocusIndex, out Card prev))
            {
                prev.SetFocused(false);
            }
            FocusIndex = next;
            AcquireCard(FocusIndex).SetFocused(true);
            slide = Widgets.Slide(strip, SlideX, StripY, slide);
            UpdateVisible();
            MaybeLoadMore();
            return true;
        }

        // The next page of this row when the focus is near its end. A shelf that arrives with only a few cards (so
        // fewer than a screenful) asks for more at once instead of waiting for the first key press.
        public void MaybeLoadMore()
        {
            if (fresh && !loading && FocusIndex >= items.Count - VisibleAhead && !string.IsNullOrEmpty(continuation))
            {
                UiGuard.Fire("FeedRow.Load", Load);
            }
        }

        public void SetShown(bool shown)
        {
            if (shown != Shown)
            {
                Shown = shown;
                Widgets.SetVisible(this, shown);
            }
        }

        // Progress bars may have changed after watching something.
        public void RefreshProgress()
        {
            foreach (Card c in activeCards.Values)
            {
                c.UpdateProgress();
            }
        }

        public void UnloadAll()
        {
            UiWork.Cancel(pump);
            foreach (Card c in activeCards.Values)
            {
                c.Unload(force: true);
            }
        }

        // True view-pool virtualization: only cards around FocusIndex exist in activeCards; far ones recycle.
        private void UpdateVisible()
        {
            int keepMin = Math.Max(0, FocusIndex - 3);
            int keepMax = Math.Min(items.Count - 1, FocusIndex + VisibleAhead + 2);
            recycleScratch.Clear();
            foreach (int idx in activeCards.Keys)
            {
                if (idx < keepMin || idx > keepMax || idx >= items.Count)
                {
                    recycleScratch.Add(idx);
                }
            }
            foreach (int idx in recycleScratch)
            {
                Card c = activeCards[idx];
                activeCards.Remove(idx);
                ReturnToPool(c);
            }
            int showMin = Math.Max(0, FocusIndex - 2);
            int showMax = Math.Min(items.Count - 1, FocusIndex + VisibleAhead + 1);
            for (int i = showMin; i <= showMax; i++)
            {
                AcquireCard(i).SetShown(true);
            }
            foreach (KeyValuePair<int, Card> kv in activeCards)
            {
                if (kv.Key < showMin || kv.Key > showMax)
                {
                    kv.Value.SetShown(false);
                }
            }
            if (focused && activeCards.TryGetValue(FocusIndex, out Card fc))
            {
                fc.SetFocused(true);
            }
            if (Shown)
            {
                RequestWork();
            }
        }

        // Fills one card on screen or next to it, the focused one first, then rightwards (where the user goes),
        // then the one on the left. Returns false when there is none left to fill.
        public bool BuildNext()
        {
            if (Disposed || !Shown)
            {
                return false;
            }
            int last = Math.Min(items.Count - 1, FocusIndex + VisibleAhead);
            for (int i = FocusIndex; i <= last; i++)
            {
                if (activeCards.TryGetValue(i, out Card c) && !c.Filled)
                {
                    c.Fill();
                    return true;
                }
            }
            if (FocusIndex > 0 && activeCards.TryGetValue(FocusIndex - 1, out Card left) && !left.Filled)
            {
                left.Fill();
                return true;
            }
            return false;
        }
    }
}
