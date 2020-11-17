/* -*- Mode: C; c-basic-offset: 4 -*-
 * vim: tabstop=4 shiftwidth=4 expandtab
 *
 * Copyright (C) 2015 Christoph Reiter <reiter.christoph@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <Python.h>
#include <structmember.h>
#include <glib.h>
#include "pygobject-object.h"
#include "pygi-async.h"
#include "pygi-util.h"
#include "pygi-info.h"
#include "pygi-invoke.h"


static PyObject *asyncio_InvalidStateError;
static PyObject *asyncio_get_event_loop;
static PyObject *cancellable_info;

/* This is never instantiated. */
PYGI_DEFINE_TYPE ("gi._gi.Async", PyGIAsync_Type, PyGIAsync)

/**
 * Async.__repr__() implementation.
 * Takes the _Async.__repr_format format string and applies the finish function
 * info to it.
 */
static PyObject*
async_repr(PyGIAsync *self) {
    PyObject *string;

    string = PyUnicode_FromFormat ("gi.Async(finish_func=%s, done=%s)",
                                   _pygi_g_base_info_get_fullname (self->finish_func->base.info),
                                   (self->result || self->exception) ? "True" : "False");

    return string;
}

/**
 * async_cancel:
 *
 * Cancel the asynchronous operation.
 */
static PyObject *
async_cancel(PyGIAsync *self) {

    return PyObject_CallMethod (self->cancellable, "cancel", NULL);
}

static PyObject *
async_done(PyGIAsync *self) {

    return PyBool_FromLong (self->result || self->exception);
}

static PyObject *
async_result(PyGIAsync *self) {

    if (!self->result && !self->exception) {
        PyErr_SetString(asyncio_InvalidStateError, "Async task is still running!");
        return NULL;
    }

    if (self->result) {
        Py_INCREF (self->result);
        return self->result;
    } else {
        PyErr_SetObject(PyExceptionInstance_Class(self->exception), self->exception);
        return NULL;
    }
}

static PyObject *
async_exception(PyGIAsync *self) {

    PyObject *res;

    if (!self->result && !self->exception) {
        PyErr_SetString(asyncio_InvalidStateError, "Async task is still running!");
        return NULL;
    }

    if (self->exception)
        res =self->exception;
    else
        res = Py_None;

    Py_INCREF (res);
    return res;
}

static PyObject*
call_soon (PyGIAsync *self, PyGIAsyncCallback *cb)
{
    PyObject *call_soon;
    PyObject *args, *kwargs = NULL;
    PyObject *ret;

    call_soon = PyObject_GetAttrString(self->loop, "call_soon");
    if (!call_soon)
        return NULL;

    args = Py_BuildValue ("(OO)", cb->func, self);
#if PY_VERSION_HEX >= 0x03070000
    kwargs = PyDict_New ();
    PyDict_SetItemString (kwargs, "context", cb->context);
    Py_CLEAR (kwargs);
#endif
    ret = PyObject_Call (call_soon, args, kwargs);

    Py_CLEAR (args);
    Py_CLEAR (call_soon);

    return ret;
}

static PyObject*
async_add_done_callback (PyGIAsync *self, PyObject *args, PyObject *kwargs)
{
    PyGIAsyncCallback callback = { NULL };

#if PY_VERSION_HEX >= 0x03070000
    static char * kwlist[] = {"", "context", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$O:add_done_callback", kwlist,
                                     &callback.func, &callback.context))
        return NULL;

    Py_INCREF(callback.func);
    if (callback.context == NULL)
        callback.context = PyContext_CopyCurrent ();
    else
        Py_INCREF(callback.context);
#else
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:add_done_callback", NULL,
                                     &callback.func))
        return NULL;
#endif

    /* Note that we don't need to copy the current context in this case. */
    if (self->result || self->exception) {
        PyObject *res = call_soon (self, &callback);

        Py_DECREF(callback.func);
#if PY_VERSION_HEX >= 0x03070000
        Py_DECREF(callback.context);
#endif
        if (res) {
            Py_DECREF(res);
            Py_RETURN_NONE;
        } else {
            return NULL;
        }
    }

    if (!self->callbacks)
        self->callbacks = g_array_new (TRUE, TRUE, sizeof (PyGIAsyncCallback));

    g_array_append_val (self->callbacks, callback);

    Py_RETURN_NONE;
}

static PyObject*
async_remove_done_callback (PyGIAsync *self, PyObject *fn)
{
    guint i = 0;
    gssize removed = 0;

    while (self->callbacks && i < self->callbacks->len) {
        PyGIAsyncCallback *cb = &g_array_index (self->callbacks, PyGIAsyncCallback, i);

        if (PyObject_RichCompareBool (cb->func, fn, Py_EQ) == 1) {
            Py_DECREF(cb->func);
#if PY_VERSION_HEX >= 0x03070000
            Py_DECREF(cb->context);
#endif

            removed += 1;
            g_array_remove_index (self->callbacks, i);
        } else {
            i += 1;
        }
    }

    return PyLong_FromSsize_t(removed);
}

static int
async_init(PyGIAsync *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = { "finish_func", "cancellable", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O!|$O!:gi._gi.Async.__init__", kwlist,
                                     &PyGICallableInfo_Type, &self->finish_func,
                                     &PyGObject_Type, &self->cancellable))
        return -1;

    /* We need to pull in Gio.Cancellable at some point, but we delay it
     * until really needed to avoid having a dependency.
     */
    if (!cancellable_info) {
        PyObject *gio;

        gio = PyImport_ImportModule("gi.repository.Gio");
        if (gio == NULL) {
            return -1;
        }
        cancellable_info = PyObject_GetAttrString(gio, "Cancellable");
        Py_DECREF(gio);

        if (!cancellable_info)
            return -1;
    }

    if (self->cancellable) {
        int res = PyObject_IsInstance (self->cancellable, cancellable_info);
        if (res == -1)
            return -1;

        if (res == 0) {
            PyErr_SetString (PyExc_TypeError, "cancellable argument needs to be of type Gio.Cancellable");
            return -1;
        }
    } else {
        self->cancellable = PyObject_CallObject (cancellable_info, NULL);
    }

    /* We currently do not support overriding the loop manual. So always set
     * the current thread-local main loop.
     */
    self->loop = PyObject_CallObject (asyncio_get_event_loop, NULL);
    if (!self->loop)
        return -1;

    return 0;
}

static PyMethodDef async_methods[] = {
    {"cancel", (PyCFunction)async_cancel, METH_NOARGS},
    {"done", (PyCFunction)async_done, METH_NOARGS},
    {"result", (PyCFunction)async_result, METH_NOARGS},
    {"exception", (PyCFunction)async_exception, METH_NOARGS},
    {"add_done_callback", (PyCFunction)async_add_done_callback, METH_VARARGS | METH_KEYWORDS},
    {"remove_done_callback", (PyCFunction)async_remove_done_callback, METH_O},
    {NULL, NULL, 0},
};

static PyObject *
async_await (PyGIAsync *self) {
    /* We return ourselves as iterator. This is legal in principle, but we
     * don't stop iterating after one item and just continue indefinately,
     * meaning that certain errors cannot be caught!
     */

    if (!self->result && !self->exception)
        self->_asyncio_future_blocking = TRUE;

    Py_INCREF (self);
    return (PyObject *) self;
}

static PyObject *
async_iternext (PyGIAsync *self) {
    /* Return ourselves if iteration needs to continue. */
    if (!self->result && !self->exception) {
        Py_INCREF (self);
        return (PyObject *) self;
    }

    if (self->exception) {
        PyErr_SetObject(PyExceptionInstance_Class(self->exception), self->exception);
        return NULL;
    } else {
        PyErr_SetObject(PyExc_StopIteration, self->result);
        return NULL;
    }
}

static PyAsyncMethods async_async_methods = {
    .am_await = (unaryfunc) async_await,
};

static void
async_dealloc(PyGIAsync *self)
{
    Py_CLEAR(self->loop);
    Py_CLEAR(self->finish_func);
    if (self->cancellable)
        Py_CLEAR(self->cancellable);
    if (self->result)
        Py_CLEAR(self->result);
    if (self->exception)
        Py_CLEAR(self->exception);

    /* XXX: This should never happen! */
    if (self->callbacks)
        g_array_free (self->callbacks, TRUE);

    PyObject_DEL(self);
}


void
pygi_async_finish_cb (GObject *source_object, gpointer res, PyGIAsync *self)
{
    PyGILState_STATE py_state;
    PyObject *source_pyobj, *res_pyobj, *args;
    PyObject *ret;
    guint i;

    /* Lock the GIL as we are coming into this code without the lock and we
      may be executing python code */
    py_state = PyGILState_Ensure ();

    /* We might still be called at shutdown time. */
    if (!Py_IsInitialized()) {
        PyGILState_Release (py_state);
        return;
    }

    res_pyobj = pygobject_new_full (res, FALSE, NULL);
    if (source_object) {
        source_pyobj = pygobject_new_full (source_object, FALSE, NULL);
        args = Py_BuildValue ("(OO)", source_pyobj, res_pyobj);
    } else {
        source_pyobj = NULL;
        args = Py_BuildValue ("(O)", res_pyobj);
    }
    ret = _wrap_g_callable_info_invoke ((PyGIBaseInfo *) self->finish_func, args, NULL);
    Py_XDECREF (res_pyobj);
    Py_XDECREF (source_pyobj);
    Py_XDECREF (args);

    if (PyErr_Occurred ()) {
        PyObject *exc = NULL, *value = NULL, *traceback = NULL;
        PyObject *exc_val;

        PyErr_Fetch (&exc, &value, &traceback);

        Py_XDECREF (ret);

        if (!value)
            exc_val = PyObject_CallFunction(exc, "");
        else
            exc_val = PyObject_CallFunction(exc, "O", value);

        if (exc_val == NULL)
            exc_val = PyObject_CallFunction(PyExc_TypeError, "Invalid exception from _finish function call!");

        g_assert (exc_val);
        self->exception = exc_val;

        Py_XDECREF (exc);
        Py_XDECREF (value);
        Py_XDECREF (traceback);
        Py_XDECREF (ret);
    } else {
        self->result = ret;
    }

    /* TODO: Call the notifiers */
    for (i = 0; self->callbacks && i < self->callbacks->len; i++) {
        PyGIAsyncCallback *cb = &g_array_index (self->callbacks, PyGIAsyncCallback, i);
        /* We stop calling anything after the first exception, but still clear
         * the internal state as if we did.
         * This matches the pure python implementation of Future.
         */
        if (!PyErr_Occurred ()) {
            ret = call_soon (self, cb);
            if (!ret)
                PyErr_PrintEx (FALSE);
            else
                Py_DECREF(ret);
        }

        Py_DECREF(cb->func);
#if PY_VERSION_HEX >= 0x03070000
        Py_DECREF(cb->context);
#endif
    }
    if (self->callbacks)
        g_array_free(self->callbacks, TRUE);
    self->callbacks = NULL;

    Py_DECREF(self);
    PyGILState_Release (py_state);
}

/**
 * pygi_async_new:
 * @finish_func: A #GIFunctionInfo to wrap that is used to finish.
 *
 * Return a new async instance.
 *
 * Returns: An instance of gi.Async or %NULL on error.
 */
PyObject *
pygi_async_new(PyObject *finish_func) {

    PyObject *res;
    PyObject *args;

    res = PyGIAsync_Type.tp_alloc (&PyGIAsync_Type, 0);

    if (res) {
        args = Py_BuildValue ("(O)", finish_func);
        PyGIAsync_Type.tp_init (res, args, NULL);
        Py_DECREF(args);
    }

    return res;
}

static struct PyMemberDef async_members[] = {
    {
        "_asyncio_future_blocking",
        T_BOOL,
        offsetof(PyGIAsync, _asyncio_future_blocking),
        0,
        NULL
    },
    {
        "_loop",
        T_OBJECT,
        offsetof(PyGIAsync, loop),
        READONLY,
        NULL
    },
    {
        "_finish_func",
        T_OBJECT,
        offsetof(PyGIAsync, finish_func),
        READONLY,
        NULL
    },
    {
        "cancellable",
        T_OBJECT,
        offsetof(PyGIAsync, cancellable),
        READONLY,
        "The Gio.Cancellable associated with the task."
    },
    { NULL, }
};


/**
 * pygi_async_register_types:
 * @module: A Python modules to which Async gets added to.
 *
 * Initializes the Async class and adds it to the passed @module.
 *
 * Returns: -1 on error, 0 on success.
 */
int pygi_async_register_types(PyObject *module) {
    PyObject *asyncio_exceptions = NULL;
    PyObject *asyncio_events = NULL;

    PyGIAsync_Type.tp_dealloc = (destructor)async_dealloc;
    PyGIAsync_Type.tp_repr = (reprfunc)async_repr;
    PyGIAsync_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    PyGIAsync_Type.tp_methods = async_methods;
    PyGIAsync_Type.tp_members = async_members;
    PyGIAsync_Type.tp_as_async = &async_async_methods;
    PyGIAsync_Type.tp_iternext = (iternextfunc) &async_iternext;
    PyGIAsync_Type.tp_init = (initproc)async_init;

    if (PyType_Ready (&PyGIAsync_Type) < 0)
        return -1;

    Py_INCREF (&PyGIAsync_Type);
    if (PyModule_AddObject (module, "Async",
                            (PyObject *)&PyGIAsync_Type) < 0) {
        Py_DECREF (&PyGIAsync_Type);
        return -1;
    }

    asyncio_exceptions = PyImport_ImportModule("asyncio.exceptions");
    if (module == NULL) {
        goto fail;
    }
    asyncio_InvalidStateError = PyObject_GetAttrString(asyncio_exceptions, "InvalidStateError");
    if (asyncio_InvalidStateError == NULL)
        goto fail;

    asyncio_events = PyImport_ImportModule("asyncio.events");
    if (module == NULL) {
        goto fail;
    }
    asyncio_get_event_loop = PyObject_GetAttrString(asyncio_events, "get_event_loop");
    if (asyncio_get_event_loop == NULL)
        goto fail;

    /* Only initialized when really needed! */
    cancellable_info = NULL;

    Py_CLEAR(asyncio_exceptions);
    Py_CLEAR(asyncio_events);
    return 0;

fail:
    Py_CLEAR(asyncio_events);
    return -1;
}
