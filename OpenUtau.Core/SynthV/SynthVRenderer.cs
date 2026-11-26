// Target: C# / .NET 10, Windows + Android arm64
// IRenderer for Synth V 2 voicebanks via the OpenSV+ C ABI (libopensv / opensv.dll).
//
// Pipeline per phrase:
//   1. opensv_execute "duration"  — predict phoneme durations from note syllables
//   2. opensv_execute "render"    — synthesise audio using OPUM pitch curve + step-1 durations
//
// The native handle (opensv_create) is lazily created per singer and cached for the lifetime
// of the singer instance; FreeMemory() on the singer tears it down.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using OpenUtau.Core.Render;
using OpenUtau.Core.Ustx;
using Serilog;

namespace OpenUtau.Core.SynthV {

    [Renderer("SYNTHV", USingerType.SynthV)]
    public class SynthVRenderer : IRenderer {

        // OpenSV+ outputs 48 kHz mono PCM.
        private const int NativeSampleRate = 48000;
        // 5 ms frame = 0.005 s. Matches the OPUM pitch-tick interval.
        private const double FrameSeconds = 0.005;
        // Error buffer size for opensv_create.
        private const int ErrorBufferSize = 2048;

        public USingerType SingerType => USingerType.SynthV;
        public bool SupportsRenderPitch => false;

        public bool SupportsExpression(UExpressionDescriptor descriptor) => false;

        public RenderResult Layout(RenderPhrase phrase) {
            return new RenderResult {
                positionMs  = phrase.positionMs,
                leadingMs   = phrase.leadingMs,
                estimatedLengthMs = phrase.durationMs + phrase.leadingMs,
            };
        }

        public async Task<RenderResult> Render(
            RenderPhrase phrase,
            Progress progress,
            int trackNo,
            CancellationTokenSource cancellation,
            bool isPreRender = false,
            RenderPhraseEvents? renderEvents = null)
        {
            if (phrase.singer is not SynthVSinger singer) {
                Log.Warning("SynthVRenderer: singer is not SynthVSinger");
                return Layout(phrase);
            }

            singer.EnsureLoaded();
            if (!singer.Loaded || singer.Voice == null) {
                Log.Warning($"SynthVRenderer: singer {singer.Id} not loaded");
                return Layout(phrase);
            }

            var handle = GetOrCreateHandle(singer);
            if (handle == IntPtr.Zero) {
                Log.Warning($"SynthVRenderer: could not obtain native handle for {singer.Id}");
                return Layout(phrase);
            }

            return await Task.Run(() => {
                cancellation.Token.ThrowIfCancellationRequested();
                return DoRender(phrase, singer, handle, cancellation);
            }, cancellation.Token);
        }

        public RenderPitchResult LoadRenderedPitch(RenderPhrase phrase) =>
            new RenderPitchResult { ticks = Array.Empty<float>(), tones = Array.Empty<float>() };

        public UExpressionDescriptor[] GetSuggestedExpressions(USinger singer, URenderSettings renderSettings) =>
            Array.Empty<UExpressionDescriptor>();

        // ── render pipeline ───────────────────────────────────────────────────

        private unsafe RenderResult DoRender(
            RenderPhrase phrase,
            SynthVSinger singer,
            IntPtr handle,
            CancellationTokenSource cancellation)
        {
            var result = Layout(phrase);

            // ── step 1: duration ──────────────────────────────────────────────
            var durationJson = BuildDurationRequest(phrase, singer.DefaultLanguage);
            var durationResult = SynthVNative.Execute(
                handle, "duration", durationJson, IntPtr.Zero, IntPtr.Zero);

            if (durationResult == IntPtr.Zero) {
                Log.Warning("SynthVRenderer: duration execute returned null");
                return result;
            }

            List<DurationPhone>? phones;
            try {
                var durationResponse = SynthVNative.ResultJsonString(durationResult);
                phones = ParseDurationResponse(durationResponse);
            } finally {
                SynthVNative.ResultFree(durationResult);
            }

            if (phones == null || phones.Count == 0) {
                Log.Warning("SynthVRenderer: duration response had no phones");
                return result;
            }

            cancellation.Token.ThrowIfCancellationRequested();

            // ── step 2: render ────────────────────────────────────────────────
            var totalFrames = phones.Sum(p => p.Frames);
            var pitchMidi   = ResamplePitchToFrames(phrase, totalFrames);
            var renderJson  = BuildRenderRequest(phones, pitchMidi);

            var audioResult = SynthVNative.Execute(
                handle, "render", renderJson, IntPtr.Zero, IntPtr.Zero);

            if (audioResult == IntPtr.Zero) {
                Log.Warning("SynthVRenderer: render execute returned null");
                return result;
            }

            float[] samples;
            double sampleRate;
            try {
                // Check for JSON error field before reading audio.
                var audioJson = SynthVNative.ResultJsonString(audioResult);
                if (!string.IsNullOrEmpty(audioJson) && audioJson.Contains("\"error\"")) {
                    Log.Warning($"SynthVRenderer: render error: {audioJson}");
                    return result;
                }
                samples    = SynthVNative.ResultSamplesCopy(audioResult);
                sampleRate = SynthVNative.ResultRate(audioResult);
            } finally {
                SynthVNative.ResultFree(audioResult);
            }

            if (samples.Length == 0) return result;

            // Resample to 44100 Hz if needed (OPUM's mix engine runs at 44100).
            if (Math.Abs(sampleRate - 44100) > 1.0) {
                samples = Resample(samples, (int)sampleRate, 44100);
            }

            result.samples = samples;
            return result;
        }

        // ── request builders ──────────────────────────────────────────────────

        /// <summary>
        /// Build the "duration" JSON request from a RenderPhrase.
        /// Groups phrase.phones by noteIndex → one entry per note/syllable.
        /// </summary>
        private static string BuildDurationRequest(RenderPhrase phrase, string defaultLanguage) {
            var notePhones = new Dictionary<int, List<RenderPhone>>();
            foreach (var phone in phrase.phones) {
                if (!notePhones.TryGetValue(phone.noteIndex, out var list)) {
                    list = new List<RenderPhone>();
                    notePhones[phone.noteIndex] = list;
                }
                list.Add(phone);
            }

            var notes = new JArray();
            foreach (var note in phrase.notes) {
                if (!notePhones.TryGetValue(note.position, out var phones)) continue;
                var phonemesArr = new JArray(phones.Select(p => (JToken)p.phoneme));
                notes.Add(new JObject {
                    ["language"] = defaultLanguage,
                    ["duration"] = note.durationMs / 1000.0,
                    ["tone"]     = note.tone,
                    ["phones"]   = phonemesArr,
                });
            }

            return new JObject { ["notes"] = notes }.ToString(Formatting.None);
        }

        /// <summary>
        /// Build the "render" JSON request from duration-predicted phones and resampled pitch.
        /// </summary>
        private static string BuildRenderRequest(List<DurationPhone> phones, float[] pitchMidi) {
            var phonesArr = new JArray(phones.Select(p => new JObject {
                ["language"] = p.Language,
                ["symbol"]   = p.Symbol,
                ["frames"]   = p.Frames,
            }));

            var pitchArr = new JArray(pitchMidi.Select(f => (JToken)(double)f));

            return new JObject {
                ["phones"]   = phonesArr,
                ["pitch"]    = pitchArr,
                ["controls"] = new JObject(),
            }.ToString(Formatting.None);
        }

        // ── duration response parser ──────────────────────────────────────────

        private static List<DurationPhone>? ParseDurationResponse(string json) {
            if (string.IsNullOrWhiteSpace(json)) return null;
            try {
                var root = JObject.Parse(json);
                if (root.ContainsKey("error")) {
                    Log.Warning($"SynthVRenderer: duration error: {root["error"]}");
                    return null;
                }
                var arr = root["phones"] as JArray;
                if (arr == null) return null;
                var result = new List<DurationPhone>(arr.Count);
                foreach (JObject item in arr) {
                    var durationSec = (double)(item["duration"] ?? 0.1);
                    int frames = Math.Max(1, (int)Math.Round(durationSec / FrameSeconds));
                    result.Add(new DurationPhone {
                        Language = (string)(item["language"] ?? "english"),
                        Symbol   = (string)(item["symbol"]   ?? "SP"),
                        Frames   = frames,
                    });
                }
                return result;
            } catch (Exception e) {
                Log.Warning(e, "SynthVRenderer: failed to parse duration response");
                return null;
            }
        }

        // ── pitch resampler ───────────────────────────────────────────────────

        /// <summary>
        /// Resample the OPUM pitch array (5-tick intervals, centitones) to per-5ms-frame MIDI floats.
        /// phrase.pitches is in centitones (semitone * 100). One frame = 5 ms.
        /// The OPUM pitch tick interval is also 5 ticks, but ticks ≠ ms; we use timeAxis for conversion.
        /// </summary>
        private static float[] ResamplePitchToFrames(RenderPhrase phrase, int totalFrames) {
            var output = new float[totalFrames];

            if (phrase.pitches == null || phrase.pitches.Length == 0) {
                // Fill with middle-C as a safe fallback.
                for (int i = 0; i < totalFrames; i++) output[i] = 60f;
                return output;
            }

            // The pitches array starts at (phrase.position - phrase.leading) in ticks,
            // at 5-tick intervals. Convert each output frame (5ms, starting at phrase start)
            // to a tick position, then index into pitches.
            const int pitchInterval = 5; // ticks
            int pitchStart = phrase.position - phrase.leading;

            var axis = phrase.timeAxis;
            double frameStartMs = phrase.positionMs - phrase.leadingMs;

            for (int frame = 0; frame < totalFrames; frame++) {
                double frameMs = frameStartMs + frame * FrameSeconds * 1000.0;
                int tick = axis.MsPosToTickPos(frameMs);
                int pitchIndex = (tick - pitchStart) / pitchInterval;
                pitchIndex = Math.Clamp(pitchIndex, 0, phrase.pitches.Length - 1);
                // centitones → MIDI semitone
                output[frame] = phrase.pitches[pitchIndex] / 100f;
            }

            return output;
        }

        // ── sample rate converter (linear interpolation) ──────────────────────

        private static float[] Resample(float[] src, int srcRate, int dstRate) {
            if (srcRate == dstRate) return src;
            int dstLength = (int)((long)src.Length * dstRate / srcRate);
            var dst = new float[dstLength];
            double ratio = (double)src.Length / dstLength;
            for (int i = 0; i < dstLength; i++) {
                double pos = i * ratio;
                int lo = (int)pos;
                int hi = Math.Min(lo + 1, src.Length - 1);
                double t = pos - lo;
                dst[i] = (float)(src[lo] * (1 - t) + src[hi] * t);
            }
            return dst;
        }

        // ── native handle management ──────────────────────────────────────────

        private static unsafe IntPtr GetOrCreateHandle(SynthVSinger singer) {
            lock (singer.HandleLock) {
                if (singer.NativeHandle != IntPtr.Zero) return singer.NativeHandle;

                var errorBytes = new byte[ErrorBufferSize];
                fixed (byte* err = errorBytes) {
                    var handle = SynthVNative.Create(
                        singer.VoiceDir,
                        string.Empty,  // use the path recorded in voice.json
                        err,
                        ErrorBufferSize);

                    if (handle == IntPtr.Zero) {
                        var message = Encoding.UTF8.GetString(errorBytes).TrimEnd('\0');
                        Log.Error($"SynthVRenderer: opensv_create failed: {message}");
                        return IntPtr.Zero;
                    }

                    singer.NativeHandle = handle;
                    return handle;
                }
            }
        }

        // ── inner types ───────────────────────────────────────────────────────

        private sealed class DurationPhone {
            public string Language { get; set; } = "english";
            public string Symbol   { get; set; } = "SP";
            public int    Frames   { get; set; } = 1;
        }
    }
}
