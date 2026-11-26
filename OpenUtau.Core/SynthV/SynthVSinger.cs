// Target: C# / .NET 10 — USinger implementation for Synth V 2 voicebanks.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Newtonsoft.Json;
using OpenUtau.Core.Ustx;
using Serilog;

namespace OpenUtau.Core.SynthV {

    public class SynthVSinger : USinger {
        // ── identity ──────────────────────────────────────────────────────────

        public override string Id            => voiceDir;
        public override string Name          => voice?.Name ?? Path.GetFileName(voiceDir);
        public override USingerType SingerType => USingerType.SynthV;
        public override string BasePath      => voiceDir;
        public override string Author        => voice?.Vendor ?? string.Empty;
        public override string Version       => voice?.Version.ToString() ?? string.Empty;
        public override string Location      => voiceDir;
        public override IList<string> Errors => errors;
        public override byte[] AvatarData    => avatarData;

        // ── public SV2-specific data ──────────────────────────────────────────

        /// <summary>Path to the directory containing voice.json.</summary>
        public string VoiceDir => voiceDir;

        /// <summary>Deserialized voice.json manifest.</summary>
        public SynthVVoice? Voice => voice;

        /// <summary>Default language from voice.json (e.g. "english", "japanese").</summary>
        public string DefaultLanguage => voice?.Language ?? "english";

        // ── private state ─────────────────────────────────────────────────────

        private readonly string voiceDir;
        private SynthVVoice? voice;
        private readonly List<string> errors = new();
        private byte[]? avatarData;

        // Handle lifetime: lazily created by SynthVRenderer, freed in FreeMemory().
        // Not the singer's responsibility to call opensv_create — only SynthVRenderer does.
        internal IntPtr NativeHandle { get; set; } = IntPtr.Zero;
        internal object HandleLock { get; } = new object();

        // ── construction ──────────────────────────────────────────────────────

        public SynthVSinger(string voiceDir) {
            this.voiceDir = voiceDir;
            found = true;
        }

        // ── lifecycle ─────────────────────────────────────────────────────────

        public override void EnsureLoaded() {
            if (loaded) return;
            Reload();
        }

        public override void Reload() {
            errors.Clear();
            voice = null;
            avatarData = null;
            loaded = false;

            var manifestPath = Path.Combine(voiceDir, "voice.json");
            if (!File.Exists(manifestPath)) {
                errors.Add($"voice.json not found in {voiceDir}");
                return;
            }

            try {
                var json = File.ReadAllText(manifestPath, Encoding.UTF8);
                voice = JsonConvert.DeserializeObject<SynthVVoice>(json);
                if (voice == null) {
                    errors.Add("Failed to parse voice.json");
                    return;
                }

                // Validate required model families.
                foreach (var family in new[] { "acoustic", "f0", "duration" }) {
                    if (!voice.ModelFiles.ContainsKey(family)) {
                        errors.Add($"voice.json is missing model family: {family}");
                    }
                }

                TryLoadAvatar();
                loaded = true;
            } catch (Exception e) {
                Log.Error(e, $"SynthVSinger: failed to load {manifestPath}");
                errors.Add(e.Message);
            }
        }

        public override void FreeMemory() {
            lock (HandleLock) {
                if (NativeHandle != IntPtr.Zero) {
                    SynthVNative.Destroy(NativeHandle);
                    NativeHandle = IntPtr.Zero;
                }
            }
        }

        // ── helpers ───────────────────────────────────────────────────────────

        private void TryLoadAvatar() {
            foreach (var candidate in new[] { "portrait.png", "portrait.jpg", "avatar.png", "avatar.jpg" }) {
                var path = Path.Combine(voiceDir, candidate);
                if (!File.Exists(path)) continue;
                try {
                    avatarData = File.ReadAllBytes(path);
                    return;
                } catch { /* non-fatal */ }
            }
        }
    }
}
