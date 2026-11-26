// Target: C# / .NET 10 — voice.json manifest for Synth V 2 voicebanks produced by OpenSVibeTuner.
using System.Collections.Generic;
using Newtonsoft.Json;

namespace OpenUtau.Core.SynthV {

    /// <summary>Deserialized voice.json manifest written by opensvibetuner export.</summary>
    public sealed class SynthVVoice {
        [JsonProperty("format_version")]  public int FormatVersion   { get; set; }
        [JsonProperty("name")]            public string Name         { get; set; } = "Unknown";
        [JsonProperty("version")]         public int Version         { get; set; }
        [JsonProperty("vendor")]          public string Vendor       { get; set; } = string.Empty;
        [JsonProperty("language")]        public string Language     { get; set; } = "english";
        [JsonProperty("phoneset")]        public string Phoneset     { get; set; } = "xsampa";
        [JsonProperty("support_languages")]
        public List<string> SupportLanguages { get; set; } = new();

        [JsonProperty("compatible_phonemes")]
        public Dictionary<string, List<string>> CompatiblePhonemes { get; set; } = new();

        [JsonProperty("language_tables")]
        public Dictionary<string, string> LanguageTables { get; set; } = new();

        [JsonProperty("model_files")]
        public Dictionary<string, SynthVModelEntry> ModelFiles { get; set; } = new();

        [JsonProperty("styles")]
        public List<SynthVStyle> Styles { get; set; } = new();

        // quality_validated is intentionally ignored — editor-only field.
    }

    public sealed class SynthVModelEntry {
        [JsonProperty("path")]               public string Path             { get; set; } = string.Empty;
        [JsonProperty("sha256")]             public string Sha256           { get; set; } = string.Empty;
        [JsonProperty("container_version")]  public int    ContainerVersion { get; set; }
    }

    public sealed class SynthVStyle {
        [JsonProperty("name")]  public string Name { get; set; } = string.Empty;
        // data = hex-encoded little-endian float32 array; not needed at render time.
        [JsonProperty("data")]  public string Data { get; set; } = string.Empty;
    }
}
