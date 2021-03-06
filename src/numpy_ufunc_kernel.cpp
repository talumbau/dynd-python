//
// Copyright (C) 2011-14 Mark Wiebe, DyND Developers
// BSD 2-Clause License, see LICENSE.txt
//


#include "numpy_ufunc_kernel.hpp"

#if defined(_MSC_VER)
#pragma warning(push,2)
#endif
#include <numpy/npy_3kcompat.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <dynd/kernels/expr_kernels.hpp>
#include <dynd/types/ckernel_deferred_type.hpp>

#include "exception_translation.hpp"
#include "utility_functions.hpp"
#include "array_functions.hpp"

#if DYND_NUMPY_INTEROP

using namespace std;
using namespace dynd;
using namespace pydynd;

PyObject *pydynd::numpy_typetuples_from_ufunc(PyObject *ufunc)
{
    // NOTE: This function does not raise C++ exceptions,
    //       it behaves as a Python C-API function.
    if (!PyObject_TypeCheck(ufunc, &PyUFunc_Type)) {
        stringstream ss;
        ss << "a numpy ufunc object is required to retrieve type tuples, ";
        pyobject_ownref repr_obj(PyObject_Repr(ufunc));
        ss << "got " << pystring_as_string(repr_obj.get());
        PyErr_SetString(PyExc_TypeError, ss.str().c_str());
        return NULL;
    }
    PyUFuncObject *uf = (PyUFuncObject *)ufunc;

    // Process the main ufunc loops list
    int builtin_count = uf->ntypes;
    int nargs = uf->nin + uf->nout;
    PyObject *result = PyList_New(builtin_count);
    if (result == NULL) {
        return NULL;
    }
    for (int i = 0; i < builtin_count; ++i) {
        PyObject *typetup = PyTuple_New(nargs);
        if (typetup == NULL) {
            Py_DECREF(result);
            return NULL;
        }
        char *types = uf->types + i * nargs;
        // Switch from numpy convention "in, out" to dynd convention "out, in"
        {
            PyObject *descr = (PyObject *)PyArray_DescrFromType(types[nargs-1]);
            if (descr == NULL) {
                Py_DECREF(result);
                Py_DECREF(typetup);
                return NULL;
            }
            PyTuple_SET_ITEM(typetup, 0, descr);
        }
        for (int j = 1; j < nargs; ++j) {
            PyObject *descr = (PyObject *)PyArray_DescrFromType(types[j-1]);
            if (descr == NULL) {
                Py_DECREF(result);
                Py_DECREF(typetup);
                return NULL;
            }
            PyTuple_SET_ITEM(typetup, j, descr);
        }
        PyList_SET_ITEM(result, i, typetup);
    }

    // Process the userloops as well
    if (uf->userloops != NULL) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(uf->userloops, &pos, &key, &value)) {
            PyUFunc_Loop1d *funcdata = (PyUFunc_Loop1d *)NpyCapsule_AsVoidPtr(value);
            while (funcdata != NULL) {
                PyObject *typetup = PyTuple_New(nargs);
                if (typetup == NULL) {
                    Py_DECREF(result);
                    return NULL;
                }
                int *types = funcdata->arg_types;
                // Switch from numpy convention "in, out" to dynd convention "out, in"
                {
                    PyObject *descr = (PyObject *)PyArray_DescrFromType(types[nargs-1]);
                    if (descr == NULL) {
                        Py_DECREF(result);
                        Py_DECREF(typetup);
                        return NULL;
                    }
                    PyTuple_SET_ITEM(typetup, 0, descr);
                }
                for (int j = 1; j < nargs; ++j) {
                    PyObject *descr = (PyObject *)PyArray_DescrFromType(types[j-1]);
                    if (descr == NULL) {
                        Py_DECREF(result);
                        Py_DECREF(typetup);
                        return NULL;
                    }
                    PyTuple_SET_ITEM(typetup, j, descr);
                }
                PyList_Append(result, typetup);

                funcdata = funcdata->next;
            }
        }
    }

    return result;
}

namespace {
    struct scalar_ufunc_deferred_data {
        PyUFuncObject *ufunc;
        PyUFuncGenericFunction funcptr;
        void *ufunc_data;
        int ckernel_acquires_gil;
        intptr_t data_types_size;
        const dynd::base_type *data_types[1];
    };

    static void delete_scalar_ufunc_deferred_data(void *self_data_ptr)
    {
        scalar_ufunc_deferred_data *data =
                        reinterpret_cast<scalar_ufunc_deferred_data *>(self_data_ptr);
        const dynd::base_type **data_types = &data->data_types[0];
        for (intptr_t i = 0; i < data->data_types_size; ++i) {
            base_type_xdecref(data_types[i]);
        }
        // Call the destructor and free the memory
        data->~scalar_ufunc_deferred_data();
        if (data->ufunc != NULL) {
            // Acquire the GIL for the python decref
            PyGILState_RAII pgs;
            Py_DECREF(data->ufunc);
        }
        free(data);
    }

    struct scalar_ufunc_ckernel_data {
        ckernel_prefix base;
        PyUFuncGenericFunction funcptr;
        void *ufunc_data;
        intptr_t data_types_size;
        PyUFuncObject *ufunc;
    };

    static void delete_scalar_ufunc_ckernel_data(ckernel_prefix *self_data_ptr)
    {
        scalar_ufunc_ckernel_data *data =
                        reinterpret_cast<scalar_ufunc_ckernel_data *>(self_data_ptr);
        if (data->ufunc != NULL) {
            // Acquire the GIL for the python decref
            PyGILState_RAII pgs;
            Py_DECREF(data->ufunc);
        }
    }

    static void scalar_ufunc_single_ckernel_acquiregil(
                    char *dst, const char * const *src,
                    ckernel_prefix *ckp)
    {
        scalar_ufunc_ckernel_data *data =
                        reinterpret_cast<scalar_ufunc_ckernel_data *>(ckp);
        char *args[NPY_MAXARGS];
        intptr_t data_types_size = data->data_types_size;
        // Set up the args array the way the numpy ufunc wants it
        memcpy(&args[0], &src[0], (data_types_size - 1) * sizeof(void *));
        args[data_types_size - 1] = dst;
        // Call the ufunc loop with a dim size of 1
        intptr_t dimsize = 1;
        intptr_t strides[NPY_MAXARGS];
        memset(strides, 0, data_types_size * sizeof(void *));
        {
            PyGILState_RAII pgs;
            data->funcptr(args, &dimsize, strides, data->ufunc_data);
        }
    }

    static void scalar_ufunc_single_ckernel_nogil(
                    char *dst, const char * const *src,
                    ckernel_prefix *ckp)
    {
        scalar_ufunc_ckernel_data *data =
                        reinterpret_cast<scalar_ufunc_ckernel_data *>(ckp);
        char *args[NPY_MAXARGS];
        intptr_t data_types_size = data->data_types_size;
        // Set up the args array the way the numpy ufunc wants it
        memcpy(&args[0], &src[0], (data_types_size - 1) * sizeof(void *));
        args[data_types_size - 1] = dst;
        // Call the ufunc loop with a dim size of 1
        intptr_t dimsize = 1;
        intptr_t strides[NPY_MAXARGS];
        memset(strides, 0, data_types_size * sizeof(intptr_t));
        data->funcptr(args, &dimsize, strides, data->ufunc_data);
    }

    static void scalar_ufunc_strided_ckernel_acquiregil(
                    char *dst, intptr_t dst_stride,
                    const char * const *src, const intptr_t *src_stride,
                    size_t count, ckernel_prefix *ckp)
    {
        scalar_ufunc_ckernel_data *data =
                        reinterpret_cast<scalar_ufunc_ckernel_data *>(ckp);
        char *args[NPY_MAXARGS];
        intptr_t data_types_size = data->data_types_size;
        // Set up the args array the way the numpy ufunc wants it
        memcpy(&args[0], &src[0], (data_types_size - 1) * sizeof(void *));
        args[data_types_size - 1] = dst;
        // Call the ufunc loop with a dim size of 1
        intptr_t strides[NPY_MAXARGS];
        memcpy(&strides[0], &src_stride[0], (data_types_size - 1) * sizeof(intptr_t));
        strides[data_types_size - 1] = dst_stride;
        {
            PyGILState_RAII pgs;
            data->funcptr(args, reinterpret_cast<intptr_t *>(&count), strides,
                            data->ufunc_data);
        }
    }

    static void scalar_ufunc_strided_ckernel_nogil(
                    char *dst, intptr_t dst_stride,
                    const char * const *src, const intptr_t *src_stride,
                    size_t count, ckernel_prefix *ckp)
    {
        scalar_ufunc_ckernel_data *data =
                        reinterpret_cast<scalar_ufunc_ckernel_data *>(ckp);
        char *args[NPY_MAXARGS];
        intptr_t data_types_size = data->data_types_size;
        // Set up the args array the way the numpy ufunc wants it
        memcpy(&args[0], &src[0], (data_types_size - 1) * sizeof(void *));
        args[data_types_size - 1] = dst;
        // Call the ufunc loop with a dim size of 1
        intptr_t strides[NPY_MAXARGS];
        memcpy(&strides[0], &src_stride[0], (data_types_size - 1) * sizeof(intptr_t));
        strides[data_types_size - 1] = dst_stride;
        data->funcptr(args, reinterpret_cast<intptr_t *>(&count), strides, data->ufunc_data);
    }

    static intptr_t instantiate_scalar_ufunc_ckernel(
        void *self_data_ptr, dynd::ckernel_builder *out_ckb,
        intptr_t ckb_offset, const char *const *dynd_metadata,
        uint32_t kerntype, const eval::eval_context *DYND_UNUSED(ectx))
    {
        // Acquire the GIL for creating the ckernel
        PyGILState_RAII pgs;
        scalar_ufunc_deferred_data *data =
                        reinterpret_cast<scalar_ufunc_deferred_data *>(self_data_ptr);
        intptr_t ckb_end = ckb_offset + sizeof(scalar_ufunc_ckernel_data);
        out_ckb->ensure_capacity_leaf(ckb_end);
        scalar_ufunc_ckernel_data *ckd =
                        out_ckb->get_at<scalar_ufunc_ckernel_data>(ckb_offset);
        ckd->base.destructor = &delete_scalar_ufunc_ckernel_data;
        if (kerntype == kernel_request_single) {
            if (data->ckernel_acquires_gil) {
                ckd->base.set_function<expr_single_operation_t>(&scalar_ufunc_single_ckernel_acquiregil);
            } else {
                ckd->base.set_function<expr_single_operation_t>(&scalar_ufunc_single_ckernel_nogil);
            }
        } else if (kerntype == kernel_request_strided) {
            if (data->ckernel_acquires_gil) {
                ckd->base.set_function<expr_strided_operation_t>(&scalar_ufunc_strided_ckernel_acquiregil);
            } else {
                ckd->base.set_function<expr_strided_operation_t>(&scalar_ufunc_strided_ckernel_nogil);
            }
        } else {
            throw runtime_error("unsupported kernel request in instantiate_scalar_ufunc_ckernel");
        }
        ckd->funcptr = data->funcptr;
        ckd->ufunc_data = data->ufunc_data;
        ckd->data_types_size = data->data_types_size;
        ckd->ufunc = data->ufunc;
        Py_INCREF(ckd->ufunc);
        return ckb_end;
    }

} // anonymous namespace

PyObject *pydynd::ckernel_deferred_from_ufunc(PyObject *ufunc,
                PyObject *type_tuple, int ckernel_acquires_gil)
{
    try {
        nd::array ckd = nd::empty(ndt::make_ckernel_deferred());
        ckernel_deferred *ckd_ptr = reinterpret_cast<ckernel_deferred *>(ckd.get_readwrite_originptr());

        // NOTE: This function does not raise C++ exceptions,
        //       it behaves as a Python C-API function.
        if (!PyObject_TypeCheck(ufunc, &PyUFunc_Type)) {
            stringstream ss;
            ss << "a numpy ufunc object is required by this function to create a ckernel_deferred, ";
            pyobject_ownref repr_obj(PyObject_Repr(ufunc));
            ss << "got " << pystring_as_string(repr_obj.get());
            PyErr_SetString(PyExc_TypeError, ss.str().c_str());
            return NULL;
        }
        PyUFuncObject *uf = (PyUFuncObject *)ufunc;
        if (uf->nout != 1) {
            PyErr_SetString(PyExc_TypeError, "numpy ufuncs with multiple return "
                            "arguments are not supported");
            return NULL;
        }
        if (uf->data == (void *)PyUFunc_SetUsesArraysAsData) {
            PyErr_SetString(PyExc_TypeError, "numpy ufuncs which require "
                            "arrays as their data is not supported");
            return NULL;
        }

        // Convert the type tuple to an integer type_num array
        if (!PyTuple_Check(type_tuple)) {
            PyErr_SetString(PyExc_TypeError, "type_tuple must be a tuple");
            return NULL;
        }
        intptr_t nargs = PyTuple_GET_SIZE(type_tuple);
        if (nargs != uf->nin + uf->nout) {
            PyErr_SetString(PyExc_ValueError, "type_tuple has the wrong size for the ufunc");
            return NULL;
        }
        int argtypes[NPY_MAXARGS];
        for (intptr_t i = 0; i < nargs; ++i) {
            PyArray_Descr *dt = NULL;
            if (!PyArray_DescrConverter(PyTuple_GET_ITEM(type_tuple, i), &dt)) {
                return NULL;
            }
            argtypes[i] = dt->type_num;
            Py_DECREF(dt);
        }

        // Search through the main loops for one that matches
        int builtin_count = uf->ntypes;
        for (int i = 0; i < builtin_count; ++i) {
            char *types = uf->types + i * nargs;
            bool matched = true;
            // Match the numpy convention "in, out" vs the dynd convention "out, in"
            for (intptr_t j = 1; j < nargs; ++j) {
                if (argtypes[j] != types[j-1]) {
                    matched = false;
                    break;
                }
            }
            if (argtypes[0] != types[nargs-1]) {
                matched = false;
            }
            // If we found a full match, return the kernel
            if (matched) {
                if (!uf->core_enabled) {
                    size_t out_ckd_size = sizeof(scalar_ufunc_deferred_data) + (nargs - 1) * sizeof(void *);
                    ckd_ptr->data_ptr = malloc(out_ckd_size);
                    memset(ckd_ptr->data_ptr, 0, out_ckd_size);
                    ckd_ptr->ckernel_funcproto = expr_operation_funcproto;
                    ckd_ptr->free_func = &delete_scalar_ufunc_deferred_data;
                    ckd_ptr->instantiate_func = &instantiate_scalar_ufunc_ckernel;
                    ckd_ptr->data_types_size = nargs;
                    // Fill in the ckernel_deferred instance data
                    scalar_ufunc_deferred_data *data = reinterpret_cast<scalar_ufunc_deferred_data *>(ckd_ptr->data_ptr);
                    data->ufunc = uf;
                    Py_INCREF(uf);
                    data->data_types_size = nargs;
                    ckd_ptr->data_dynd_types = reinterpret_cast<ndt::type *>(data->data_types);
                    for (intptr_t j = 0; j < nargs; ++j) {
                        data->data_types[j] = ndt_type_from_numpy_type_num(argtypes[j]).release();
                    }
                    data->ckernel_acquires_gil = ckernel_acquires_gil;
                    data->funcptr = uf->functions[i];
                    data->ufunc_data = uf->data[i];
                    return wrap_array(ckd);
                } else {
                    // TODO: support gufunc
                    PyErr_SetString(PyExc_ValueError, "gufunc isn't implemented yet");
                    return NULL;
                }
            }
        }

        PyErr_SetString(PyExc_ValueError, "converting extended ufunc loops isn't implemented yet");
        return NULL;
    } catch (...) {
        translate_exception();
        return NULL;
    }
}

#endif // DYND_NUMPY_INTEROP