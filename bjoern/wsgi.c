#include "common.h"
#include "server.h"
#include "bjoernmodule.h"
#include "wsgi.h"

static PyKeywordFunc start_response;
static inline bool _check_start_response(Request*);
static bool wsgi_sendheaders(Request*);
static inline bool wsgi_sendstring(Request*);
static bool wsgi_sendfile(Request*);
static bool wsgi_senditer(Request*);

typedef struct {
    PyObject_HEAD
    Request* request;
} StartResponse;
static PyTypeObject StartResponse_Type;


bool
wsgi_call_application(Request* request)
{
    StartResponse* start_response = PyObject_NEW(StartResponse, &StartResponse_Type);
    start_response->request = request;

    PyObject* retval = PyObject_CallFunctionObjArgs(
        wsgi_app,
        request->headers,
        start_response,
        NULL /* sentinel */
    );

    PyObject_FREE(start_response);

    if(retval == NULL)
        return false;

    request->state |= REQUEST_RESPONSE_WSGI;

    /* Optimize the most common case: */
    if(PyList_Check(retval) && PyList_GET_SIZE(retval) == 1) {
        PyObject* tmp = retval;
        retval = PyList_GET_ITEM(tmp, 0);
        Py_INCREF(retval);
        Py_DECREF(tmp);
        goto string_resp; /* eeeeeevil */
    }


    if(PyFile_Check(retval)) {
        request->state |= REQUEST_WSGI_FILE_RESPONSE;
        request->response = retval;
        goto out;
    }

    if(PyString_Check(retval)) {
string_resp:
        request->state |= REQUEST_WSGI_STRING_RESPONSE;
        request->response = retval;
        goto out;
    }

    PyObject* iter = PyObject_GetIter(retval);
    Py_DECREF(retval);
    TYPECHECK2(iter, PyIter, PySeqIter, "wsgi application return value", false);

    request->state |= REQUEST_WSGI_ITER_RESPONSE;
    request->response = iter;
    /* Get the first item of the iterator, because that may execute code that
     * invokes `start_response` (which might not have been invoked yet).
     * Think of the following scenario:
     *
     *     def app(environ, start_response):
     *         start_response('200 Ok', ...)
     *         yield 'Hello World'
     *
     * That would make `app` return an iterator (more precisely, a generator).
     * Unfortunately, `start_response` wouldn't be called until the first item
     * of that iterator is requested; `start_response` however has to be called
     * _before_ the wsgi body is sent, because it passes the HTTP headers.
     */
    request->response_curiter = PyIter_Next(iter);
    if(PyErr_Occurred())
        return false;

out:
    if(!request->response_headers) {
        PyErr_SetString(
            PyExc_TypeError,
            "wsgi application returned before start_response was called"
        );
        return false;
    }
    return true;
}

bool
wsgi_send_response(Request* request)
{
    if(!(request->state & REQUEST_RESPONSE_HEADERS_SENT)) {
        if(wsgi_sendheaders(request))
            return true;
        request->state |= REQUEST_RESPONSE_HEADERS_SENT;
    }

    request_state state = request->state;
    if(state & REQUEST_WSGI_STRING_RESPONSE)    return wsgi_sendstring(request);
    else if(state & REQUEST_WSGI_FILE_RESPONSE) return wsgi_sendfile(request);
    else if(state & REQUEST_WSGI_ITER_RESPONSE) return wsgi_senditer(request);

    assert(0);
}

static bool
wsgi_sendheaders(Request* request)
{
    char buf[1024*4];
    size_t bufpos = 0;
    #define buf_write(src, len) \
        do { \
            size_t n = len; \
            const char* s = src;  \
            while(n--) buf[bufpos++] = *s++; \
        } while(0)

    buf_write("HTTP/1.1 ", strlen("HTTP/1.1 "));
    buf_write(PyString_AS_STRING(request->status),
              PyString_GET_SIZE(request->status));

    size_t n_headers = PyList_GET_SIZE(request->response_headers);
    for(size_t i=0; i<n_headers; ++i) {
        PyObject* tuple = PyList_GET_ITEM(request->response_headers, i);
        TYPECHECK(tuple, PyTuple, "headers", true);

        if(PyTuple_GET_SIZE(tuple) < 2) {
            PyErr_Format(
                PyExc_TypeError,
                "headers must be tuples of length 2, not %d",
                PyTuple_GET_SIZE(tuple)
            );
            return true;
        }
        PyObject* field = PyTuple_GET_ITEM(tuple, 0);
        PyObject* value = PyTuple_GET_ITEM(tuple, 1);
        TYPECHECK(field, PyString, "header tuple items", true);
        TYPECHECK(value, PyString, "header tuple items", true);

        buf_write("\r\n", strlen("\r\n"));
        buf_write(PyString_AS_STRING(field), PyString_GET_SIZE(field));
        buf_write(": ", strlen(": "));
        buf_write(PyString_AS_STRING(value), PyString_GET_SIZE(value));
    }
    buf_write("\r\n\r\n", strlen("\r\n\r\n"));

    return !sendall(request, buf, bufpos);
}

static inline bool
wsgi_sendstring(Request* request)
{
    sendall(
        request,
        PyString_AS_STRING(request->response),
        PyString_GET_SIZE(request->response)
    );
    return true;
}

static bool
wsgi_sendfile(Request* request)
{
    assert(0);
}

static bool
wsgi_senditer(Request* request)
{
#define ITER_MAXSEND 1024*4
    register PyObject* curiter = request->response_curiter;
    if(!curiter) return true;

    register ssize_t sent = 0;
    while(curiter && sent < ITER_MAXSEND) {
        TYPECHECK(curiter, PyString, "wsgi iterable items", true);
        if(!sendall(request, PyString_AS_STRING(curiter),
                    PyString_GET_SIZE(curiter)))
            return true;
        sent += PyString_GET_SIZE(curiter);
        Py_DECREF(curiter);
        curiter = PyIter_Next(request->response);
        if(PyErr_Occurred()) {
            Py_XDECREF(curiter);
            /* TODO: What to do here? Parts of the response are already sent */
            return true;
        }
    }

    if(curiter) {
        request->response_curiter = curiter;
        return false;
    } else {
        return true;
    }
}



static PyObject*
start_response(PyObject* self, PyObject* args, PyObject* kwargs)
{
    Request* req = ((StartResponse*)self)->request;

    if(req->state & REQUEST_RESPONSE_HEADERS_SENT) {
        PyErr_SetString(
            PyExc_TypeError,
            "start_response called but headers already sent"
        );
        return NULL;
    }

    bool first_call = !req->response_headers;
    PyObject* exc_info = NULL;
    if(!PyArg_UnpackTuple(args, "start_response", 2, 3,
            &req->status, &req->response_headers, &exc_info))
        return NULL;

    if(!first_call) {
        TYPECHECK(exc_info, PyTuple, "start_response argument 3", NULL);
        if(PyTuple_GET_SIZE(exc_info) != 3) {
            PyErr_Format(
                PyExc_TypeError,
                "start_response argument 3 must be a tuple of length 3, "
                "not of length %d",
                PyTuple_GET_SIZE(exc_info)
            );
            return NULL;
        }
        PyErr_Restore(
            PyTuple_GET_ITEM(exc_info, 0),
            PyTuple_GET_ITEM(exc_info, 1),
            PyTuple_GET_ITEM(exc_info, 2)
        );
    }

    TYPECHECK(req->status, PyString, "start_response argument 1", NULL);
    TYPECHECK(req->response_headers, PyList, "start_response argument 2", NULL);

    Py_INCREF(req->status);
    Py_INCREF(req->response_headers);

    Py_RETURN_NONE;
}

static PyTypeObject StartResponse_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "start_response",
    sizeof(StartResponse),
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    start_response /* __call__ */
};
