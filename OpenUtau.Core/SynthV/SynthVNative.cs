// Target: C# / .NET 10, Windows + Android arm64 (libopensv.so / opensv.dll)
// Wraps the OpenSV+ C ABI defined in opensv.h (api version 3).
using System;
using System.Runtime.InteropServices;

namespace OpenUtau.Core.SynthV {

    internal static unsafe class SynthVNative {
        // Matches CMake OUTPUT_NAME opensv_native in OpenSV+'s CMakeLists.txt.
        // Android: libopensv_native.so  |  Windows: opensv_native.dll  |  Linux/macOS: libopensv_native.so
        private const string LibName = "opensv_native";

        // Returns 3 for this compatible OpenSV+ API.
        [DllImport(LibName, EntryPoint = "opensv_api_version", CallingConvention = CallingConvention.Cdecl)]
        public static extern int ApiVersion();

        // Load a voice and its DNNI models. Returns null on failure (error written to errorBuffer).
        [DllImport(LibName, EntryPoint = "opensv_create", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern IntPtr Create(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string voicePath,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string modelsPath,
            byte* errorBuffer,
            int errorCapacity);

        [DllImport(LibName, EntryPoint = "opensv_destroy", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Destroy(IntPtr handle);

        // Execute "duration", "pitch", or "render".
        [DllImport(LibName, EntryPoint = "opensv_execute", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern IntPtr Execute(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string operation,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string requestJson,
            IntPtr cancelCallback,  // int(*)(void*) — pass IntPtr.Zero for no cancellation
            IntPtr cancelContext);

        [DllImport(LibName, EntryPoint = "opensv_result_json", CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr ResultJson(IntPtr result);

        [DllImport(LibName, EntryPoint = "opensv_result_samples", CallingConvention = CallingConvention.Cdecl)]
        public static extern float* ResultSamples(IntPtr result);

        [DllImport(LibName, EntryPoint = "opensv_result_count", CallingConvention = CallingConvention.Cdecl)]
        public static extern int ResultCount(IntPtr result);

        [DllImport(LibName, EntryPoint = "opensv_result_rate", CallingConvention = CallingConvention.Cdecl)]
        public static extern double ResultRate(IntPtr result);

        [DllImport(LibName, EntryPoint = "opensv_result_free", CallingConvention = CallingConvention.Cdecl)]
        public static extern void ResultFree(IntPtr result);

        // ── helpers ──────────────────────────────────────────────────────────

        public static string ResultJsonString(IntPtr result) {
            var ptr = ResultJson(result);
            return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(ptr) ?? string.Empty;
        }

        public static float[] ResultSamplesCopy(IntPtr result) {
            int count = ResultCount(result);
            if (count <= 0) return Array.Empty<float>();
            float* src = ResultSamples(result);
            var dst = new float[count];
            for (int i = 0; i < count; i++) dst[i] = src[i];
            return dst;
        }
    }
}
