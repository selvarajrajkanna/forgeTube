using System;
using System.Collections.Generic;
using Tizen.NUI;
using Tizen.NUI.BaseComponents;
using static OmegaTube.I18n;

namespace OmegaTube.Ui
{
    // Shared 4x10 character grid + 5-key action row (Page, Space, Delete, action3, action4) used by SearchScreen and TextEntry.
    public class OnScreenKeyboard : View
    {
        public const int CharRows = 4, ActionRow = 4, ActionKeys = 5;

        private static readonly string[][] Qwerty =
        {
            new[] { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            new[] { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
            new[] { "a", "s", "d", "f", "g", "h", "j", "k", "l", "'" },
            new[] { "z", "x", "c", "v", "b", "n", "m", ",", ".", "-" },
        };
        private static readonly string[][] Azerty =
        {
            new[] { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            new[] { "a", "z", "e", "r", "t", "y", "u", "i", "o", "p" },
            new[] { "q", "s", "d", "f", "g", "h", "j", "k", "l", "m" },
            new[] { "w", "x", "c", "v", "b", "n", "é", "'", ".", "-" },
        };
        private static readonly string[][] QwertySpanish =
        {
            new[] { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            new[] { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
            new[] { "a", "s", "d", "f", "g", "h", "j", "k", "l", "ñ" },
            new[] { "z", "x", "c", "v", "b", "n", "m", ",", ".", "-" },
        };
        private static readonly string[][] Symbols =
        {
            new[] { "à", "â", "ä", "á", "é", "è", "ê", "ë", "í", "î" },
            new[] { "ï", "ó", "ô", "ö", "ú", "ù", "û", "ü", "ç", "ñ" },
            new[] { "œ", "æ", "ß", "¿", "¡", "?", "!", "&", "#", "@" },
            new[] { ":", ";", "/", "+", "=", "(", ")", "\"", "'", "_" },
        };

        private readonly List<List<TextLabel>> keys = new List<List<TextLabel>>();
        private readonly int highlightActionIndex;
        private int row = 1, col;
        private bool symbolPage;

        private string[][] Layout => symbolPage ? Symbols
                                   : Lang == "fr" ? Azerty : Lang == "es" ? QwertySpanish : Qwerty;

        public bool IsActionRow => row == ActionRow;
        public int ActionIndex => col;

        public OnScreenKeyboard(int keyW, int keyH, int keyGap, float actionFontSize, bool roundKeys,
                                string action3Label, string action4Label, int highlightActionIndex = -1)
        {
            this.highlightActionIndex = highlightActionIndex;
            int totalW = 10 * (keyW + keyGap) - keyGap;
            int totalH = (ActionRow + 1) * (keyH + keyGap) - keyGap;
            Size = new Size(totalW, totalH);

            int actionW = (totalW - (ActionKeys - 1) * keyGap) / ActionKeys;
            for (int r = 0; r <= ActionRow; r++)
            {
                var line = new List<TextLabel>();
                int x = 0;
                int count = r == ActionRow ? ActionKeys : 10;
                for (int c = 0; c < count; c++)
                {
                    int w = r == ActionRow ? actionW : keyW;
                    var key = Widgets.Label("", r == ActionRow ? actionFontSize : Theme.BodySize + 4, Theme.Text, w, keyH);
                    key.HorizontalAlignment = HorizontalAlignment.Center;
                    key.VerticalAlignment = VerticalAlignment.Center;
                    key.BackgroundColor = Theme.Surface;
                    if (roundKeys)
                    {
                        Widgets.Round(key, 10);
                    }
                    key.Position = new Position(x, r * (keyH + keyGap));
                    Add(key);
                    line.Add(key);
                    x += w + keyGap;
                }
                keys.Add(line);
            }
            LabelKeys(action3Label, action4Label);
        }

        public void SetActionLabel(int index, string text) => keys[ActionRow][index].Text = text;

        public void JumpToActionRow() => row = ActionRow;

        public void ToggleSymbols()
        {
            symbolPage = !symbolPage;
            string[][] layout = Layout;
            for (int r = 0; r < CharRows; r++)
            {
                for (int c = 0; c < 10; c++)
                {
                    keys[r][c].Text = layout[r][c];
                }
            }
            keys[ActionRow][0].Text = symbolPage ? "abc" : "àç?#";
        }

        public string CurrentChar() => row < CharRows ? Layout[row][col] : null;

        public bool MoveUp()
        {
            if (row == 0)
            {
                return false;
            }
            // From the action row, back onto the 10 columns under the wide key.
            col = row == ActionRow ? Math.Min(9, col * 10 / ActionKeys) : col;
            row--;
            return true;
        }

        public bool MoveDown()
        {
            if (row == ActionRow)
            {
                return false;
            }
            row++;
            // Map the 10 columns onto the wide keys of the action row.
            col = row == ActionRow ? Math.Min(ActionKeys - 1, col * ActionKeys / 10) : col;
            return true;
        }

        public bool MoveLeft()
        {
            if (col == 0)
            {
                return false;
            }
            col--;
            return true;
        }

        public bool MoveRight()
        {
            if (col >= keys[row].Count - 1)
            {
                return false;
            }
            col++;
            return true;
        }

        public void Paint(bool focused)
        {
            for (int r = 0; r < keys.Count; r++)
            {
                for (int c = 0; c < keys[r].Count; c++)
                {
                    bool f = focused && r == row && c == col;
                    bool hi = r == ActionRow && c == highlightActionIndex;
                    keys[r][c].BackgroundColor = f ? Theme.Accent : hi ? Theme.SurfaceHigh : Theme.Surface;
                }
            }
        }

        private void LabelKeys(string action3Label, string action4Label)
        {
            string[][] layout = Layout;
            for (int r = 0; r < CharRows; r++)
            {
                for (int c = 0; c < 10; c++)
                {
                    keys[r][c].Text = layout[r][c];
                }
            }
            List<TextLabel> actions = keys[ActionRow];
            actions[0].Text = symbolPage ? "abc" : "àç?#";
            actions[1].Text = T("Space");
            actions[2].Text = "← " + T("Delete");
            actions[3].Text = action3Label;
            actions[4].Text = action4Label;
        }
    }
}
