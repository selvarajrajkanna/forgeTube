using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using OmegaTube.Data;
using Tizen.NUI;
using Tizen.NUI.BaseComponents;
using static OmegaTube.I18n;

namespace OmegaTube.Ui
{
    // A vertically scrolling grid of cards backed by a paged feed (Home, Subscriptions, History, sections).
    // The focused row is kept second from the top; more pages load well before the focus reaches the end.
    // Smoothness on the TV: cards exist only for the rows around the focus (and are shown only there: hidden
    // actors cost the renderer nothing); their texts and thumbnails are filled by UiWork, focused row first.
    // When the feed has highlights (Subscriptions: YouTube's "most relevant" shelf), they sit above the grid
    // as a horizontally scrolling row; Up from the first grid row enters it, Down leaves it.
    public class FeedGrid : View, IFeedView
    {
        private const int Columns = 4;
        private const int GapX = 34, GapY = 40;
        private const int Pad = Theme.FocusMargin;   // room for the focused card's scale-up
        private const int HeadingH = 56;              // section title above the highlights row and the grid
        private const int RowsBefore = 2, RowsAfter = 4;   // rows kept shown around the focused one
        private const int PrefetchRows = 5;           // next page asked when the focus is this close to the end

        public int Count => items.Count + highlights.Count;
        public bool ShowingPlaceholders => placeholderView != null;
        public VideoItem FocusedItem => inHighlights ? highlights[hlFocus].Item : items.Count > 0 ? items[focus] : null;
        public event Action InitialLoadFinished;
        private bool initialLoadNotified;

        private readonly int cardW, rowStep, height;
        private readonly View content;
        private readonly View hlStrip;
        private readonly TextLabel hlHeading, gridHeading;
        private readonly TextLabel status;
        private readonly List<VideoItem> items = new List<VideoItem>();
        private readonly Dictionary<int, Card> activeCards = new Dictionary<int, Card>();
        private readonly Stack<Card> cardPool = new Stack<Card>();
        private readonly List<int> recycleScratch = new List<int>();
        private const int MaxPoolSize = 36;
        private readonly List<Card> highlights = new List<Card>();
        private Func<string, Task<FeedPage>> loader;
        private string continuation = "";
        private bool fresh, loading, focused;
        private int focus;
        private bool inHighlights;
        private int hlFocus;
        private int gridTop;      // y of the first grid row inside content (below the highlights)
        private int generation;   // ignores pages of a previous Show()
        private Animation scroll, hlScroll;
        private readonly Func<bool> pump;
        private View emptyView;           // centered sign-in / empty-state card (built on first use)
        private TextLabel emptyMessage;
        private View placeholderView;
        private readonly int width;

        public FeedGrid(int width, int height)
        {
            pump = BuildNext;
            this.width = width;
            this.height = height;
            Size = new Size(width, height);
            ClippingMode = ClippingModeType.ClipChildren;
            cardW = (width - 2 * Pad - (Columns - 1) * GapX) / Columns;
            rowStep = Theme.CardHeight(cardW) + GapY;
            content = new View { Position = new Position(0, Pad), Size = new Size(width, 100000) };
            Add(content);
            hlHeading = Widgets.Label(T("Most relevant"), Theme.TitleSize - 6, Theme.Text, width - 2 * Pad, HeadingH);
            hlHeading.PositionX = Pad;
            hlStrip = new View { Position = new Position(0, HeadingH), Size = new Size(100000, rowStep) };
            gridHeading = Widgets.Label(T("All videos"), Theme.TitleSize - 6, Theme.Text, width - 2 * Pad, HeadingH);
            gridHeading.Position = new Position(Pad, HeadingH + rowStep);
            foreach (View v in new View[] { hlHeading, hlStrip, gridHeading })
            {
                v.Hide();
                content.Add(v);
            }
            status = Widgets.Label("", Theme.BodySize, Theme.TextDim, width - 2 * Pad, 120);
            status.MultiLine = true;
            status.Position = new Position(Pad, Pad + 20);
            Add(status);
        }

        // Replaces the content with another feed.
        public void Show(Func<string, Task<FeedPage>> newLoader, string emptyText = null)
        {
            generation++;
            hlHeading.Text = T("Most relevant");   // the app language may have changed since the last feed
            gridHeading.Text = T("All videos");
            ClearCards();
            focus = 0;
            continuation = "";
            fresh = false;
            loading = false;
            loader = newLoader;
            content.PositionY = Pad;
            if (loader == null)
            {
                status.Text = "";
                ShowEmpty(emptyText ?? "");
                return;
            }
            emptyView?.Hide();
            status.Text = "";
            ShowPlaceholders();
            AfterChange();
            UiGuard.Fire("FeedGrid.Load", Load);
        }

        private void ShowPlaceholders()
        {
            ClearPlaceholders();
            placeholderView = new View { Size = new Size(width, 2 * rowStep) };
            for (int r = 0; r < 2; r++)
            {
                for (int c = 0; c < Columns; c++)
                {
                    View card = Widgets.SkeletonCard(cardW);
                    card.Position = new Position(Pad + c * (cardW + GapX), r * rowStep);
                    placeholderView.Add(card);
                }
            }
            content.Add(placeholderView);
        }

        private void ClearPlaceholders()
        {
            if (placeholderView != null)
            {
                content.Remove(placeholderView);
                placeholderView.Dispose();
                placeholderView = null;
            }
        }

        public void Wake()
        {
            UiWork.Request(pump);
        }

        public void UnloadAll()
        {
            UiWork.Cancel(pump);
            foreach (Card c in activeCards.Values)
            {
                c.Unload(force: true);
            }
            foreach (Card c in highlights)
            {
                c.Unload(force: true);
            }
        }

        // Centered sign-in / empty-state card (used when a section has no loader, e.g. signed out): an account
        // glyph in a glass circle over the message, instead of a bare line of text in the corner.
        private void ShowEmpty(string message)
        {
            if (emptyView == null)
            {
                int panelW = Math.Min(width - 2 * Pad, 1040);
                const int panelH = 360;
                emptyView = Widgets.Rounded(Theme.Panel, panelW, panelH, Theme.PanelRadius + 4);
                emptyView.Position = new Position((width - panelW) / 2, Math.Max(Pad, (height - panelH) / 2 - 30));

                var circle = new ImageView { ResourceUrl = Theme.Image("circle_mask"), Color = Theme.SurfaceHigh,
                                             Size = new Size(128, 128), Position = new Position((panelW - 128) / 2, 46) };
                emptyView.Add(circle);
                var icon = new ImageView { ResourceUrl = Theme.Icon("set_account"), Size = new Size(66, 66), Position = new Position(31, 31) };
                circle.Add(icon);

                emptyMessage = Widgets.Label("", Theme.BodySize + 4, Theme.Text, panelW - 160, 150);
                emptyMessage.MultiLine = true;
                emptyMessage.HorizontalAlignment = HorizontalAlignment.Center;
                emptyMessage.Position = new Position(80, 210);
                emptyView.Add(emptyMessage);
                Add(emptyView);
            }
            emptyMessage.Text = message;
            emptyView.Show();
        }

        // Asks the first page again (new videos come first), back at the top right away so they do not land
        // under the focus. Cards of videos still there are kept. A next page on its way is dropped (generation).
        public void Refresh()
        {
            if (loader == null)
            {
                return;
            }
            generation++;
            loading = false;
            fresh = false;
            FocusedCard()?.SetFocused(false);
            focus = 0;
            hlFocus = 0;
            inHighlights = false;   // first grid card, as on opening (the highlights stay in view above)
            AfterChange();
            UiGuard.Fire("FeedGrid.Load", Load);
        }

        protected override void Dispose(DisposeTypes type)
        {
            UiWork.Cancel(pump);
            if (scroll != null) { scroll.Stop(); scroll.Dispose(); scroll = null; }
            if (hlScroll != null) { hlScroll.Stop(); hlScroll.Dispose(); hlScroll = null; }
            base.Dispose(type);
        }

        private void ClearCards()
        {
            ClearPlaceholders();
            items.Clear();
            foreach (Card c in activeCards.Values)
            {
                content.Remove(c);
                c.Dispose();
            }
            activeCards.Clear();
            while (cardPool.Count > 0)
            {
                Card c = cardPool.Pop();
                content.Remove(c);
                c.Dispose();
            }
            if (highlights.Count > 0)
            {
                SetHighlights(new List<VideoItem>());
            }
            inHighlights = false;
        }

        // Cards of videos already shown are kept (network page after the cached one).
        private void SetHighlights(List<VideoItem> newItems)
        {
            Dictionary<string, Card> old = Card.IndexById(highlights);
            highlights.Clear();
            foreach (VideoItem item in newItems)
            {
                Card card = Card.Take(old, item) ?? new Card(item, cardW);
                card.Position = new Position(Pad + highlights.Count * (cardW + GapX), 0);
                if (card.GetParent() == null)
                {
                    hlStrip.Add(card);
                }
                highlights.Add(card);
            }
            Card.Drop(old);
            bool any = highlights.Count > 0;
            Widgets.SetVisible(hlHeading, any);
            Widgets.SetVisible(hlStrip, any);
            Widgets.SetVisible(gridHeading, any);
            gridTop = any ? 2 * HeadingH + rowStep : 0;
            hlFocus = Math.Min(hlFocus, Math.Max(0, highlights.Count - 1));
            hlStrip.PositionX = 0;
        }

        // New items in place of the current ones, keeping active cards of videos still in the visible window.
        private void ReplaceItems(List<VideoItem> newItems)
        {
            Dictionary<string, Card> old = Card.IndexById(activeCards.Values);
            activeCards.Clear();
            items.Clear();
            items.AddRange(newItems);
            focus = Math.Min(focus, Math.Max(0, items.Count - 1));
            int focusRow = inHighlights ? 0 : focus / Columns;
            int minIdx = Math.Max(0, (focusRow - RowsBefore) * Columns);
            int maxIdx = Math.Min(items.Count - 1, (focusRow + RowsAfter + 1) * Columns - 1);
            for (int i = minIdx; i <= maxIdx; i++)
            {
                Card card = Card.Take(old, items[i]) ?? TakeFromPoolOrNew(items[i]);
                PlaceCard(card, i);
                card.SetShown(NearFocus(i / Columns));
                activeCards[i] = card;
            }
            foreach (Card leftover in old.Values)
            {
                ReturnToPool(leftover);
            }
        }

        private void PlaceCard(Card card, int i)
        {
            card.PositionX = Pad + (i % Columns) * (cardW + GapX);
            card.PositionY = gridTop + (i / Columns) * rowStep;
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
            var card = new Card(item, cardW);
            card.SetShown(false);
            content.Add(card);
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
                content.Remove(card);
                card.Dispose();
            }
        }

        private Card AcquireCard(int i)
        {
            if (activeCards.TryGetValue(i, out Card existing))
            {
                return existing;
            }
            Card card = TakeFromPoolOrNew(items[i]);
            PlaceCard(card, i);
            activeCards[i] = card;
            return card;
        }

        // Ensures active cards exist for the visible window around the focus.
        private void EnsureCards()
        {
            if (items.Count == 0)
            {
                return;
            }
            int focusRow = inHighlights ? 0 : focus / Columns;
            int minIdx = Math.Max(0, (focusRow - RowsBefore) * Columns);
            int maxIdx = Math.Min(items.Count - 1, (focusRow + RowsAfter + 1) * Columns - 1);
            for (int i = minIdx; i <= maxIdx; i++)
            {
                AcquireCard(i);
            }
        }

        private bool NearFocus(int row)
        {
            int focusRow = inHighlights ? 0 : focus / Columns;
            return row >= focusRow - RowsBefore && row <= focusRow + RowsAfter;
        }

        private async Task Load()
        {
            if (loading || loader == null)
            {
                return;
            }
            loading = true;
            int gen = generation;
            FeedPage page;
            try
            {
                page = await loader(fresh ? continuation : "");
            }
            finally
            {
                if (gen == generation)
                {
                    loading = false;
                }
            }
            if (gen != generation || Disposed)
            {
                return;  // the grid switched to another feed meanwhile
            }
            if (!initialLoadNotified)
            {
                initialLoadNotified = true;
                InitialLoadFinished?.Invoke();
            }
            if (page.Error != null)
            {
                ClearPlaceholders();
                if (Count == 0)
                {
                    status.Text = T("Error: {0}", page.Error);
                }
                return;
            }
            ClearPlaceholders();
            if (!fresh)
            {
                fresh = true;
                SetHighlights(page.Highlights);   // first: it moves the grid (gridTop)
                ReplaceItems(page.Items);
                focus = Math.Min(focus, Math.Max(0, items.Count - 1));
            }
            else
            {
                items.AddRange(page.Items);
            }
            continuation = page.Continuation;
            status.Text = Count == 0 ? T("No videos") : "";
            AfterChange();
            MaybePrefetch();
        }

        // Asks the next page early, while the user still has rows to look at.
        private void MaybePrefetch()
        {
            if (fresh && !loading && !string.IsNullOrEmpty(continuation) && focus / Columns >= (items.Count - 1) / Columns - PrefetchRows)
            {
                UiGuard.Fire("FeedGrid.Load", Load);
            }
        }

        private void AfterChange()
        {
            inHighlights = highlights.Count > 0 && (inHighlights || items.Count == 0);
            EnsureCards();
            foreach (KeyValuePair<int, Card> kv in activeCards)
            {
                kv.Value.SetFocused(focused && !inHighlights && kv.Key == focus);
            }
            for (int i = 0; i < highlights.Count; i++)
            {
                highlights[i].SetFocused(focused && inHighlights && i == hlFocus);
            }
            ScrollToFocus();
        }

        public void SetFocused(bool value)
        {
            if (focused == value)
            {
                return;
            }
            focused = value;
            FocusedCard()?.SetFocused(value);
        }

        private Card FocusedCard()
        {
            if (inHighlights)
            {
                return highlights.Count > 0 ? highlights[hlFocus] : null;
            }
            return items.Count > 0 && focus >= 0 && focus < items.Count ? AcquireCard(focus) : null;
        }

        public void RefreshProgress()
        {
            foreach (Card c in highlights)
            {
                c.UpdateProgress();
            }
            foreach (Card c in activeCards.Values)
            {
                c.UpdateProgress();
            }
        }

        // Arrow keys. Returns false when the move leaves the grid on that side (Left at the first column,
        // Up on the first row), so the screen can move the focus elsewhere.
        public bool Move(string key)
        {
            if (Count == 0)
            {
                return key != "Left" && key != "Up";
            }
            if (inHighlights)
            {
                return MoveInHighlights(key);
            }
            int next = focus;
            switch (key)
            {
                case "Left":
                    if (focus % Columns == 0)
                    {
                        return false;
                    }
                    next = focus - 1;
                    break;
                case "Right":
                    next = focus % Columns < Columns - 1 ? Math.Min(items.Count - 1, focus + 1) : focus;
                    break;
                case "Up":
                    if (focus < Columns)
                    {
                        if (highlights.Count == 0)
                        {
                            return false;
                        }
                        SwitchArea(true);
                        return true;
                    }
                    next = focus - Columns;
                    break;
                case "Down":
                    next = Math.Min(items.Count - 1, focus + Columns);
                    break;
            }
            if (next != focus)
            {
                if (activeCards.TryGetValue(focus, out Card prev))
                {
                    prev.SetFocused(false);
                }
                focus = next;
                AcquireCard(focus).SetFocused(true);
                ScrollToFocus();
                MaybePrefetch();
            }
            return true;
        }

        private bool MoveInHighlights(string key)
        {
            int next = hlFocus;
            switch (key)
            {
                case "Left":
                    if (hlFocus == 0)
                    {
                        return false;
                    }
                    next = hlFocus - 1;
                    break;
                case "Right":
                    next = Math.Min(highlights.Count - 1, hlFocus + 1);
                    break;
                case "Up":
                    return false;
                case "Down":
                    if (items.Count > 0)
                    {
                        SwitchArea(false);
                    }
                    return true;
            }
            if (next != hlFocus)
            {
                highlights[hlFocus].SetFocused(false);
                hlFocus = next;
                highlights[hlFocus].SetFocused(true);
                ScrollToFocus();
            }
            return true;
        }

        private void SwitchArea(bool toHighlights)
        {
            FocusedCard()?.SetFocused(false);
            inHighlights = toHighlights;
            FocusedCard()?.SetFocused(focused);
            ScrollToFocus();
        }

        private void ScrollToFocus()
        {
            if (highlights.Count > 0)
            {
                // Keep the focused highlight within the visible columns.
                int first = Math.Max(0, hlFocus - (Columns - 1));
                hlScroll = Widgets.Slide(hlStrip, -first * (cardW + GapX), HeadingH, hlScroll);
            }
            if (inHighlights)
            {
                scroll = Widgets.Slide(content, 0, Pad, scroll);
                UpdateVisibleRows();
                return;
            }
            int row = focus / Columns;
            // The first row keeps the highlights in view; further rows sit second from the top.
            int focusY = gridTop + row * rowStep;
            scroll = Widgets.Slide(content, 0, Pad - (row == 0 ? 0 : Math.Max(0, focusY - rowStep)), scroll);
            UpdateVisibleRows();
        }

        // True view-pool virtualization: recycles far off-screen cards back to cardPool and keeps activeCards O(1).
        private void UpdateVisibleRows()
        {
            int focusRow = inHighlights ? 0 : focus / Columns;
            int keepMinRow = Math.Max(0, focusRow - RowsBefore - 1);
            int keepMaxRow = focusRow + RowsAfter + 1;
            recycleScratch.Clear();
            foreach (int idx in activeCards.Keys)
            {
                int row = idx / Columns;
                if (idx >= items.Count || row < keepMinRow || row > keepMaxRow)
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
            EnsureCards();
            foreach (KeyValuePair<int, Card> kv in activeCards)
            {
                kv.Value.SetShown(NearFocus(kv.Key / Columns));
            }
            if (highlights.Count > 0)
            {
                int first = Math.Max(0, hlFocus - (Columns - 1));
                for (int i = 0; i < highlights.Count; i++)
                {
                    if (focusRow > RowsBefore + 1 || i < first - 2 || i > first + Columns + 2)
                    {
                        highlights[i].Unload();
                    }
                }
            }
            UiWork.Request(pump);
        }

        private static readonly int[] FillOrder = { 0, 1, -1, 2, 3, 4, -2 };

        // Fills one card per call: the focused row, then below (where the user goes), above, further rows,
        // and the highlights on screen. Returns false when everything near the focus is filled.
        private bool BuildNext()
        {
            if (Disposed || !Visibility)
            {
                return false;   // a hidden feed (other menu entry): Wake() when shown again
            }
            int first = Math.Max(0, hlFocus - (Columns - 1));
            if (inHighlights && FillHighlights(first))
            {
                return true;
            }
            int focusRow = inHighlights ? 0 : focus / Columns;
            foreach (int d in FillOrder)
            {
                int row = focusRow + d;
                for (int i = Math.Max(0, row * Columns); row >= 0 && i < Math.Min(items.Count, (row + 1) * Columns); i++)
                {
                    if (activeCards.TryGetValue(i, out Card c) && !c.Filled)
                    {
                        c.Fill();
                        return true;
                    }
                }
            }
            return focusRow <= 1 && FillHighlights(first);
        }

        private bool FillHighlights(int first)
        {
            for (int i = Math.Max(0, first - 1); i < Math.Min(highlights.Count, first + Columns + 1); i++)
            {
                if (!highlights[i].Filled)
                {
                    highlights[i].Fill();
                    return true;
                }
            }
            return false;
        }
    }
}
