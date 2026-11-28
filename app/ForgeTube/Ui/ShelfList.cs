using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using ForgeTube.Data;
using Tizen.NUI;
using Tizen.NUI.BaseComponents;
using static ForgeTube.I18n;

namespace ForgeTube.Ui
{
    // The home as YouTube's TV app shows it: titled rows by topic ("Recommandations", "Jeux vidéo"...), each a
    // horizontally scrolling FeedRow, stacked vertically with the focused row at the top. More rows load as the
    // focus nears the last one; a row asks for more of its own items as its focus nears its end.
    // Card filling is ordered here for all rows: the focused row, then the one below, above, and so on.
    public class ShelfList : View, IFeedView
    {
        private const int RowsAround = 2;   // rows kept shown above / below the focused one
        private const int PrefetchRows = 3;
        private static readonly int[] FillOrder = { 0, 1, -1, 2, 3, -2 };   // offsets from the focused row

        public int Count => rows.Count;
        public bool ShowingPlaceholders => placeholderView != null;
        public VideoItem FocusedItem => rows.Count > 0 ? rows[focus].FocusedItem : null;
        public event Action InitialLoadFinished;
        private bool initialLoadNotified;

        private readonly int width;
        private readonly int leftInset;
        private readonly View content;
        private readonly TextLabel status;
        private readonly List<FeedRow> rows = new List<FeedRow>();
        private View placeholderView;
        private Func<string, Task<FeedPage>> loader;
        private string continuation = "";
        private bool fresh, loading, focused;
        private int focus;
        private int generation;
        private readonly Func<bool> pump;
        private Animation scroll;

        public ShelfList(int width, int height, int leftInset = Theme.FocusMargin)
        {
            this.width = width;
            this.leftInset = leftInset;
            pump = BuildNext;
            Size = new Size(width, height);
            ClippingMode = ClippingModeType.ClipToBoundingBox;
            content = new View { Size = new Size(width, 100000) };
            Add(content);
            status = Widgets.Label("", Theme.BodySize, Theme.TextDim, width - leftInset - Theme.FocusMargin, 120);
            status.MultiLine = true;
            status.Position = new Position(leftInset, Theme.FocusMargin + 20);
            Add(status);
        }

        private int RowTop(int i) => i * Theme.RowHeight;

        private void LayoutRows()
        {
            int y = 0;
            foreach (FeedRow row in rows)
            {
                row.PositionY = y;
                y += row.RowHeight;
            }
        }

        // loader("") gives the first rows, loader(continuation) the next ones.
        public void Show(Func<string, Task<FeedPage>> newLoader)
        {
            generation++;
            Clear();
            loader = newLoader;
            continuation = "";
            fresh = false;
            loading = false;
            focus = 0;
            content.PositionY = 0;
            status.Text = "";
            if (loader != null)
            {
                ShowPlaceholders();
            }
            UiGuard.Fire("ShelfList.Load", Load);
        }

        private void ShowPlaceholders()
        {
            ClearPlaceholders();
            placeholderView = new View { Size = new Size(width, 2 * Theme.RowHeight) };
            int step = Theme.CardWidth + Theme.CardGap;
            int cols = Math.Max(1, (width - leftInset + Theme.CardGap) / step);
            for (int r = 0; r < 2; r++)
            {
                int y = r * Theme.RowHeight;
                var titleBar = Widgets.Rounded(Theme.Surface, 260, 26, 8);
                titleBar.Position = new Position(leftInset, y + 12);
                placeholderView.Add(titleBar);
                for (int c = 0; c < cols; c++)
                {
                    View card = Widgets.SkeletonCard(Theme.CardWidth);
                    card.Position = new Position(leftInset + c * step, y + 64);
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
            foreach (FeedRow r in rows)
            {
                r.UnloadAll();
            }
        }

        // Asks the first rows again (new recommendations), back at the top right away so they do not land
        // under the focus. Rows and cards still showing the same thing are kept. A next page on its way is
        // dropped (generation).
        public void Refresh()
        {
            if (loader == null)
            {
                return;
            }
            generation++;
            loading = false;
            fresh = false;
            if (rows.Count > 0)
            {
                rows[focus].SetFocused(false);
                focus = 0;
                foreach (FeedRow r in rows)
                {
                    r.ResetFocus();
                }
                AfterChange();
            }
            UiGuard.Fire("ShelfList.Load", Load);
        }

        protected override void Dispose(DisposeTypes type)
        {
            UiWork.Cancel(pump);
            if (scroll != null) { scroll.Stop(); scroll.Dispose(); scroll = null; }
            if (type == DisposeTypes.Explicit)
            {
                Clear();
            }
            base.Dispose(type);
        }

        private void Clear()
        {
            ClearPlaceholders();
            foreach (FeedRow r in rows)
            {
                content.Remove(r);
                r.Dispose();
            }
            rows.Clear();
        }

        // `reuse`: rows shown from the cache, recycled when the shelf at the same place has the same title.
        private void AddShelves(List<Shelf> shelves, List<FeedRow> reuse = null)
        {
            foreach (Shelf s in shelves)
            {
                string title = s.Title.Length > 0 ? s.Title : T("Recommended");
                int i = rows.Count;
                FeedRow row = reuse != null && i < reuse.Count && reuse[i]?.Title == title ? reuse[i] : null;
                if (row != null)
                {
                    reuse[i] = null;
                }
                else
                {
                    // Items beyond the shelf's first ones: the same browse call with the row's continuation.
                    row = new FeedRow(title, c => YouTube.Browse("", c), width,
                                      Theme.CardWidth, leftInset, Theme.RowHeight, Theme.CardGap)
                    {
                        Position = new Position(0, RowTop(i)),
                        RequestWork = () => UiWork.Request(pump),
                    };
                    content.Add(row);
                }
                rows.Add(row);
                row.Seed(s.Items, s.Continuation);
            }
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
            catch (Exception e)
            {
                if (gen == generation)
                {
                    loading = false;   // or no later page would ever be asked for
                }
                UiGuard.Report("ShelfList.Load", e);
                return;
            }
            if (gen != generation || Disposed)
            {
                return;
            }
            loading = false;
            if (!initialLoadNotified)
            {
                initialLoadNotified = true;
                InitialLoadFinished?.Invoke();
            }
            if (page.Error != null)
            {
                ClearPlaceholders();
                if (rows.Count == 0)
                {
                    status.Text = T("Error: {0}", page.Error);
                }
                return;
            }
            ClearPlaceholders();
            List<FeedRow> reuse = null;
            if (!fresh)
            {
                // The first network page replaces the cached rows (keeping the focused row index, and the rows
                // and cards that show the same thing).
                fresh = true;
                reuse = new List<FeedRow>(rows);
                rows.Clear();
            }
            continuation = page.Continuation;
            AddShelves(page.Shelves, reuse);
            foreach (FeedRow r in reuse ?? new List<FeedRow>())
            {
                if (r != null)
                {
                    content.Remove(r);
                    r.Dispose();
                }
            }
            focus = Math.Min(focus, Math.Max(0, rows.Count - 1));
            status.Text = rows.Count == 0 ? T("No videos") : "";
            AfterChange();
            MaybePrefetch();
        }

        private void MaybePrefetch()
        {
            if (fresh && !loading && !string.IsNullOrEmpty(continuation) && focus >= rows.Count - 1 - PrefetchRows)
            {
                UiGuard.Fire("ShelfList.Load", Load);
            }
        }

        private void AfterChange()
        {
            LayoutRows();
            for (int i = 0; i < rows.Count; i++)
            {
                rows[i].SetFocused(focused && i == focus);
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
            if (rows.Count > 0)
            {
                rows[focus].SetFocused(value);
            }
        }

        public void RefreshProgress()
        {
            foreach (FeedRow r in rows)
            {
                r.RefreshProgress();
            }
        }

        // Arrow keys. Returns false when the move leaves the list on that side (Left at a row's first card).
        public bool Move(string key)
        {
            if (rows.Count == 0)
            {
                return key != "Left";
            }
            switch (key)
            {
                case "Left":
                    return rows[focus].Move(-1);
                case "Right":
                    rows[focus].Move(1);
                    return true;
                case "Up":
                case "Down":
                    int next = Math.Max(0, Math.Min(rows.Count - 1, focus + (key == "Up" ? -1 : 1)));
                    if (next != focus)
                    {
                        rows[focus].SetFocused(false);
                        focus = next;
                        rows[focus].SetFocused(true);
                        ScrollToFocus();
                        MaybePrefetch();
                    }
                    return true;
            }
            return true;
        }

        // The focused row at the top; rows far from it are hidden and their card textures unloaded.
        private void ScrollToFocus()
        {
            scroll = Widgets.Slide(content, 0, -RowTop(focus), scroll);
            for (int i = 0; i < rows.Count; i++)
            {
                bool show = i >= focus - RowsAround && i <= focus + RowsAround + 1;
                rows[i].SetShown(show);
                if (i < focus - RowsAround - 1 || i > focus + RowsAround + 2)
                {
                    rows[i].UnloadAll();
                }
            }
            if (rows.Count > 0)
            {
                rows[focus].MaybeLoadMore();   // a short first row (a few cards) fills up without a key press
            }
            UiWork.Request(pump);
        }

        // One card filled per call, in the rows around the focus (see FillOrder).
        private bool BuildNext()
        {
            if (Disposed || !Visibility)
            {
                return false;   // a hidden feed (other menu entry): Wake() when shown again
            }
            foreach (int d in FillOrder)
            {
                int i = focus + d;
                if (i >= 0 && i < rows.Count && rows[i].BuildNext())
                {
                    return true;
                }
            }
            return false;
        }
    }
}
