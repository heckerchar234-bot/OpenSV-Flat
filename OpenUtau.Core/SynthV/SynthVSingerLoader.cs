// Target: C# / .NET 10 — scans the singers directory for SynthV 2 voicebanks (voice.json).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using OpenUtau.Core.Util;
using Serilog;

namespace OpenUtau.Core.SynthV {

    public static class SynthVSingerLoader {

        private const string VoiceManifest = "voice.json";

        /// <summary>
        /// Enumerate every SynthV 2 voicebank found under <see cref="PathManager.SingersPath"/>.
        /// A voicebank is any directory (up to two levels deep) that contains voice.json at its root.
        /// </summary>
        public static IEnumerable<SynthVSinger> FindAllSingers() {
            var root = PathManager.Inst.SingersPath;
            if (!Directory.Exists(root)) yield break;

            // Top-level directories: singers/VoiceName/voice.json
            foreach (var dir in SafeEnumDirs(root)) {
                if (IsVoiceBank(dir)) {
                    var singer = TryLoad(dir);
                    if (singer != null) yield return singer;
                    continue;
                }
                // One level deeper: singers/vendor/VoiceName/voice.json
                foreach (var sub in SafeEnumDirs(dir)) {
                    if (!IsVoiceBank(sub)) continue;
                    var singer = TryLoad(sub);
                    if (singer != null) yield return singer;
                }
            }
        }

        // ── private ───────────────────────────────────────────────────────────

        private static bool IsVoiceBank(string dir) =>
            File.Exists(Path.Combine(dir, VoiceManifest));

        private static SynthVSinger? TryLoad(string dir) {
            try {
                return new SynthVSinger(dir);
            } catch (Exception e) {
                Log.Error(e, $"SynthVSingerLoader: failed to construct singer from {dir}");
                return null;
            }
        }

        private static IEnumerable<string> SafeEnumDirs(string path) {
            try {
                return Directory.EnumerateDirectories(path);
            } catch (Exception e) {
                Log.Warning(e, $"SynthVSingerLoader: cannot enumerate {path}");
                return Enumerable.Empty<string>();
            }
        }
    }
}
