using System;
using System.Collections.Generic;
using Tizen.NUI;
using Tizen.NUI.BaseComponents;
using static ForgeTube.I18n;

namespace ForgeTube.Ui
{
    // Full-screen text input over the current screen (the player keeps playing under it): the search screen's
    // keyboard layouts, a growing text box, and Cancel / Send keys. The owner forwards keys to Key() and
    // removes the view when Done is called.
    public class TextEntry : View
    {
        private enum KeyAction { Page, Space, Delete, Cancel, Send }
        private const int KeyW = 104, KeyH = 76, KeyGap = 10;
        private const int PanelW = 1240, BoxW = PanelW - 120, BoxLines = 5;
        private const float BoxSize = Theme.BodySize + 4;

        // (text) on Send, null on Cancel / Back.
        public Action<string> Done;

        private readonly TextLabel box;
        private readonly TextLabel counter;
        private readonly OnScreenKeyboard keyboard;
        private readonly string placeholder;
        private readonly int maxLength;
        private string text;
        private bool busy;

        public string Text => text;

        public TextEntry(string title, string placeholder, string initial, int maxLength)
        {
            this.placeholder = placeholder;
            this.maxLength = maxLength;
            text = initial ?? "";
            Size = new Size(1920, 1080);
            BackgroundColor = new Color(0, 0, 0, 0.6f);

            int panelH = 110 + (int)(BoxLines * BoxSize * 1.35f) + 70 + 5 * (KeyH + KeyGap) + 40;
            var panel = Widgets.Rounded(new Color(0.09f, 0.09f, 0.09f, 0.98f), PanelW, panelH, Theme.PanelRadius);
            panel.Position = new Position((1920 - PanelW) / 2, (1080 - panelH) / 2);
            Add(panel);

            var heading = Widgets.Label(title, Theme.TitleSize, Theme.Text, PanelW - 120);
            heading.Position = new Position(60, 36);
            Widgets.Bold(heading);
            panel.Add(heading);

            int boxH = (int)(BoxLines * BoxSize * 1.35f) + 20;
            var frame = Widgets.Rounded(Theme.Surface, BoxW, boxH, 12);
            frame.Position = new Position(60, 100);
            panel.Add(frame);
            box = Widgets.Label("", BoxSize, Theme.Text, BoxW - 40, boxH - 20);
            box.MultiLine = true;
            box.Ellipsis = false;
            box.VerticalAlignment = VerticalAlignment.Top;
            box.Position = new Position(20, 10);
            frame.Add(box);
            counter = Widgets.Label("", Theme.SmallSize, Theme.TextDim, 300);
            counter.HorizontalAlignment = HorizontalAlignment.End;
            counter.Position = new Position(60 + BoxW - 300, 100 + boxH + 6);
            panel.Add(counter);

            int kbX = (PanelW - (10 * (KeyW + KeyGap) - KeyGap)) / 2;
            int kbY = 100 + boxH + 50;
            keyboard = new OnScreenKeyboard(KeyW, KeyH, KeyGap, Theme.BodySize, roundKeys: true,
                                            T("Cancel"), T("Send"), (int)KeyAction.Send);
            keyboard.Position = new Position(kbX, kbY);
            panel.Add(keyboard);
            Refresh();
        }

        // While sending: keys ignored, the Send key says so.
        public void SetBusy(bool value)
        {
            busy = value;
            keyboard.SetActionLabel((int)KeyAction.Send, value ? T("Sending…") : T("Send"));
        }

        private void Refresh()
        {
            int perLine = Widgets.CharsPerLine(BoxW - 40, BoxSize);
            if (text.Length == 0)
            {
                box.Text = placeholder;
                box.TextColor = Theme.TextDim;
            }
            else
            {
                // Last lines only, so the cursor stays in view.
                List<string> lines = Widgets.WrapLines(text, perLine);
                int from = Math.Max(0, lines.Count - BoxLines);
                box.Text = string.Join("\n", lines.GetRange(from, lines.Count - from)) + "_";
                box.TextColor = Theme.Text;
            }
            counter.Text = text.Length + " / " + maxLength;
            keyboard.Paint(focused: true);
        }

        public void Key(string key)
        {
            if (busy)
            {
                return;
            }
            switch (key)
            {
                case "Up":
                    keyboard.MoveUp();
                    break;
                case "Down":
                    keyboard.MoveDown();
                    break;
                case "Left":
                    keyboard.MoveLeft();
                    break;
                case "Right":
                    keyboard.MoveRight();
                    break;
                case "Return":
                    if (keyboard.IsActionRow)
                    {
                        Press((KeyAction)keyboard.ActionIndex);
                        return;
                    }
                    Type(keyboard.CurrentChar());
                    break;
                case "XF86Back":
                case "Escape":
                    Done?.Invoke(null);
                    return;
                default:
                    // Number keys of the remote.
                    if (key.Length == 1 && char.IsDigit(key[0]))
                    {
                        Type(key);
                    }
                    break;
            }
            Refresh();
        }

        private void Type(string s)
        {
            if (text.Length + s.Length <= maxLength)
            {
                text += s;
            }
        }

        private void Press(KeyAction action)
        {
            switch (action)
            {
                case KeyAction.Page:
                    keyboard.ToggleSymbols();
                    break;
                case KeyAction.Space:
                    if (text.Length > 0 && !text.EndsWith(" "))
                    {
                        Type(" ");
                    }
                    break;
                case KeyAction.Delete:
                    if (text.Length > 0)
                    {
                        text = text.Substring(0, text.Length - 1);
                    }
                    break;
                case KeyAction.Cancel:
                    Done?.Invoke(null);
                    return;
                case KeyAction.Send:
                    if (text.Trim().Length > 0)
                    {
                        Done?.Invoke(text.Trim());
                        return;
                    }
                    break;
            }
            Refresh();
        }
    }
}
