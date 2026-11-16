using System;
using OmegaTube.Data;

namespace OmegaTube.Ui
{
    // What the home screen needs from a feed shown beside the rail (rows by topic or a grid).
    public interface IFeedView
    {
        int Count { get; }
        VideoItem FocusedItem { get; }
        event Action InitialLoadFinished;
        void SetFocused(bool value);
        bool Move(string key);
        void RefreshProgress();
        void Refresh();
        void Wake();   // shown again: resume filling its cards
    }
}
