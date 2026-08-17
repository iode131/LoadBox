/*
 * elk_extensions.h
 *
 * Travesty-style declarations of the optional Elk Audio VST3 extension
 * interfaces, so a plugin built against travesty (rather than the Steinberg
 * SDK) can expose string properties to Sushi.
 *
 * VST3 has no string-valued parameters. Elk fills that gap with a private
 * pair of interfaces, discovered through the normal queryInterface mechanism:
 * hosts that do not know them simply get V3_NO_INTERFACE and carry on.
 *
 *   IElkControllerExtension        queried by the host off the edit controller;
 *                                  lets the host enumerate, read and write
 *                                  string properties. All calls are made from
 *                                  a non-realtime thread.
 *   IElkComponentHandlerExtension  queried by the plugin off the component
 *                                  handler; lets the plugin tell the host that
 *                                  a property value changed. Must not be
 *                                  called from the audio thread.
 *
 * These declarations mirror, field for field and method for method,
 * elk_vst3_extensions.h from https://github.com/elk-audio/elk-plugin-extensions
 * as consumed by Sushi. C++ reference parameters are passed as pointers in the
 * platform ABI, so `elk::PropertyInfo&` becomes `elk_property_info*` and
 * `const elk::PropertyValue&` becomes `const elk_property_value*`.
 *
 * Deliberately declared outside travesty's align_push.h/align_pop.h block,
 * matching the Elk header, which sets no packing of its own.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "base.h"

#define ELK_STRING_PROPERTY_DEFAULT_LENGTH 65535

/**
 * A string property value. On the way out of the plugin, `value` points at
 * plugin-owned memory that the host copies before the call returns. On the way
 * in, it points at host-owned memory valid only for the duration of the call.
 */
struct elk_property_value {
	const char* value;
	int32_t length; // in chars, not counting any null terminator
};

/**
 * Description of one string property. `id` must not collide with any
 * parameter id: hosts register properties and parameters in one id space.
 */
struct elk_property_info {
	v3_param_id id;
	v3_str_128 name;  // unique, machine readable, e.g. "ir_file_left"
	v3_str_128 label; // display name, e.g. "IR File Left"
	int32_t flags;    // see below
};

enum {
	ELK_PROPERTY_NO_FLAGS            = 0,
	ELK_PROPERTY_IS_READ_ONLY        = 1 << 0, // host may not write it
	ELK_PROPERTY_AUDIO_THREAD_NOTIFY = 1 << 1  // also notify on the audio
	                                           // thread, before process()
};

/**
 * elk controller extension - extends v3_edit_controller
 */

struct elk_controller_extension {
#ifndef __cplusplus
	struct v3_funknown;
#endif
	int32_t (V3_API* get_property_count)(void* self);
	v3_result (V3_API* get_property_info)(void* self, int32_t property_idx, struct elk_property_info* info);
	v3_result (V3_API* get_property_value)(void* self, int32_t property_id, struct elk_property_value* value);
	v3_result (V3_API* set_property_value)(void* self, int32_t property_id, const struct elk_property_value* value);
};

static constexpr const v3_tuid elk_controller_extension_iid =
	V3_ID(0x1016CCA4, 0x930B4F58, 0x83918C4F, 0x8C4F99AB);

/**
 * elk component handler extension - extends v3_component_handler
 */

struct elk_component_handler_extension {
#ifndef __cplusplus
	struct v3_funknown;
#endif
	v3_result (V3_API* notify_property_value_change)(void* self, int32_t property_id, const struct elk_property_value* value);
};

static constexpr const v3_tuid elk_component_handler_extension_iid =
	V3_ID(0x83952AFF, 0x52844C67, 0xAC127387, 0x196C5DD4);

#ifdef __cplusplus

/**
 * C++ variants
 */

struct elk_controller_extension_cpp : v3_funknown {
	elk_controller_extension ext;
};

struct elk_component_handler_extension_cpp : v3_funknown {
	elk_component_handler_extension ext;
};

// No v3_cpp_obj specialisation needed for either: both sit directly behind
// v3_funknown, which is exactly what the generic template in base.h assumes.

#endif
