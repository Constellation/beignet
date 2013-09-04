/* 
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Benjamin Segovia <benjamin.segovia@intel.com>
 */

#ifndef __CL_DRIVER_H__
#define __CL_DRIVER_H__

#include <stdint.h>
#include <stdlib.h>

/* Various limitations we should remove actually */
#define GEN_MAX_SURFACES 128
#define GEN_MAX_SAMPLERS 16

/**************************************************************************
 * cl_driver:
 * Hide behind some call backs the buffer allocation / deallocation ... This
 * will allow us to make the use of a software performance simulator easier and
 * to minimize the code specific for the HW and for the simulator
 **************************************************************************/

/* Encapsulates command buffer / data buffer / kernels */
typedef struct _cl_buffer *cl_buffer;

/* Encapsulates buffer manager */
typedef struct _cl_buffer_mgr *cl_buffer_mgr;

/* Encapsulates the driver backend functionalities */
typedef struct _cl_driver *cl_driver;

/* Encapsulates the gpgpu stream of commands */
typedef struct _cl_gpgpu *cl_gpgpu;

/* Encapsulates the event  of a command stream */
typedef struct _cl_gpgpu_event *cl_gpgpu_event;

typedef struct _cl_context_prop *cl_context_prop;
typedef struct _cl_sampler *cl_sampler;

/**************************************************************************
 * Driver
 **************************************************************************/
/* Create a new driver */
typedef cl_driver (cl_driver_new_cb)(cl_context_prop);
extern cl_driver_new_cb *cl_driver_new;

/* Delete the driver */
typedef void (cl_driver_delete_cb)(cl_driver);
extern cl_driver_delete_cb *cl_driver_delete;

/* Get the buffer manager from the driver */
typedef cl_buffer_mgr (cl_driver_get_bufmgr_cb)(cl_driver);
extern cl_driver_get_bufmgr_cb *cl_driver_get_bufmgr;

/* Get the Gen version from the driver */
typedef uint32_t (cl_driver_get_ver_cb)(cl_driver);
extern cl_driver_get_ver_cb *cl_driver_get_ver;

/**************************************************************************
 * GPGPU command streamer
 **************************************************************************/
/* Describe texture tiling */
typedef enum cl_gpgpu_tiling {
  GPGPU_NO_TILE = 0,
  GPGPU_TILE_X  = 1,
  GPGPU_TILE_Y  = 2,
} cl_gpgpu_tiling;

/* Cache control options */
typedef enum cl_cache_control {
  cc_gtt      = 0x0,
  cc_l3       = 0x1,
  cc_llc      = 0x2,
  cc_llc_l3   = 0x3
} cl_cache_control;

typedef enum gpu_command_status {
  command_queued    = 3,
  command_submitted = 2,
  command_running   = 1,
  command_complete  = 0
} gpu_command_status;

/* Use this structure to bind kernels in the gpgpu state */
typedef struct cl_gpgpu_kernel {
  const char *name;        /* kernel name and bo name */
  uint32_t grf_blocks;     /* register blocks kernel wants (in 8 reg blocks) */
  uint32_t curbe_sz;         /* total size of all curbes */
  cl_buffer bo;            /* kernel code in the proper addr space */
  int32_t barrierID;       /* barrierID for _this_ kernel */
  uint32_t use_slm:1;      /* For gen7 (automatic barrier management) */
  uint32_t thread_n:15;    /* For gen7 (automatic barrier management) */
  uint32_t slm_sz;         /* For gen7 (automatic SLM allocation) */
} cl_gpgpu_kernel;

/* Create a new gpgpu state */
typedef cl_gpgpu (cl_gpgpu_new_cb)(cl_driver);
extern cl_gpgpu_new_cb *cl_gpgpu_new;

/* Delete the gpgpu state */
typedef void (cl_gpgpu_delete_cb)(cl_gpgpu);
extern cl_gpgpu_delete_cb *cl_gpgpu_delete;

/* Synchonize GPU with CPU */
typedef cl_gpgpu (cl_gpgpu_sync_cb)(cl_gpgpu);
extern cl_gpgpu_sync_cb *cl_gpgpu_sync;

/* Bind a regular unformatted buffer */
typedef void (cl_gpgpu_bind_buf_cb)(cl_gpgpu, cl_buffer, uint32_t offset, uint32_t cchint);
extern cl_gpgpu_bind_buf_cb *cl_gpgpu_bind_buf;

/* bind samplers defined in both kernel and kernel args. */
typedef void (cl_gpgpu_bind_sampler_cb)(cl_gpgpu, uint32_t *samplers, size_t sampler_sz);
extern cl_gpgpu_bind_sampler_cb *cl_gpgpu_bind_sampler;

/* Set a 2d texture */
typedef void (cl_gpgpu_bind_image_cb)(cl_gpgpu state,
                                      uint32_t id,
                                      cl_buffer obj_bo,
                                      uint32_t format,
                                      uint32_t type,
                                      int32_t w,
                                      int32_t h,
                                      int pitch,
                                      cl_gpgpu_tiling tiling);

extern cl_gpgpu_bind_image_cb *cl_gpgpu_bind_image;

/* Setup a stack */
typedef void (cl_gpgpu_set_stack_cb)(cl_gpgpu, uint32_t offset, uint32_t size, uint32_t cchint);
extern cl_gpgpu_set_stack_cb *cl_gpgpu_set_stack;

/* Setup scratch */
typedef void (cl_gpgpu_set_scratch_cb)(cl_gpgpu, uint32_t per_thread_size);
extern cl_gpgpu_set_scratch_cb *cl_gpgpu_set_scratch;

/* Configure internal state */
typedef void (cl_gpgpu_state_init_cb)(cl_gpgpu, uint32_t max_threads, uint32_t size_cs_entry);
extern cl_gpgpu_state_init_cb *cl_gpgpu_state_init;

/* Set the buffer object where to report performance counters */
typedef void (cl_gpgpu_set_perf_counters_cb)(cl_gpgpu, cl_buffer perf);
extern cl_gpgpu_set_perf_counters_cb *cl_gpgpu_set_perf_counters;

/* Fills current curbe buffer with data */
typedef void (cl_gpgpu_upload_curbes_cb)(cl_gpgpu, const void* data, uint32_t size);
extern cl_gpgpu_upload_curbes_cb *cl_gpgpu_upload_curbes;

typedef cl_buffer (cl_gpgpu_alloc_constant_buffer_cb)(cl_gpgpu, uint32_t size);
extern cl_gpgpu_alloc_constant_buffer_cb *cl_gpgpu_alloc_constant_buffer;

/* Setup all indirect states */
typedef void (cl_gpgpu_states_setup_cb)(cl_gpgpu, cl_gpgpu_kernel *kernel);
extern cl_gpgpu_states_setup_cb *cl_gpgpu_states_setup;

/* Upload the constant samplers as specified inside the OCL kernel */
typedef void (cl_gpgpu_upload_samplers_cb)(cl_gpgpu *state, const void *data, uint32_t n);
extern cl_gpgpu_upload_samplers_cb *cl_gpgpu_upload_samplers;

/* Set a sampler */
typedef void (cl_gpgpu_set_sampler_cb)(cl_gpgpu, uint32_t index, uint32_t non_normalized);
extern cl_gpgpu_set_sampler_cb *cl_gpgpu_set_sampler;

/* Allocate the batch buffer and return the BO used for the batch buffer */
typedef void (cl_gpgpu_batch_reset_cb)(cl_gpgpu, size_t sz);
extern cl_gpgpu_batch_reset_cb *cl_gpgpu_batch_reset;

/* Atomic begin, pipeline select, urb, pipeline state and constant buffer */
typedef void (cl_gpgpu_batch_start_cb)(cl_gpgpu);
extern cl_gpgpu_batch_start_cb *cl_gpgpu_batch_start;

/* atomic end with possibly inserted flush */
typedef void (cl_gpgpu_batch_end_cb)(cl_gpgpu, int32_t flush_mode);
extern cl_gpgpu_batch_end_cb *cl_gpgpu_batch_end;

/* Flush the command buffer */
typedef void (cl_gpgpu_flush_cb)(cl_gpgpu);
extern cl_gpgpu_flush_cb *cl_gpgpu_flush;

/* new a event for a batch buffer */
typedef cl_gpgpu_event (cl_gpgpu_event_new_cb)(cl_gpgpu);
extern cl_gpgpu_event_new_cb *cl_gpgpu_event_new;

/* new a event for a batch buffer */
typedef int (cl_gpgpu_event_update_status_cb)(cl_gpgpu_event, int);
extern cl_gpgpu_event_update_status_cb *cl_gpgpu_event_update_status;

/* new a event for a batch buffer */
typedef void (cl_gpgpu_event_pending_cb)(cl_gpgpu, cl_gpgpu_event);
extern cl_gpgpu_event_pending_cb *cl_gpgpu_event_pending;

/* new a event for a batch buffer */
typedef void (cl_gpgpu_event_resume_cb)(cl_gpgpu_event);
extern cl_gpgpu_event_resume_cb *cl_gpgpu_event_resume;

/* new a event for a batch buffer */
typedef void (cl_gpgpu_event_delete_cb)(cl_gpgpu_event);
extern cl_gpgpu_event_delete_cb *cl_gpgpu_event_delete;


/* Will spawn all threads */
typedef void (cl_gpgpu_walker_cb)(cl_gpgpu,
                                  uint32_t simd_sz,
                                  uint32_t thread_n,
                                  const size_t global_wk_off[3],
                                  const size_t global_wk_sz[3],
                                  const size_t local_wk_sz[3]);
extern cl_gpgpu_walker_cb *cl_gpgpu_walker;

/**************************************************************************
 * Buffer
 **************************************************************************/
/* Allocate a buffer */
typedef cl_buffer (cl_buffer_alloc_cb)(cl_buffer_mgr, const char*, size_t, size_t);
extern cl_buffer_alloc_cb *cl_buffer_alloc;

/* Set a buffer's tiling mode */
typedef cl_buffer (cl_buffer_set_tiling_cb)(cl_buffer, int tiling, size_t stride);
extern cl_buffer_set_tiling_cb *cl_buffer_set_tiling;

#include "cl_context.h"
typedef struct _cl_context *cl_context;

typedef cl_buffer (cl_buffer_alloc_from_eglimage_cb)(cl_context, void*, unsigned int *,
                                                     int *, int *, int *, int *);
extern cl_buffer_alloc_from_eglimage_cb *cl_buffer_alloc_from_eglimage;

/* Unref a buffer and destroy it if no more ref */
typedef void (cl_buffer_unreference_cb)(cl_buffer);
extern cl_buffer_unreference_cb *cl_buffer_unreference;

/* Add one more ref on a buffer */
typedef void (cl_buffer_reference_cb)(cl_buffer);
extern cl_buffer_reference_cb *cl_buffer_reference;

/* Map a buffer */
typedef int (cl_buffer_map_cb)(cl_buffer, uint32_t write_enable);
extern cl_buffer_map_cb *cl_buffer_map;

/* Unmap a buffer */
typedef int (cl_buffer_unmap_cb)(cl_buffer);
extern cl_buffer_unmap_cb *cl_buffer_unmap;

/* Map a buffer in the GTT domain */
typedef int (cl_buffer_map_gtt_cb)(cl_buffer);
extern cl_buffer_map_gtt_cb *cl_buffer_map_gtt;

/* Map a buffer in the GTT domain, non waiting the GPU read or write*/
typedef int (cl_buffer_map_gtt_unsync_cb)(cl_buffer);
extern cl_buffer_map_gtt_unsync_cb *cl_buffer_map_gtt_unsync;

/* Unmap a buffer in the GTT domain */
typedef int (cl_buffer_unmap_gtt_cb)(cl_buffer);
extern cl_buffer_unmap_gtt_cb *cl_buffer_unmap_gtt;

/* Get the virtual address (when mapped) */
typedef void* (cl_buffer_get_virtual_cb)(cl_buffer);
extern cl_buffer_get_virtual_cb *cl_buffer_get_virtual;

/* Get the size of the buffer */
typedef size_t (cl_buffer_get_size_cb)(cl_buffer);
extern cl_buffer_get_size_cb *cl_buffer_get_size;

/* Pin a buffer */
typedef int (cl_buffer_pin_cb)(cl_buffer, uint32_t alignment);
extern cl_buffer_pin_cb *cl_buffer_pin;

/* Unpin a buffer */
typedef int (cl_buffer_unpin_cb)(cl_buffer);
extern cl_buffer_unpin_cb *cl_buffer_unpin;

/* Fill data in the buffer */
typedef int (cl_buffer_subdata_cb)(cl_buffer, unsigned long, unsigned long, const void*);
extern cl_buffer_subdata_cb *cl_buffer_subdata;

/* Wait for all pending rendering for this buffer to complete */
typedef int (cl_buffer_wait_rendering_cb) (cl_buffer);
extern cl_buffer_wait_rendering_cb *cl_buffer_wait_rendering;

/* Get the device id */
typedef int (cl_driver_get_device_id_cb)(void);
extern cl_driver_get_device_id_cb *cl_driver_get_device_id;

#endif /* __CL_DRIVER_H__ */

