/*****************************************************************************
 * system include files
 ****************************************************************************/

#include <assert.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if KMP_OS_UNIX
#include <execinfo.h>
#include <dlfcn.h>
#endif

/*****************************************************************************
 * ompt include files
 ****************************************************************************/

#include "ompt-specific.cpp"

/*****************************************************************************
 * macros
 ****************************************************************************/

#define ompt_get_callback_success 1
#define ompt_get_callback_failure 0

#define no_tool_present 0

#define OMPT_API_ROUTINE static

#ifndef OMPT_STR_MATCH
#define OMPT_STR_MATCH(haystack, needle) (!strcasecmp(haystack, needle))
#endif

/*****************************************************************************
 * types
 ****************************************************************************/

typedef struct {
  const char *state_name;
  omp_state_t state_id;
} omp_state_info_t;

typedef struct {
  const char *name;
  ompt_mutex_impl_t id;
} ompt_mutex_impl_info_t;

enum tool_setting_e {
  omp_tool_error,
  omp_tool_unset,
  omp_tool_disabled,
  omp_tool_enabled
};

typedef int (*ompt_initialize_t)(ompt_function_lookup_t lookup,
                                 struct ompt_fns_t *fns);

typedef void (*ompt_finalize_t)(struct ompt_fns_t *fns);

/*****************************************************************************
 * global variables
 ****************************************************************************/

ompt_callbacks_active_t ompt_enabled;

omp_state_info_t omp_state_info[] = {
#define omp_state_macro(state, code) {#state, state},
    FOREACH_OMP_STATE(omp_state_macro)
#undef omp_state_macro
};

ompt_mutex_impl_info_t ompt_mutex_impl_info[] = {
#define ompt_mutex_impl_macro(name, id) {#name, name},
    FOREACH_OMPT_MUTEX_IMPL(ompt_mutex_impl_macro)
#undef ompt_mutex_impl_macro
};

ompt_callbacks_internal_t ompt_callbacks;

static ompt_fns_t *ompt_fns = NULL;

/*****************************************************************************
 * forward declarations
 ****************************************************************************/

static ompt_interface_fn_t ompt_fn_lookup(const char *s);

OMPT_API_ROUTINE ompt_data_t *ompt_get_thread_data(void);

/*****************************************************************************
 * initialization and finalization (private operations)
 ****************************************************************************/

/* On Unix-like systems that support weak symbols the following implementation
 * of ompt_start_tool() will be used in case no tool-supplied implementation of
 * this function is present in the address space of a process.
 *
 * On Windows, the ompt_tool_windows function is used to find the
 * ompt_tool symbol across all modules loaded by a process. If ompt_tool is
 * found, ompt_tool's return value is used to initialize the tool. Otherwise,
 * NULL is returned and OMPT won't be enabled */

typedef ompt_fns_t *(*ompt_start_tool_t)(unsigned int, const char*);

#if KMP_OS_UNIX

#if OMPT_HAVE_WEAK_ATTRIBUTE
_OMP_EXTERN __attribute__((weak))
#elif defined KMP_DYNAMIC_LIB
_OMP_EXTERN
#warning Activation of OMPT is might fail for tools statically linked into the application.
#else
#error Activation of OMPT is not supported on this platform.
#endif
ompt_fns_t *ompt_start_tool(unsigned int omp_version,
                            const char *runtime_version) {
#ifdef KMP_DYNAMIC_LIB
  ompt_fns_t *ret = NULL;
  // Try next symbol in the address space
  ompt_start_tool_t next_tool = NULL;
  *(void **)(&next_tool) = dlsym(RTLD_NEXT, "ompt_start_tool");
  if (next_tool)
    ret = (next_tool)(omp_version, runtime_version);
  return ret;
#else
#if OMPT_DEBUG
  printf("ompt_start_tool() is called from the RTL\n");
#endif
  return NULL;
#endif
}

#elif OMPT_HAVE_PSAPI

#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#define ompt_start_tool ompt_tool_windows

// The number of loaded modules to start enumeration with EnumProcessModules()
#define NUM_MODULES 128

static ompt_fns_t *ompt_tool_windows(unsigned int omp_version,
                                     const char *runtime_version) {
  int i;
  DWORD needed, new_size;
  HMODULE *modules;
  HANDLE process = GetCurrentProcess();
  modules = (HMODULE *)malloc(NUM_MODULES * sizeof(HMODULE));
  ompt_start_tool_t ompt_tool_p = NULL;

#if OMPT_DEBUG
  printf("ompt_tool_windows(): looking for ompt_start_tool\n");
#endif
  if (!EnumProcessModules(process, modules, NUM_MODULES * sizeof(HMODULE),
                          &needed)) {
    // Regardless of the error reason use the stub initialization function
    free(modules);
    return NULL;
  }
  // Check if NUM_MODULES is enough to list all modules
  new_size = needed / sizeof(HMODULE);
  if (new_size > NUM_MODULES) {
#if OMPT_DEBUG
    printf("ompt_tool_windows(): resize buffer to %d bytes\n", needed);
#endif
    modules = (HMODULE *)realloc(modules, needed);
    // If resizing failed use the stub function.
    if (!EnumProcessModules(process, modules, needed, &needed)) {
      free(modules);
      return NULL;
    }
  }
  for (i = 0; i < new_size; ++i) {
    (FARPROC &)ompt_tool_p = GetProcAddress(modules[i], "ompt_start_tool");
    if (ompt_tool_p) {
#if OMPT_DEBUG
      TCHAR modName[MAX_PATH];
      if (GetModuleFileName(modules[i], modName, MAX_PATH))
        printf("ompt_tool_windows(): ompt_start_tool found in module %s\n",
               modName);
#endif
      free(modules);
      return (*ompt_tool_p)(omp_version, runtime_version);
    }
#if OMPT_DEBUG
    else {
      TCHAR modName[MAX_PATH];
      if (GetModuleFileName(modules[i], modName, MAX_PATH))
        printf("ompt_tool_windows(): ompt_start_tool not found in module %s\n",
               modName);
    }
#endif
  }
  free(modules);
  return NULL;
}
#else
#error Either __attribute__((weak)) or psapi.dll are required for OMPT support
#endif // OMPT_HAVE_WEAK_ATTRIBUTE

static ompt_fns_t *ompt_try_start_tool(unsigned int omp_version,
                                       const char *runtime_version) {
  ompt_fns_t *ret = NULL;
  ompt_start_tool_t start_tool = NULL;
#if KMP_OS_WINDOWS
  // Cannot use colon to describe a list of absolute paths on Windows
  const char *sep = ";";
#else
  const char *sep = ":";
#endif

  // Try in the current address space
  if ((ret = ompt_start_tool(omp_version, runtime_version)))
    return ret;

  // Try tool-libraries-var ICV
  const char *tool_libs = getenv("OMP_TOOL_LIBRARIES");
  if (tool_libs) {
    const char *libs = __kmp_str_format("%s", tool_libs);
    char *buf;
    char *fname = __kmp_str_token(CCAST(char *, libs), sep, &buf);
    while (fname) {
#if KMP_OS_UNIX
      void *h = dlopen(fname, RTLD_LAZY);
      if (h) {
        *(void **)(&start_tool) = dlsym(h, "ompt_start_tool");
#elif KMP_OS_WINDOWS
      HMODULE h = LoadLibrary(fname);
      if (h) {
        start_tool = (ompt_start_tool_t)GetProcAddress(h, "ompt_start_tool");
#else
#error Activation of OMPT is not supported on this platform.
#endif
        if (start_tool && (ret = (*start_tool)(omp_version, runtime_version)))
          break;
      }
      fname = __kmp_str_token(NULL, sep, &buf);
    }
    __kmp_str_free(&libs);
  }
  return ret;
}

void ompt_pre_init() {
  //--------------------------------------------------
  // Execute the pre-initialization logic only once.
  //--------------------------------------------------
  static int ompt_pre_initialized = 0;

  if (ompt_pre_initialized)
    return;

  ompt_pre_initialized = 1;

  //--------------------------------------------------
  // Use a tool iff a tool is enabled and available.
  //--------------------------------------------------
  const char *ompt_env_var = getenv("OMP_TOOL");
  tool_setting_e tool_setting = omp_tool_error;

  if (!ompt_env_var || !strcmp(ompt_env_var, ""))
    tool_setting = omp_tool_unset;
  else if (OMPT_STR_MATCH(ompt_env_var, "disabled"))
    tool_setting = omp_tool_disabled;
  else if (OMPT_STR_MATCH(ompt_env_var, "enabled"))
    tool_setting = omp_tool_enabled;

#if OMPT_DEBUG
  printf("ompt_pre_init(): tool_setting = %d\n", tool_setting);
#endif
  switch (tool_setting) {
  case omp_tool_disabled:
    break;

  case omp_tool_unset:
  case omp_tool_enabled:

    //--------------------------------------------------
    // Load tool iff specified in environment variable
    //--------------------------------------------------
    ompt_fns =
        ompt_try_start_tool(__kmp_openmp_version, ompt_get_runtime_version());

    memset(&ompt_enabled, 0, sizeof(ompt_enabled));
    break;

  case omp_tool_error:
    fprintf(stderr, "Warning: OMP_TOOL has invalid value \"%s\".\n"
                    "  legal values are (NULL,\"\",\"disabled\","
                    "\"enabled\").\n",
            ompt_env_var);
    break;
  }
#if OMPT_DEBUG
  printf("ompt_pre_init(): ompt_enabled = %d\n", ompt_enabled);
#endif
}

void ompt_post_init() {
  //--------------------------------------------------
  // Execute the post-initialization logic only once.
  //--------------------------------------------------
  static int ompt_post_initialized = 0;

  if (ompt_post_initialized)
    return;

  ompt_post_initialized = 1;

  //--------------------------------------------------
  // Initialize the tool if so indicated.
  //--------------------------------------------------
  if (ompt_fns) {
    ompt_enabled.enabled = !!ompt_fns->initialize(ompt_fn_lookup, ompt_fns);

    ompt_thread_t *root_thread = ompt_get_thread();

    ompt_set_thread_state(root_thread, omp_state_overhead);

    if (ompt_enabled.ompt_callback_thread_begin) {
      ompt_callbacks.ompt_callback(ompt_callback_thread_begin)(
          ompt_thread_initial, __ompt_get_thread_data_internal());
    }
    ompt_data_t *task_data;
    __ompt_get_task_info_internal(0, NULL, &task_data, NULL, NULL, NULL);
    if (ompt_enabled.ompt_callback_task_create) {
      ompt_callbacks.ompt_callback(ompt_callback_task_create)(
          NULL, NULL, task_data, ompt_task_initial, 0,
          NULL);
    }

    ompt_set_thread_state(root_thread, omp_state_work_serial);
  }
}

void ompt_fini() {
  if (ompt_enabled.enabled) {
    ompt_fns->finalize(ompt_fns);
  }

  memset(&ompt_enabled, 0, sizeof(ompt_enabled));
}

/*****************************************************************************
 * interface operations
 ****************************************************************************/

/*****************************************************************************
 * state
 ****************************************************************************/

OMPT_API_ROUTINE int ompt_enumerate_states(int current_state, int *next_state,
                                           const char **next_state_name) {
  const static int len = sizeof(omp_state_info) / sizeof(omp_state_info_t);
  int i = 0;

  for (i = 0; i < len - 1; i++) {
    if (omp_state_info[i].state_id == current_state) {
      *next_state = omp_state_info[i + 1].state_id;
      *next_state_name = omp_state_info[i + 1].state_name;
      return 1;
    }
  }

  return 0;
}

OMPT_API_ROUTINE int ompt_enumerate_mutex_impls(int current_impl,
                                                int *next_impl,
                                                const char **next_impl_name) {
  const static int len =
      sizeof(ompt_mutex_impl_info) / sizeof(ompt_mutex_impl_info_t);
  int i = 0;
  for (i = 0; i < len - 1; i++) {
    if (ompt_mutex_impl_info[i].id != current_impl)
      continue;
    *next_impl = ompt_mutex_impl_info[i + 1].id;
    *next_impl_name = ompt_mutex_impl_info[i + 1].name;
    return 1;
  }
  return 0;
}

/*****************************************************************************
 * callbacks
 ****************************************************************************/

OMPT_API_ROUTINE int ompt_set_callback(ompt_callbacks_t which,
                                       ompt_callback_t callback) {
  switch (which) {

#define ompt_event_macro(event_name, callback_type, event_id)                  \
  case event_name:                                                             \
    if (ompt_event_implementation_status(event_name)) {                        \
      ompt_callbacks.ompt_callback(event_name) = (callback_type)callback;      \
      ompt_enabled.event_name = 1;                                             \
    }                                                                          \
    return ompt_event_implementation_status(event_name);

    FOREACH_OMPT_EVENT(ompt_event_macro)

#undef ompt_event_macro

  default:
    return ompt_set_error;
  }
}

OMPT_API_ROUTINE int ompt_get_callback(ompt_callbacks_t which,
                                       ompt_callback_t *callback) {
  switch (which) {

#define ompt_event_macro(event_name, callback_type, event_id)                  \
  case event_name:                                                             \
    if (ompt_event_implementation_status(event_name)) {                        \
      ompt_callback_t mycb =                                                   \
          (ompt_callback_t)ompt_callbacks.ompt_callback(event_name);           \
      if (mycb) {                                                              \
        *callback = mycb;                                                      \
        return ompt_get_callback_success;                                      \
      }                                                                        \
    }                                                                          \
    return ompt_get_callback_failure;

    FOREACH_OMPT_EVENT(ompt_event_macro)

#undef ompt_event_macro

  default:
    return ompt_get_callback_failure;
  }
}

/*****************************************************************************
 * parallel regions
 ****************************************************************************/

OMPT_API_ROUTINE int ompt_get_parallel_info(int ancestor_level,
                                            ompt_data_t **parallel_data,
                                            int *team_size) {
  return __ompt_get_parallel_info_internal(ancestor_level, parallel_data,
                                           team_size);
}

OMPT_API_ROUTINE omp_state_t ompt_get_state(ompt_wait_id_t *wait_id) {
  omp_state_t thread_state = __ompt_get_state_internal(wait_id);

  if (thread_state == omp_state_undefined) {
    thread_state = omp_state_work_serial;
  }

  return thread_state;
}

/*****************************************************************************
 * tasks
 ****************************************************************************/

OMPT_API_ROUTINE ompt_data_t *ompt_get_thread_data(void) {
  return __ompt_get_thread_data_internal();
}

OMPT_API_ROUTINE int ompt_get_task_info(int ancestor_level, int *type,
                                        ompt_data_t **task_data,
                                        ompt_frame_t **task_frame,
                                        ompt_data_t **parallel_data,
                                        int *thread_num) {
  return __ompt_get_task_info_internal(ancestor_level, type, task_data,
                                       task_frame, parallel_data, thread_num);
}

/*****************************************************************************
 * places
 ****************************************************************************/

OMPT_API_ROUTINE int ompt_get_num_places(void) {
// copied from kmp_ftn_entry.h (but modified)
#if !KMP_AFFINITY_SUPPORTED
  return 0;
#else
  if (!KMP_AFFINITY_CAPABLE())
    return 0;
  return __kmp_affinity_num_masks;
#endif
}

OMPT_API_ROUTINE int ompt_get_place_proc_ids(int place_num, int ids_size,
                                             int *ids) {
// copied from kmp_ftn_entry.h (but modified)
#if !KMP_AFFINITY_SUPPORTED
  return 0;
#else
  int i, count;
  int tmp_ids[ids_size];
  if (!KMP_AFFINITY_CAPABLE())
    return 0;
  if (place_num < 0 || place_num >= (int)__kmp_affinity_num_masks)
    return 0;
  /* TODO: Is this safe for asynchronous call from signal handler during runtime shutdown? */
  kmp_affin_mask_t *mask = KMP_CPU_INDEX(__kmp_affinity_masks, place_num);
  count = 0;
  KMP_CPU_SET_ITERATE(i, mask) {
    if ((!KMP_CPU_ISSET(i, __kmp_affin_fullMask)) ||
        (!KMP_CPU_ISSET(i, mask))) {
      continue;
    }
    if (count < ids_size)
      tmp_ids[count] = i;
    count++;
  }
  if (ids_size >= count) {
    for (i = 0; i < count; i++) {
      ids[i] = tmp_ids[i];
    }
  }
  return count;
#endif
}

OMPT_API_ROUTINE int ompt_get_place_num(void) {
// copied from kmp_ftn_entry.h (but modified)
#if !KMP_AFFINITY_SUPPORTED
  return -1;
#else
  int gtid;
  kmp_info_t *thread;
  if (!KMP_AFFINITY_CAPABLE())
    return -1;
  gtid = __kmp_entry_gtid();
  thread = __kmp_thread_from_gtid(gtid);
  if(thread == NULL || thread->th.th_current_place < 0)
    return -1;
  return thread->th.th_current_place;
#endif
}

OMPT_API_ROUTINE int ompt_get_partition_place_nums(int place_nums_size,
                                                   int *place_nums) {
// copied from kmp_ftn_entry.h (but modified)
#if !KMP_AFFINITY_SUPPORTED
  return 0;
#else
  int i, gtid, place_num, first_place, last_place, start, end;
  kmp_info_t *thread;
  if (!KMP_AFFINITY_CAPABLE())
    return 0;
  gtid = __kmp_entry_gtid();
  thread = __kmp_thread_from_gtid(gtid);
  if(thread==NULL)
    return 0;
  first_place = thread->th.th_first_place;
  last_place = thread->th.th_last_place;
  if (first_place < 0 || last_place < 0)
    return 0;
  if (first_place <= last_place) {
    start = first_place;
    end = last_place;
  } else {
    start = last_place;
    end = first_place;
  }
  if (end - start <= place_nums_size)
    for (i = 0, place_num = start; place_num <= end; ++place_num, ++i) {
      place_nums[i] = place_num;
    }
  return end - start;
#endif
}

/*****************************************************************************
 * places
 ****************************************************************************/

OMPT_API_ROUTINE int ompt_get_proc_id(void) {
#if KMP_OS_LINUX
  return sched_getcpu();
#else
  return -1;
#endif
}

/*****************************************************************************
 * placeholders
 ****************************************************************************/

// Don't define this as static. The loader may choose to eliminate the symbol
// even though it is needed by tools.
#define OMPT_API_PLACEHOLDER

// Ensure that placeholders don't have mangled names in the symbol table.
#ifdef __cplusplus
extern "C" {
#endif

OMPT_API_PLACEHOLDER void ompt_idle(void) {
  // This function is a placeholder used to represent the calling context of
  // idle OpenMP worker threads. It is not meant to be invoked.
  assert(0);
}

OMPT_API_PLACEHOLDER void ompt_overhead(void) {
  // This function is a placeholder used to represent the OpenMP context of
  // threads working in the OpenMP runtime.  It is not meant to be invoked.
  assert(0);
}

OMPT_API_PLACEHOLDER void ompt_barrier_wait(void) {
  // This function is a placeholder used to represent the OpenMP context of
  // threads waiting for a barrier in the OpenMP runtime. It is not meant
  // to be invoked.
  assert(0);
}

OMPT_API_PLACEHOLDER void ompt_task_wait(void) {
  // This function is a placeholder used to represent the OpenMP context of
  // threads waiting for a task in the OpenMP runtime. It is not meant
  // to be invoked.
  assert(0);
}

OMPT_API_PLACEHOLDER void ompt_mutex_wait(void) {
  // This function is a placeholder used to represent the OpenMP context of
  // threads waiting for a mutex in the OpenMP runtime. It is not meant
  // to be invoked.
  assert(0);
}

#ifdef __cplusplus
};
#endif

/*****************************************************************************
 * compatability
 ****************************************************************************/

OMPT_API_ROUTINE int ompt_get_ompt_version() { return OMPT_VERSION; }

/*****************************************************************************
* application-facing API
 ****************************************************************************/

/*----------------------------------------------------------------------------
 | control
 ---------------------------------------------------------------------------*/

int __kmp_control_tool(uint64_t command, uint64_t modifier, void *arg) {

  if (ompt_enabled.enabled) {
    if (ompt_enabled.ompt_callback_control_tool) {
      return ompt_callbacks.ompt_callback(ompt_callback_control_tool)(
          command, modifier, arg, OMPT_LOAD_RETURN_ADDRESS(__kmp_entry_gtid()));
    } else {
      return -1;
    }
  } else {
    return -2;
  }
}

/*****************************************************************************
 * misc
 ****************************************************************************/

OMPT_API_ROUTINE uint64_t ompt_get_unique_id(void) {
  return __ompt_get_unique_id_internal();
}

/*****************************************************************************
 * Target
 ****************************************************************************/

OMPT_API_ROUTINE int ompt_get_target_info(
    uint64_t *device_num,
    ompt_id_t *target_id,
    ompt_id_t *host_op_id)
{
  return 0; //thread is not in a target region
}

OMPT_API_ROUTINE int ompt_get_num_devices(void)
{
  return 1; //only one device (the current device) is available
}

/*****************************************************************************
 * API inquiry for tool
 ****************************************************************************/

static ompt_interface_fn_t ompt_fn_lookup(const char *s) {

#define ompt_interface_fn(fn)                                                  \
  fn##_t fn##_f = fn;                                                          \
  if (strcmp(s, #fn) == 0)                                                     \
    return (ompt_interface_fn_t)fn##_f;

  FOREACH_OMPT_INQUIRY_FN(ompt_interface_fn)

  FOREACH_OMPT_PLACEHOLDER_FN(ompt_interface_fn)

  return (ompt_interface_fn_t)0;
}