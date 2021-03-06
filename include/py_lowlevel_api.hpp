//
// Copyright (C) 2011-14 Mark Wiebe, DyND Developers
// BSD 2-Clause License, see LICENSE.txt
//

#ifndef _DYND__PY_LOWLEVEL_API_HPP_
#define _DYND__PY_LOWLEVEL_API_HPP_

#include <dynd/lowlevel_api.hpp>

#include "array_functions.hpp"
#include "type_functions.hpp"

namespace pydynd {

/**
 * This struct contains a bunch of function which provide
 * low level C-level access to the innards of dynd's python
 * exposure.
 *
 * These functions are static and should not be modified
 * after initialization.
 */
struct py_lowlevel_api_t {
    uintptr_t version;
    // Extracts the dynd object pointers from their Python wrappers.
    // These functions do not check the type of the arguments.
    dynd::array_preamble *(*get_array_ptr)(WArray *obj);
    const dynd::base_type *(*get_base_type_ptr)(WType *obj);
    PyObject *(*array_from_ptr)(PyObject *dt, PyObject *ptr, PyObject *owner, PyObject *access);
    PyObject *(*make_assignment_ckernel)(void *out_ckb, intptr_t ckb_offset,
                                         PyObject *dst_tp_obj,
                                         const void *dst_metadata,
                                         PyObject *src_tp_obj,
                                         const void *src_metadata,
                                         PyObject *funcproto,
                                         PyObject *kerntype, PyObject *ectx);
    PyObject *(*make_ckernel_deferred_from_assignment)(PyObject *dst_tp_obj, PyObject *src_tp_obj,
                    PyObject *funcproto, PyObject *errmode);
    PyObject *(*make_ckernel_deferred_from_property)(PyObject *tp_obj, PyObject *propname,
                    PyObject *funcproto, PyObject *errmode);
    PyObject *(*numpy_typetuples_from_ufunc)(PyObject *ufunc);
    PyObject *(*ckernel_deferred_from_ufunc)(PyObject *ufunc,
                    PyObject *type_tuple, int ckernel_acquires_gil);
    PyObject *(*lift_ckernel_deferred)(PyObject *ckd, PyObject *types);
    PyObject *(*lift_reduction_ckernel_deferred)(PyObject *elwise_reduction, PyObject *lifted_type,
                    PyObject *dst_initialization, PyObject *axis, PyObject *keepdims,
                    PyObject *associative, PyObject *commutative,
                    PyObject *right_associative, PyObject *reduction_identity);
    PyObject *(*ckernel_deferred_from_pyfunc)(PyObject *instantiate_pyfunc, PyObject *types);
};

} // namespace pydynd

/**
 * Returns a pointer to the static low level API structure.
 */
extern "C" const void *dynd_get_py_lowlevel_api();

#endif // _DYND__PY_LOWLEVEL_API_HPP_
