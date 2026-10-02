#include "filc_native.h"
#include "filc_runtime.h"

/* The framework's native half: a pending flag in each Fil-C object's header
 * and the slow path of the access hook.
 *
 * The flag lives in the object's aux word, next to Fil-C's own flags, in the
 * one flag bit Fil-C leaves free. Fil-C updates those flags with
 * compare-and-swap (see FILC_OBJECT_FLAG_WEAK_KEY), so this does too. The
 * compiler tests the flag inline on every access through an escaping pointer
 * and calls filc_resolve_pending only when it is set; the framework, a Fil-C
 * resolver registered here, then waits for the calls that own the object. */

#define FILC_OBJECT_FLAG_ASYNC_PENDING ((filc_object_flags)64)

_Static_assert(!(FILC_OBJECT_FLAG_ASYNC_PENDING &
                 (FILC_OBJECT_FLAG_GLOBAL | FILC_OBJECT_FLAG_READONLY |
                  FILC_OBJECT_FLAG_FREE | FILC_OBJECT_FLAG_MMAP |
                  FILC_OBJECT_FLAG_GLOBAL_AUX | FILC_OBJECT_FLAG_WEAK_KEY |
                  FILC_OBJECT_FLAGS_SPECIAL_MASK)),
               "the pending flag must not overlap a Fil-C object flag");

/* The framework's resolver, a Fil-C function taking the object's payload. */
static filc_ptr filc_async_resolver;

/* pending: 0 clears the flag, 1 sets it, 2 sets it for calls that only read
 * the object, unless the object is read-only (clearing it then): the program
 * cannot store into a read-only object, so it has nothing to wait for. A
 * read-only object's header is never written. */
PAS_API void filc_native_zasync_set_pending(filc_thread* my_thread,
                                            filc_ptr buf, int pending) {
  PAS_UNUSED_PARAM(my_thread);
  filc_object* object = filc_ptr_object(buf);
  if (!object)
    return;
  for (;;) {
    uintptr_t aux = object->aux;
    filc_object_flags flags = filc_aux_get_flags(aux);
    bool set = pending == 1 ||
               (pending == 2 && !(flags & FILC_OBJECT_FLAG_READONLY));
    filc_object_flags next = set ? flags | FILC_OBJECT_FLAG_ASYNC_PENDING
                                 : flags & ~FILC_OBJECT_FLAG_ASYNC_PENDING;
    if (next == flags)
      return;
    if (pas_compare_and_swap_uintptr_weak(
            &object->aux, aux, filc_aux_create(next, filc_aux_get_ptr(aux))))
      return;
  }
}

PAS_API void filc_native_zasync_set_resolver(filc_thread* my_thread,
                                             filc_ptr resolver) {
  filc_flight_ptr_store(my_thread, &filc_async_resolver, resolver);
}

/* Called by compiled code, only when an access goes to an object whose
 * pending flag was set. Hands the object to the framework's resolver, framed
 * like any native call into user code. */
PAS_API void filc_resolve_pending(void* ptr, void* lower) {
  PAS_UNUSED_PARAM(ptr);
  if (!lower)
    return;
  filc_object* object = filc_object_for_lower(lower);
  if (!(filc_object_get_flags(object) & FILC_OBJECT_FLAG_ASYNC_PENDING))
    return;
  if (filc_ptr_is_totally_null(filc_async_resolver))
    return;

  filc_thread* my_thread = filc_get_my_thread();
  bool was_top_native_frame_unlocked =
      my_thread->top_native_frame && !my_thread->top_native_frame->locked;
  if (was_top_native_frame_unlocked)
    filc_lock_top_native_frame(my_thread);

  FILC_DEFINE_FRAME("filc_resolve_pending");
  filc_push_frame(my_thread, frame);
  filc_native_frame native_frame;
  filc_push_native_frame(my_thread, &native_frame);

  filc_call_user_void_ptr(my_thread,
                          filc_flight_ptr_load(my_thread, &filc_async_resolver),
                          filc_ptr_create_with_object(my_thread, object));

  filc_pop_native_frame(my_thread, &native_frame);
  filc_pop_frame(my_thread, frame);
  if (was_top_native_frame_unlocked)
    filc_unlock_top_native_frame(my_thread);
}
