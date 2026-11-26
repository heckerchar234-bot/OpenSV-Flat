#ifndef OPENSV_PLUS_OPENSV_H
#define OPENSV_PLUS_OPENSV_H

/* OpenSV+ C ABI. The same header works from C and C++ programs. */
#if defined(_WIN32) && defined(OPENSV_STATIC)
#define OPENSV_API
#elif defined(_WIN32) && defined(OPENSV_BUILDING_LIBRARY)
#define OPENSV_API __declspec(dllexport)
#elif defined(_WIN32)
#define OPENSV_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define OPENSV_API __attribute__((visibility("default")))
#else
#define OPENSV_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 3 for this compatible OpenSV+ API. */
OPENSV_API int opensv_api_version(void);

/* Load a voice and its DNNI models. A null or empty models_path uses the
   model location recorded in the voice document. Writes a UTF-8 error on
   failure and returns null. Serialize all calls made with the same handle. */
OPENSV_API void* opensv_create(const char* voice_path, const char* models_path,
                               char* error_buffer, int error_capacity);
OPENSV_API void opensv_destroy(void* handle);

/* Run "duration", "pitch" or "render". Request/response bodies are UTF-8
   JSON. The cancellation callback is checked between inference stages.
   The returned result belongs to the caller and must be released with
   opensv_result_free. An error result is a JSON object with an "error" field. */
OPENSV_API void* opensv_execute(void* handle, const char* operation,
                                const char* request_json,
                                int (*cancelled)(void* context), void* context);

/* Result pointers remain valid until opensv_result_free is called. Audio is
   mono, 32-bit float PCM at the sample rate returned by opensv_result_rate. */
OPENSV_API const char* opensv_result_json(void* result);
OPENSV_API const float* opensv_result_samples(void* result);
OPENSV_API int opensv_result_count(void* result);
OPENSV_API double opensv_result_rate(void* result);
OPENSV_API void opensv_result_free(void* result);

#ifdef __cplusplus
}
#endif

#endif
