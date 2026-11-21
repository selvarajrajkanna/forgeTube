namespace OmegaTube
{
    // App texts: English (India) default and English.
    public static class I18n
    {
        public static readonly string[] Codes = { "en-IN", "en" };
        public static readonly string[] Names = { "English (India)", "English" };

        // Current app language: "en-IN" (default) or "en".
        public static string Lang { get; set; } = "en-IN";

        public static string T(string en) => In(Lang, en);

        public static string T(string en, params object[] args) => string.Format(T(en), args);

        // The text in a given language instead of the current one.
        public static string In(string lang, string en) => en;

        // Decimal separator ('.' in both English and English (India)).
        public static string Decimal(string number) => number;
    }
}
