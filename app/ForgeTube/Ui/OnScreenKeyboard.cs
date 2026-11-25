using System;
using System.Collections.Generic;
using Tizen.NUI;
using Tizen.NUI.BaseComponents;
using static ForgeTube.I18n;

namespace ForgeTube.Ui
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
        private int paintedRow = -1, paintedCol = -1;
        private bool paintedFocused;
        private bool symbolPage;

        private string[][] KeyLayout => symbolPage ? Symbols : Qwerty;

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

        public void JumpToTopRow()
        {
            row = 0;
            col = Math.Min(col, 9);
        }

        public void JumpToActionRow()
        {
            row = ActionRow;
            col = Math.Min(col, ActionKeys - 1);
        }

        public void ToggleSymbols()
        {
            symbolPage = !symbolPage;
            string[][] layout = KeyLayout;
            for (int r = 0; r < CharRows; r++)
            {
                for (int c = 0; c < 10; c++)
                {
                    keys[r][c].Text = layout[r][c];
                }
            }
            keys[ActionRow][0].Text = symbolPage ? "abc" : "àç?#";
        }

        public string CurrentChar() => row < CharRows ? KeyLayout[row][col] : null;

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
            if (paintedRow < 0)
            {
                for (int r = 0; r < keys.Count; r++)
                {
                    for (int c = 0; c < keys[r].Count; c++)
                    {
                        PaintKey(r, c, focused && r == row && c == col);
                    }
                }
            }
            else
            {
                if (paintedRow == row && paintedCol == col && paintedFocused == focused)
                {
                    return;
                }
                if (paintedRow < keys.Count && paintedCol < keys[paintedRow].Count)
                {
                    PaintKey(paintedRow, paintedCol, false);
                }
                PaintKey(row, col, focused);
            }
            paintedRow = row;
            paintedCol = col;
            paintedFocused = focused;
        }

        private void PaintKey(int r, int c, bool isFocused)
        {
            if (r < 0 || r >= keys.Count || c < 0 || c >= keys[r].Count)
            {
                return;
            }
            bool hi = r == ActionRow && c == highlightActionIndex;
            keys[r][c].BackgroundColor = isFocused ? Theme.Accent : hi ? Theme.SurfaceHigh : Theme.Surface;
        }

        private void LabelKeys(string action3Label, string action4Label)
        {
            string[][] layout = KeyLayout;
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
