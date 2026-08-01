/* JNI handlers for Android assets, streams, PlayerPrefs, display state, and
 * UnityPlayer host queries. */
#ifndef UNITY_JNI_H
#define UNITY_JNI_H

#include <stdarg.h>
#include <stdint.h>

/* FakeID is defined in jni_fake.c; we only read .cls/.name/.sig (all char[]). */

void  unity_jni_init(const char *data_root);
void  unity_jni_flush(void);                       /* clean-exit persistence fallback */
int   unity_owns_class(const char *cls);          /* 1 if this module handles it */
int   unity_owns_recv(void *recv);                /* 1 if recv is a UHandle */
const char *unity_recv_class(void *recv);         /* exact Java class for UHandle */
void  unity_handle_free(void *recv);

void     *unity_dispatch_object(void *recv, const void *id, va_list va);
uint64_t  unity_dispatch_int   (void *recv, const void *id, va_list va); /* int/bool/long */
float     unity_dispatch_float (void *recv, const void *id, va_list va);
void      unity_dispatch_void  (void *recv, const void *id, va_list va);

/* AndroidJavaObject and some Unity native wrappers call the JNI jvalue[] ("A")
 * entry points. These helpers consume that array directly; trying to forward it
 * through an empty va_list silently discarded every PlayerPrefs key/value. */
int unity_dispatch_object_a(void *recv, const void *id, const void *args, void **out);
int unity_dispatch_int_a   (void *recv, const void *id, const void *args, uint64_t *out);
int unity_dispatch_float_a (void *recv, const void *id, const void *args, float *out);
int unity_dispatch_void_a  (void *recv, const void *id, const void *args);

/* Boxed PlayerPrefs values returned by getAll() iteration. jni_fake.c routes
 * unbox calls (intValue/longValue/booleanValue/floatValue) and IsInstanceOf by
 * RECEIVER so only our own boxed handles are affected (other code's Integer/etc.
 * objects are untouched). */
int       unity_is_boxed   (void *recv);                 /* 1 if recv is our boxed value */
uint64_t  unity_boxed_int  (void *recv);                 /* int/long/bool payload */
float     unity_boxed_float (void *recv);                /* float payload */
int       unity_isinstance (void *obj, const char *clazz); /* 1/0 for boxed, -1 = not ours */

/* provided by jni_fake.c (see integration note 4) */
extern void       *jni_make_string(const char *utf);
extern void       *jni_make_object(const char *label);
extern void       *jni_bytearray_data(void *arr, int *len_out);
extern const char *jni_string_utf(void *jstr);

#endif /* UNITY_JNI_H */
