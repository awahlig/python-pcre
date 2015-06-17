/* python-pcre

Copyright (c) 2012-2015, Arkadiusz Wahlig
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <Python.h>
#include <structmember.h>

#include <pcre.h>

#if PY_MAJOR_VERSION >= 3
#    define PY3
#    define PyInt_FromLong PyLong_FromLong
#    if PY_VERSION_HEX >= 0x03030000
#        define PY3_NEW_UNICODE
#    endif
#endif

/* Custom errors/configs. */
#define PYPCRE_ERROR_STUDY      (-50)
#define PYPCRE_CONFIG_NONE      (1000)
#define PYPCRE_CONFIG_VERSION   (1001)

/* JIT was added in PCRE 8.20. */
#ifdef PCRE_STUDY_JIT_COMPILE
#    define PYPCRE_HAS_JIT_API
#else
#    define PCRE_STUDY_JIT_COMPILE  (0)
#    define PCRE_CONFIG_JIT         PYPCRE_CONFIG_NONE
#    define PCRE_CONFIG_JITTARGET   PYPCRE_CONFIG_NONE
#    define pcre_free_study         pcre_free
#endif

/* Flag added in PCRE 8.34. */
#ifndef PCRE_CONFIG_PARENS_LIMIT
#    define PCRE_CONFIG_PARENS_LIMIT    PYPCRE_CONFIG_NONE
#endif

static PyObject *PyExc_PCREError;
static PyObject *PyExc_NoMatch;

/* Used to hold UTF-8 data extracted from any of the supported
 * input objects in a most efficient way.
 */
typedef struct {
    const char *string;
    int length;
    PyObject *op;
    Py_buffer *buffer;
} pypcre_string_t;

/* Release buffer created by pypcre_buffer_get(). */
static void
pypcre_buffer_release(Py_buffer *buffer)
{
    if (buffer) {
        PyBuffer_Release(buffer);
        PyMem_Free(buffer);
    }
}

/* Get new style buffer from object <op>. */
static Py_buffer *
pypcre_buffer_get(PyObject *op, int flags)
{
    Py_buffer *view;

    view = (Py_buffer *)PyMem_Malloc(sizeof(Py_buffer));
    if (view == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    memset(view, 0, sizeof(Py_buffer));
    if (PyObject_GetBuffer(op, view, flags) < 0) {
        PyMem_Free(view);
        return NULL;
    }

    return view;
}

/* Release string created by pypcre_string_get(). */
static void
pypcre_string_release(pypcre_string_t *str)
{
    if (str) {
        pypcre_buffer_release(str->buffer);
        Py_XDECREF(str->op);
        memset(str, 0, sizeof(pypcre_string_t));
    }
}

/* Helper function handling buffers containing bytes. */
static int
_string_get_from_bytes(pypcre_string_t *str, PyObject *op, int *options,
                       Py_buffer *view, int viewrel)
{
    const unsigned char *start = (const unsigned char *)view->buf;
    const unsigned char *p, *end = start + view->len;
    unsigned char c, *q;
    Py_ssize_t count = 0;

    if (!(*options & PCRE_UTF8)) {
        *options |= PCRE_NO_UTF8_CHECK;

        /* Count non-ascii bytes. */
        for (p = start; p < end; ++p) {
            if (*p > 127)
                ++count;
        }
    }

    /* As-is if ascii or declared UTF-8 by caller. */
    if (count == 0) {
        str->string = (const char *)view->buf;
        str->length = view->len;
        /* Save buffer if it needs to be released. */
        if (viewrel)
            str->buffer = view;
        str->op = op;
        Py_INCREF(op);
        return 0;
    }

    /* Non-ascii characters will take two bytes. */
    count += view->len;
    op = PyBytes_FromStringAndSize(NULL, count);
    if (op == NULL) {
        if (viewrel)
            pypcre_buffer_release(view);
        return -1;
    }

    q = (unsigned char *)PyBytes_AS_STRING(op);
    str->string = (const char *)q;
    str->length = count;
    str->op = op;

    /* Inline Latin1 -> UTF-8 conversion. */
    for (p = start; p < end; ++p) {
        if ((c = *p) > 127) {
            *q++ = 0xc0 | (c >> 6);
            *q++ = 0x80 | (c & 0x3f);
        }
        else
            *q++ = c;
    }

    if (viewrel)
        pypcre_buffer_release(view);
    return 0;
}

#ifndef PY3_NEW_UNICODE
/* Helper function handling buffers containing Py_UNICODE. */
static int
_string_get_from_pyunicode(pypcre_string_t *str, int *options, Py_buffer *view,
                           int viewrel, Py_ssize_t size)
{
    PyObject *op;

    op = PyUnicode_EncodeUTF8((const Py_UNICODE *)view->buf, size, NULL);
    if (viewrel)
        pypcre_buffer_release(view);
    if (op == NULL)
        return -1;

    str->string = PyBytes_AS_STRING(op);
    str->length = PyBytes_GET_SIZE(op);
    str->op = op;
    *options |= PCRE_NO_UTF8_CHECK;
    return 0;
}
#endif

/* Extract UTF-8 data from <op>.
 * Sets str->string and str->length to UTF-8 data buffer.
 * Sets str->op to <op> if no encoding was required or to a bytes object
 * owning str->string if encoded internally to UTF-8.
 * If PCRE_UTF8 option is set, bytes-like objects are assumed to be UTF-8.
 * Sets PCRE_NO_UTF8_CHECK option if encoded internally or ascii.
 * Returns 0 if successful or sets an exception and returns -1 in case
 * of an error.
 */
static int
pypcre_string_get(pypcre_string_t *str, PyObject *op, int *options)
{
    memset(str, 0, sizeof(pypcre_string_t));

    /* This is not strictly needed because bytes support the buffer
     * interface but it's more efficient.
     */
    if (PyBytes_Check(op)) {
        Py_buffer view;

        view.buf = PyBytes_AS_STRING(op);
        view.len = PyBytes_GET_SIZE(op);
        return _string_get_from_bytes(str, op, options, &view, 0);
    }

    if (PyUnicode_Check(op)) {
        *options |= PCRE_NO_UTF8_CHECK;

#ifdef PY3_NEW_UNICODE
        if (PyUnicode_READY(op) < 0)
            return -1;

        /* If stored as ascii, return as-is because ascii is a subset of UTF-8. */
        if (PyUnicode_IS_ASCII(op)) {
            str->string = (const char *)PyUnicode_DATA(op);
            str->length = PyUnicode_GET_LENGTH(op);
            str->op = op;
            Py_INCREF(op);
            return 0;
        }
#endif

        /* Encode into UTF-8 bytes object. */
        op = PyUnicode_AsUTF8String(op);
        if (op == NULL)
            return -1;

        str->string = PyBytes_AS_STRING(op);
        str->length = PyBytes_GET_SIZE(op);
        str->op = op;
        return 0;
    }

    /* Try the new buffer interface, */
    if (PyObject_CheckBuffer(op)) {
        Py_buffer *view;

        view = pypcre_buffer_get(op, PyBUF_ND);
        if (view == NULL)
            return -1;

        /* Buffer contains bytes. */
        if (view->itemsize == 1 && view->ndim == 1)
            return _string_get_from_bytes(str, op, options, view, 1);

#ifdef PY3_NEW_UNICODE
        else if ((view->itemsize == 2 || view->itemsize == 4) && view->ndim == 1) {
            /* Buffer contains 2-byte or 4-byte values. */
            PyObject *unicode;

            /* Convert to unicode object. */
            unicode = PyUnicode_FromKindAndData((view->itemsize == 2 ? PyUnicode_2BYTE_KIND :
                    PyUnicode_4BYTE_KIND), view->buf, view->shape[0]);
            pypcre_buffer_release(view);
            if (unicode == NULL)
                return -1;

            /* Encode into UTF-8 bytes object. */
            op = PyUnicode_AsUTF8String(unicode);
            Py_DECREF(unicode);
            if (op == NULL)
                return -1;

            str->string = PyBytes_AS_STRING(op);
            str->length = PyBytes_GET_SIZE(op);
            str->op = op;
            *options |= PCRE_NO_UTF8_CHECK;
            return 0;
        }
#else
        /* Buffer contains Py_UNICODE values. */
        else if (view->itemsize == sizeof(Py_UNICODE) && view->ndim == 1)
            return _string_get_from_pyunicode(str, options, view, 1, view->shape[0]);
#endif

        pypcre_buffer_release(view);
        PyErr_SetString(PyExc_TypeError, "unsupported buffer format");
        return -1;
    }

#ifndef PY3
    /* Try the old buffer interface. */
    if (Py_TYPE(op)->tp_as_buffer
            && Py_TYPE(op)->tp_as_buffer->bf_getreadbuffer
            && Py_TYPE(op)->tp_as_buffer->bf_getsegcount
            /* Must have exactly 1 segment. */
            && Py_TYPE(op)->tp_as_buffer->bf_getsegcount(op, NULL) == 1) {

        Py_buffer view;
        Py_ssize_t size;

        /* Read the segment. */
        view.len = Py_TYPE(op)->tp_as_buffer->bf_getreadbuffer(op, 0, &view.buf);
        if (view.len < 0)
            return -1;

        /* Get object length. */
        size = PyObject_Size(op);
        if (size < 0)
            return -1;

        /* Segment contains bytes. */
        if (PyBytes_Check(op) || view.len == size)
            return _string_get_from_bytes(str, op, options, &view, 0);

        /* Segment contains unicode. */
        else if (view.len == sizeof(Py_UNICODE) * size)
            return _string_get_from_pyunicode(str, options, &view, 0, size);

        PyErr_SetString(PyExc_TypeError, "buffer size mismatch");
        return -1;
    }
#endif

    /* Unsupported object type. */
    PyErr_Format(PyExc_TypeError, "expected string or buffer, not %.200s",
            Py_TYPE(op)->tp_name);
    return -1;
}

#define ISUTF8(c) (((c) & 0xC0) != 0x80)
#ifdef Py_UNICODE_WIDE
#define UTF8LOOPBODY { \
    (void)(ISUTF8(s[++i]) || ISUTF8(s[++i]) || ISUTF8(s[++i]) || ++i); ++charnum; }
#else
#define UTF8LOOPBODY { \
    (void)(ISUTF8(s[++i]) || ISUTF8(s[++i]) || ++i); ++charnum; }
#endif

/* Converts UTF-8 byte offsets into character offsets.
 * If <endpos> is specified, it must not be less than <pos>.
 */
static void
pypcre_string_byte_to_char_offsets(const pypcre_string_t *str, int *pos, int *endpos)
{
    const char *s = str->string;
    Py_ssize_t length = str->length;
    int charnum = 0, i = 0, offset;

    if (pos && (*pos >= 0)) {
        offset = *pos;
        while ((i < offset) && (i < length))
            UTF8LOOPBODY
        *pos = charnum;
    }
    if (endpos && (*endpos >= 0)) {
        offset = *endpos;
        while ((i < offset) && (i < length))
            UTF8LOOPBODY
        *endpos = charnum;
    }
}

/* Converts character offsets into UTF-8 byte offsets.
 * If <endpos> is specified it must not be less than <pos>.
 */
static void
pypcre_string_char_to_byte_offsets(const pypcre_string_t *str, int *pos, int *endpos)
{
    const char *s = str->string;
    Py_ssize_t length = str->length;
    int charnum = 0, i = 0, offset;

    if (pos && (*pos >= 0)) {
        offset = *pos;
        while ((charnum < offset) && (i < length))
            UTF8LOOPBODY
        *pos = i;
    }
    if (endpos && (*endpos >= 0)) {
        offset = *endpos;
        while ((charnum < offset) && (i < length))
            UTF8LOOPBODY
        *endpos = i;
    }
}

/* Sets an exception from PCRE error code and error string. */
static void
set_pcre_error(int rc, const char *s)
{
    PyObject *op;

    switch (rc) {
        case PCRE_ERROR_NOMEMORY:
            PyErr_NoMemory();
            break;

        case PCRE_ERROR_NOMATCH:
            PyErr_SetNone(PyExc_NoMatch);
            break;

        case 5: /* number too big in {} quantifier */
            PyErr_SetString(PyExc_OverflowError, s);
            break;

        default:
            op = Py_BuildValue("(is)", rc, s);
            if (op) {
                PyErr_SetObject(PyExc_PCREError, op);
                Py_DECREF(op);
            }
    }
}

/*
 * Pattern
 */

typedef struct {
    PyObject_HEAD
    PyObject *pattern; /* as passed in */
    PyObject *groupindex; /* name->index dict */
    pcre *code; /* compiled pattern */
    pcre_extra *extra; /* pcre_study result */
#ifdef PYPCRE_HAS_JIT_API
    pcre_jit_stack *jit_stack; /* user-allocated jit stack */
#endif
    int flags; /* as passed in */
    int groups; /* capturing groups count */
} PyPatternObject;

/* Returns 0 if Pattern.__init__ has been called or sets an exception
 * and returns -1 if not.  Pattern.__init__ sets all fields in one go
 * so 0 means they can all be safely used.
 */
static int
assert_pattern_ready(PyPatternObject *op)
{
    if (op && op->code)
        return 0;

    PyErr_SetString(PyExc_AssertionError, "pattern not ready");
    return -1;
}

/* Converts an object into group index or sets an exception and returns -1
 * if object is of bad type or value is out of range.
 * Supports int/long group indexes and str/unicode group names.
 */
static Py_ssize_t
get_index(PyPatternObject *op, PyObject *index)
{
    Py_ssize_t i = -1;

#ifdef PY3
    if (PyLong_Check(index))
        i = PyLong_AsSsize_t(index);
#else
    if (PyInt_Check(index) || PyLong_Check(index))
        i = PyInt_AsSsize_t(index);
#endif

    else {
        index = PyDict_GetItem(op->groupindex, index);
#ifdef PY3
        if (index && PyLong_Check(index))
            i = PyLong_AsSsize_t(index);
#else
        if (index && (PyInt_Check(index) || PyLong_Check(index)))
            i = PyInt_AsSsize_t(index);
#endif
    }

    /* PyInt_AsSsize_t() may have failed. */
    if (PyErr_Occurred())
        return -1;

    /* Return group index if it's in range. */
    if (i >= 0 && i <= op->groups)
        return i;

    PyErr_SetString(PyExc_IndexError, "no such group");
    return -1;
}

/* Create a mapping from group names to group indexes. */
static PyObject *
make_groupindex(pcre *code, int unicode)
{
    PyObject *dict;
    int rc, index, count, size;
    const unsigned char *table;
    PyObject *key, *value;

    if ((rc = pcre_fullinfo(code, NULL, PCRE_INFO_NAMECOUNT, &count)) != 0
            || (rc = pcre_fullinfo(code, NULL, PCRE_INFO_NAMEENTRYSIZE, &size)) != 0
            || (rc = pcre_fullinfo(code, NULL, PCRE_INFO_NAMETABLE, &table)) != 0) {
        set_pcre_error(rc, "failed to query nametable properties");
        return NULL;
    }

    dict = PyDict_New();
    if (dict == NULL)
        return NULL;

    for (index = 0; index < count; ++index) {
        /* Group name starts from the third byte.  Must not be empty. */
        if (table[2] == 0) {
            Py_DECREF(dict);
            set_pcre_error(84, "group name must not be empty");
            return NULL;
        }

        /* Create group name object. */
#ifdef PY3
        /* XXX re module in 3 always uses unicode here */
        key = PyUnicode_FromString((const char *)(table + 2));
#else
        if (unicode)
            key = PyUnicode_FromString((const char *)(table + 2));
        else
            key = PyBytes_FromString((const char *)(table + 2));
#endif
        if (key == NULL) {
            Py_DECREF(dict);
            return NULL;
        }

        /* First two bytes contain the group index. */
        value = PyInt_FromLong((table[0] << 8) | table[1]);
        if (value == NULL) {
            Py_DECREF(key);
            Py_DECREF(dict);
            return NULL;
        }

        rc = PyDict_SetItem(dict, key, value);
        Py_DECREF(value);
        Py_DECREF(key);
        if (rc < 0) {
            Py_DECREF(dict);
            return NULL;
        }

        table += size;
    }

    return dict;
}

static int
pattern_init(PyPatternObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *pattern, *loads = NULL, *groupindex;
    int rc, groups, flags = 0;
    pcre *code;

    static const char *const kwlist[] = {"pattern", "flags", "loads", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|iS:__init__", (char **)kwlist,
            &pattern, &flags, &loads))
        return -1;

    /* Patterns can be serialized using dumps() and then unserialized
     * using the "loads" argument.
     */
    if (loads) {
        Py_ssize_t size;

        size = PyBytes_GET_SIZE(loads);
        code = pcre_malloc(size);
        if (code == NULL) {
            PyErr_NoMemory();
            return -1;
        }

        memcpy(code, PyBytes_AS_STRING(loads), size);
    }
    else {
        pypcre_string_t str;
        const char *err = NULL;
        int o, options = flags;

        /* Extract UTF-8 string from the pattern object.  Encode if needed. */
        if (pypcre_string_get(&str, pattern, &options) < 0)
            return -1;

        /* Compile the regex. */
        code = pcre_compile2(str.string, options | PCRE_UTF8, &rc, &err, &o, NULL);
        if (code == NULL) {
            PyObject *op;

            /* Convert byte offset into character offset if needed. */
            if (str.op != pattern)
                pypcre_string_byte_to_char_offsets(&str, &o, NULL);
            pypcre_string_release(&str);

            op = PyBytes_FromFormat("%.200s at position %d", err, o);
            if (op) {
                /* Note.  Compilation error codes are positive. */
                set_pcre_error(rc, PyBytes_AS_STRING(op));
                Py_DECREF(op);
            }
            return -1;
        }
        pypcre_string_release(&str);
    }

    /* Get number of capturing groups. */
    if ((rc = pcre_fullinfo(code, NULL, PCRE_INFO_CAPTURECOUNT, &groups)) != 0) {
        pcre_free(code);
        set_pcre_error(rc, "failed to query number of capturing groups");
        return -1;
    }

    /* Create a dict mapping named group names to their indexes. */
    groupindex = make_groupindex(code, PyUnicode_Check(pattern));
    if (groupindex == NULL) {
        pcre_free(code);
        return -1;
    }

    pcre_free(self->code);
    self->code = code;

    Py_CLEAR(self->pattern);
    self->pattern = pattern;
    Py_INCREF(pattern);

    Py_CLEAR(self->groupindex);
    self->groupindex = groupindex;

    self->flags = flags;
    self->groups = groups;

    return 0;
}

static void
pattern_dealloc(PyPatternObject *self)
{
    Py_XDECREF(self->pattern);
    Py_XDECREF(self->groupindex);
    pcre_free(self->code);
    pcre_free_study(self->extra);
#ifdef PYPCRE_HAS_JIT_API
    if (self->jit_stack)
        pcre_jit_stack_free(self->jit_stack);
#endif
    Py_TYPE(self)->tp_free(self);
}

static PyObject *
pattern_study(PyPatternObject *self, PyObject *args)
{
    int options = 0;
    const char *err = NULL;
    pcre_extra *extra;

    if (!PyArg_ParseTuple(args, "|i:study", &options))
        return NULL;

    if (assert_pattern_ready(self) < 0)
        return NULL;

    /* Study the pattern. */
    extra = pcre_study(self->code, options, &err);
    if (err) {
        set_pcre_error(PYPCRE_ERROR_STUDY, err);
        return NULL;
    }

    /* Replace previous study results. */
    pcre_free_study(self->extra);
    self->extra = extra;

    /* Return True if studying the pattern produced additional
     * information that will help speed up matching.
     */
    return PyBool_FromLong(extra != NULL);
}

static PyObject *
pattern_set_jit_stack(PyPatternObject *self, PyObject *args)
{
    int startsize, maxsize;
#ifdef PYPCRE_HAS_JIT_API
    int rc, jit;
    pcre_jit_stack *stack;
#endif

    if (!PyArg_ParseTuple(args, "ii", &startsize, &maxsize))
        return NULL;

#ifdef PYPCRE_HAS_JIT_API
    /* Check whether PCRE library has been built with JIT support. */
    if ((rc = pcre_config(PCRE_CONFIG_JIT, &jit)) != 0) {
        set_pcre_error(rc, "failed to query JIT support");
        return NULL;
    }

    /* Error if no JIT support. */
    if (!jit) {
        PyErr_SetString(PyExc_AssertionError, "PCRE library built without JIT support");
        return NULL;
    }

    /* Assigning a new JIT stack requires a studied pattern. */
    if (self->extra == NULL) {
        PyErr_SetString(PyExc_AssertionError, "pattern must be studied first");
        return NULL;
    }

    /* Allocate new JIT stack. */
    stack = pcre_jit_stack_alloc(startsize, maxsize);
    if (stack == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    /* Release old stack and assign the new one. */
    if (self->jit_stack)
        pcre_jit_stack_free(self->jit_stack);
    self->jit_stack = stack;
    pcre_assign_jit_stack(self->extra, NULL, stack);

    Py_RETURN_NONE;

#else
    /* JIT API not supported. */
    PyErr_SetString(PyExc_AssertionError, "PCRE library too old");
    return NULL;

#endif
}

/* Serializes a pattern into a string.
 * Pattern can be unserialized using the "loads" argument of __init__.
 */
static PyObject *
pattern_dumps(PyPatternObject *self)
{
    size_t size;
    int rc;

    if (assert_pattern_ready(self) < 0)
        return NULL;

    rc = pcre_fullinfo(self->code, NULL, PCRE_INFO_SIZE, &size);
    if (rc != 0) {
        set_pcre_error(rc, "failed to query pattern size");
        return NULL;
    }
    return PyBytes_FromStringAndSize((char *)self->code, size);
}

static PyObject *
pattern_richcompare(PyPatternObject *self, PyObject *other, int op);

static const PyMethodDef pattern_methods[] = {
    {"study",           (PyCFunction)pattern_study,             METH_VARARGS},
    {"set_jit_stack",   (PyCFunction)pattern_set_jit_stack,     METH_VARARGS},
    {"dumps",           (PyCFunction)pattern_dumps,             METH_NOARGS},
    {NULL}      /* sentinel */
};

static const PyMemberDef pattern_members[] = {
    {"pattern",     T_OBJECT,   offsetof(PyPatternObject, pattern),     READONLY},
    {"flags",       T_INT,      offsetof(PyPatternObject, flags),       READONLY},
    {"groups",      T_INT,      offsetof(PyPatternObject, groups),      READONLY},
    {"groupindex",  T_OBJECT,   offsetof(PyPatternObject, groupindex),  READONLY},
    {NULL}      /* sentinel */
};

static PyTypeObject PyPattern_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_pcre.Pattern",                    /* tp_name */
    sizeof(PyPatternObject),            /* tp_basicsize */
    0,                                  /* tp_itemsize */
    (destructor)pattern_dealloc,        /* tp_dealloc */
    0,                                  /* tp_print */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_compare */
    0,                                  /* tp_repr */
    0,                                  /* tp_as_number */
    0,                                  /* tp_as_sequence */
    0,                                  /* tp_as_mapping */
    0,                                  /* tp_hash */
    0,                                  /* tp_call */
    0,                                  /* tp_str */
    0,                                  /* tp_getattro */
    0,                                  /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   /* tp_flags */
    0,                                  /* tp_doc */
    0,                                  /* tp_traverse */
    0,                                  /* tp_clear */
    (richcmpfunc)pattern_richcompare,   /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    0,                                  /* tp_iter */
    0,                                  /* tp_iternext */
    (PyMethodDef *)pattern_methods,     /* tp_methods */
    (PyMemberDef *)pattern_members,     /* tp_members */
    0,                                  /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    (initproc)pattern_init,             /* tp_init */
    0,                                  /* tp_alloc */
    0,                                  /* tp_new */
    0,                                  /* tp_free */
};

static PyObject *
pattern_richcompare(PyPatternObject *self, PyObject *otherobj, int op)
{
    PyPatternObject *other;
    int equal, rc;
    size_t size, other_size;

    /* Only == and != comparisons to another pattern supported. */
    if (!PyObject_TypeCheck(otherobj, &PyPattern_Type) || (op != Py_EQ && op != Py_NE)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    other = (PyPatternObject *)otherobj;
    if (self->code == other->code)
        equal = 1;
    else if (self->code == NULL || other->code == NULL)
        equal = 0;
    else if ((rc = pcre_fullinfo(self->code, NULL, PCRE_INFO_SIZE, &size)) != 0
            || (rc = pcre_fullinfo(other->code, NULL, PCRE_INFO_SIZE, &other_size)) != 0) {
        set_pcre_error(rc, "failed to query pattern size");
        return NULL;
    }
    else if (size != other_size)
        equal = 0;
    else
        equal = (memcmp(self->code, other->code, size) == 0);

    return PyBool_FromLong(op == Py_EQ ? equal : !equal);
}

/*
 * Match
 */

typedef struct {
    PyObject_HEAD
    PyPatternObject *pattern; /* pattern instance */
    PyObject *subject; /* as passed in */
    pypcre_string_t str; /* UTF-8 string */
    int *ovector; /* matched spans */
    int startpos; /* after boundary checks */
    int endpos; /* after boundary checks */
    int flags; /* as passed in */
    int lastindex; /* returned by pcre_exec */
} PyMatchObject;

/* Returns 0 if Match.__init__ has been called or sets an exception
 * and returns -1 if not.  Match.__init__ sets all fields in one go
 * so 0 means they can all be safely used.
 */
static int
assert_match_ready(PyMatchObject *op)
{
    if (op && op->ovector)
        return 0;

    PyErr_SetString(PyExc_AssertionError, "match not ready");
    return -1;
}

/* Retrieves offsets into the subject object for given group.  Both
 * pos and endpos can be NULL if not used.  Returns 0 if successful
 * or sets an exception and returns -1 in case of an error.
 */
static int
get_span(PyMatchObject *op, Py_ssize_t index, int *pos, int *endpos)
{
    if (index < 0 || index > op->pattern->groups) {
        PyErr_SetString(PyExc_IndexError, "no such group");
        return -1;
    }

    if (pos)
        *pos = op->ovector[index * 2];
    if (endpos)
        *endpos = op->ovector[index * 2 + 1];

    /* Sanity check. */
    if (pos && endpos && (*pos > *endpos) && (*endpos >= 0)) {
        PyErr_SetString(PyExc_RuntimeError, "bad span");
        return -1;
    }

    /* If subject has been encoded internally to UTF-8,
     * convert byte offsets into character offsets.
     */
    if (op->subject != op->str.op)
        pypcre_string_byte_to_char_offsets(&op->str, pos, endpos);

    return 0;
}

/* Slices the subject string using offsets from given group.  If group
 * has no match, returns the default object.  Returns new reference.
 */
static PyObject *
get_slice(PyMatchObject *op, Py_ssize_t index, PyObject *def)
{
    int pos, endpos;

    if (get_span(op, index, &pos, &endpos) < 0)
        return NULL;

    if (pos >= 0 && endpos >= 0)
        return PySequence_GetSlice(op->subject, pos, endpos);

    Py_INCREF(def);
    return def;
}

/* Same as get_slice() but takes PyObject group index. */
static PyObject *
get_slice_o(PyMatchObject *op, PyObject *index, PyObject *def)
{
    Py_ssize_t i = get_index(op->pattern, index);
    if (i < 0)
        return NULL;

    return get_slice(op, i, def);
}

static int
match_init(PyMatchObject *self, PyObject *args, PyObject *kwds)
{
    PyPatternObject *pattern;
    PyObject *subject;
    int pos = -1, endpos = -1, flags = 0, options, *ovector, ovecsize, startoffset, size, rc;
    pypcre_string_t str;

    static const char *const kwlist[] = {"pattern", "string", "pos", "endpos", "flags", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!O|iii:__init__", (char **)kwlist,
            &PyPattern_Type, &pattern, &subject, &pos, &endpos, &flags))
        return -1;

    if (assert_pattern_ready(pattern) < 0)
        return -1;

    /* Extract UTF-8 string from the subject object.  Encode if needed. */
    options = flags;
    if (pypcre_string_get(&str, subject, &options) < 0)
        return -1;

    /* Check bounds. */
    if (pos < 0)
        pos = 0;
    if (endpos < 0 || endpos > str.length)
        endpos = str.length;
    if (pos > endpos) {
        pypcre_string_release(&str);
        PyErr_SetNone(PyExc_NoMatch);
        return -1;
    }

    /* If subject has been encoded internally, convert provided character offsets
     * into byte offsets.
     */
    startoffset = pos;
    size = endpos;
    if (str.op != subject)
        pypcre_string_char_to_byte_offsets(&str, &startoffset, &size);

    /* Create ovector array. */
    ovecsize = (pattern->groups + 1) * 3;
    ovector = pcre_malloc(ovecsize * sizeof(int));
    if (ovector == NULL) {
        pypcre_string_release(&str);
        PyErr_NoMemory();
        return -1;
    }

    /* Perform the match. */
    rc = pcre_exec(pattern->code, pattern->extra, str.string, size, startoffset,
            options & ~PCRE_UTF8, ovector, ovecsize);
    if (rc < 0) {
        pypcre_string_release(&str);
        pcre_free(ovector);
        set_pcre_error(rc, "failed to match pattern");
        return -1;
    }

    Py_CLEAR(self->pattern);
    self->pattern = pattern;
    Py_INCREF(pattern);

    Py_CLEAR(self->subject);
    self->subject = subject;
    Py_INCREF(subject);

    pypcre_string_release(&self->str);
    memcpy(&self->str, &str, sizeof(pypcre_string_t));

    pcre_free(self->ovector);
    self->ovector = ovector;

    self->startpos = pos;
    self->endpos = endpos;
    self->flags = flags;
    self->lastindex = rc - 1;

    return 0;
}

static void
match_dealloc(PyMatchObject *self)
{
    Py_XDECREF(self->pattern);
    Py_XDECREF(self->subject);
    pypcre_string_release(&self->str);
    pcre_free(self->ovector);
    Py_TYPE(self)->tp_free(self);
}

static PyObject *
match_group(PyMatchObject *self, PyObject *args)
{
    PyObject *result;
    Py_ssize_t i, size;

    if (assert_match_ready(self) < 0)
        return NULL;

    size = PyTuple_GET_SIZE(args);
    switch (size) {
        case 0: /* no args -- return the whole match */
            result = get_slice(self, 0, Py_None);
            break;

        case 1: /* one arg -- return a single slice */
            result = get_slice_o(self, PyTuple_GET_ITEM(args, 0), Py_None);
            break;

        default: /* more than one arg -- return a tuple of slices */
            result = PyTuple_New(size);
            if (result == NULL)
                return NULL;
            for (i = 0; i < size; ++i) {
                PyObject *item = get_slice_o(self, PyTuple_GET_ITEM(args, i), Py_None);
                if (item == NULL) {
                    Py_DECREF(result);
                    return NULL;
                }
                PyTuple_SET_ITEM(result, i, item);
            }
            break;
    }

    return result;
}

static PyObject *
match_start(PyMatchObject *self, PyObject *args)
{
    PyObject *index = NULL;
    Py_ssize_t i = 0;
    int pos;

    if (!PyArg_UnpackTuple(args, "start", 0, 1, &index))
        return NULL;

    if (assert_match_ready(self) < 0)
        return NULL;

    if (index) {
        i = get_index(self->pattern, index);
        if (i < 0)
            return NULL;
    }

    if (get_span(self, i, &pos, NULL) < 0)
        return NULL;

    return PyInt_FromLong(pos);
}

static PyObject *
match_end(PyMatchObject *self, PyObject *args)
{
    PyObject *index = NULL;
    Py_ssize_t i = 0;
    int endpos;

    if (!PyArg_UnpackTuple(args, "end", 0, 1, &index))
        return NULL;

    if (assert_match_ready(self) < 0)
        return NULL;

    if (index) {
        i = get_index(self->pattern, index);
        if (i < 0)
            return NULL;
    }

    if (get_span(self, i, NULL, &endpos) < 0)
        return NULL;

    return PyInt_FromLong(endpos);
}

static PyObject *
match_span(PyMatchObject *self, PyObject *args)
{
    PyObject *index = NULL;
    Py_ssize_t i = 0;
    int pos, endpos;

    if (!PyArg_UnpackTuple(args, "span", 0, 1, &index))
        return NULL;

    if (assert_match_ready(self) < 0)
        return NULL;

    if (index) {
        i = get_index(self->pattern, index);
        if (i < 0)
            return NULL;
    }

    if (get_span(self, i, &pos, &endpos) < 0)
        return NULL;

    return Py_BuildValue("(ii)", pos, endpos);
}

static PyObject *
match_groups(PyMatchObject *self, PyObject *args)
{
    PyObject *result;
    PyObject *def = Py_None;
    Py_ssize_t index;

    if (!PyArg_UnpackTuple(args, "groups", 0, 1, &def))
        return NULL;

    if (assert_match_ready(self) < 0)
        return NULL;

    result = PyTuple_New(self->pattern->groups);
    if (result == NULL)
        return NULL;

    for (index = 1; index <= self->pattern->groups; ++index) {
        PyObject *item = get_slice(self, index, def);
        if (item == NULL) {
            Py_DECREF(result);
            return NULL;
        }
        PyTuple_SET_ITEM(result, index - 1, item);
    }

    return result;
}

static PyObject *
match_groupdict(PyMatchObject *self, PyObject *args)
{
    PyObject *def = Py_None;
    PyObject *dict, *key, *value;
    Py_ssize_t pos;
    int rc;

    if (!PyArg_UnpackTuple(args, "groupdict", 0, 1, &def))
        return NULL;

    if (assert_match_ready(self) < 0)
        return NULL;

    dict = PyDict_New();
    if (dict == NULL)
        return NULL;

    pos = 0;
    while (PyDict_Next(self->pattern->groupindex, &pos, &key, &value)) {
        value = get_slice_o(self, value, def);
        if (value == NULL) {
            Py_DECREF(dict);
            return NULL;
        }
        rc = PyDict_SetItem(dict, key, value);
        Py_DECREF(value);
        if (rc < 0) {
            Py_DECREF(dict);
            return NULL;
        }
    }

    return dict;
}

static PyObject *
match_lastindex_getter(PyMatchObject *self, void *closure)
{
    if (self->lastindex > 0)
        return PyInt_FromLong(self->lastindex);
    Py_RETURN_NONE;
}

static PyObject *
match_lastgroup_getter(PyMatchObject *self, void *closure)
{
    PyObject *key, *value;
    Py_ssize_t pos;

    if (assert_match_ready(self) < 0)
        return NULL;

    /* Simple reverse lookup into groupindex. */
    pos = 0;
    while (PyDict_Next(self->pattern->groupindex, &pos, &key, &value)) {
#ifdef PY3
        if (PyLong_Check(value) && PyLong_AS_LONG(value) == self->lastindex)
#else
        if (PyInt_Check(value) && PyInt_AS_LONG(value) == self->lastindex)
#endif
        {
            Py_INCREF(key);
            return key;
        }
    }

    Py_RETURN_NONE;
}

static PyObject *
match_regs_getter(PyMatchObject *self, void *closure)
{
    PyObject *regs, *item;
    Py_ssize_t count, i;

    if (assert_match_ready(self) < 0)
        return NULL;

    count = self->pattern->groups + 1;
    regs = PyTuple_New(count);
    if (regs == NULL)
        return NULL;

    for (i = 0; i < count; ++i) {
        item = Py_BuildValue("(ii)", self->ovector[(i * 2)],
                self->ovector[(i * 2) + 1]);
        if (item == NULL) {
            Py_DECREF(regs);
            return NULL;
        }
        PyTuple_SET_ITEM(regs, i, item);
    }

    return regs;
}

static const PyMethodDef match_methods[] = {
    {"group",       (PyCFunction)match_group,       METH_VARARGS},
    {"start",       (PyCFunction)match_start,       METH_VARARGS},
    {"end",         (PyCFunction)match_end,         METH_VARARGS},
    {"span",        (PyCFunction)match_span,        METH_VARARGS},
    {"groups",      (PyCFunction)match_groups,      METH_VARARGS},
    {"groupdict",   (PyCFunction)match_groupdict,   METH_VARARGS},
    {NULL}      /* sentinel */
};

static const PyGetSetDef match_getset[] = {
    {"lastindex",   (getter)match_lastindex_getter},
    {"lastgroup",   (getter)match_lastgroup_getter},
    {"regs",        (getter)match_regs_getter},
    {NULL}      /* sentinel */
};

static const PyMemberDef match_members[] = {
    {"string",      T_OBJECT,   offsetof(PyMatchObject, subject),   READONLY},
    {"re",          T_OBJECT,   offsetof(PyMatchObject, pattern),   READONLY},
    {"pos",         T_INT,      offsetof(PyMatchObject, startpos),  READONLY},
    {"endpos",      T_INT,      offsetof(PyMatchObject, endpos),    READONLY},
    {"flags",       T_INT,      offsetof(PyMatchObject, flags),     READONLY},
    {NULL}      /* sentinel */
};

static PyTypeObject PyMatch_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_pcre.Match",                      /* tp_name */
    sizeof(PyMatchObject),              /* tp_basicsize */
    0,                                  /* tp_itemsize */
    (destructor)match_dealloc,          /* tp_dealloc */
    0,                                  /* tp_print */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_compare */
    0,                                  /* tp_repr */
    0,                                  /* tp_as_number */
    0,                                  /* tp_as_sequence */
    0,                                  /* tp_as_mapping */
    0,                                  /* tp_hash */
    0,                                  /* tp_call */
    0,                                  /* tp_str */
    0,                                  /* tp_getattro */
    0,                                  /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   /* tp_flags */
    0,                                  /* tp_doc */
    0,                                  /* tp_traverse */
    0,                                  /* tp_clear */
    0,                                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    0,                                  /* tp_iter */
    0,                                  /* tp_iternext */
    (PyMethodDef *)match_methods,       /* tp_methods */
    (PyMemberDef *)match_members,       /* tp_members */
    (PyGetSetDef *)match_getset,        /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    (initproc)match_init,               /* tp_init */
    0,                                  /* tp_alloc */
    0,                                  /* tp_new */
    0,                                  /* tp_free */
};

/*
 * _pcre
 */

static int
_config_do_get_int(PyObject *dict, const char *name, int what, int boolean)
{
    int rc, value = 0;
    PyObject *op;

    if (what != PYPCRE_CONFIG_NONE)
        pcre_config(what, &value);

    if (boolean)
        op = PyBool_FromLong(value);
    else
        op = PyInt_FromLong(value);
    if (op == NULL)
        return -1;

    rc = PyDict_SetItemString(dict, name, op);
    Py_DECREF(op);
    return rc;
}

static unsigned long
_config_get_ulong(PyObject *dict, const char *name, int what)
{
    int rc;
    unsigned long value = 0;
    PyObject *op;

    if (what != PYPCRE_CONFIG_NONE)
        pcre_config(what, &value);

    op = PyInt_FromLong(value);
    if (op == NULL)
        return -1;

    rc = PyDict_SetItemString(dict, name, op);
    Py_DECREF(op);
    return rc;
}

static int
_config_get_int(PyObject *dict, const char *name, int what)
{
    return _config_do_get_int(dict, name, what, 0);
}

static int
_config_get_bool(PyObject *dict, const char *name, int what)
{
    return _config_do_get_int(dict, name, what, 1);
}

static int
_config_get_str(PyObject *dict, const char *name, int what)
{
    int rc;
    const char *value = NULL;
    PyObject *op;

    if (what == PYPCRE_CONFIG_VERSION)
        value = pcre_version();
    else if (what != PYPCRE_CONFIG_NONE)
        pcre_config(what, &value);

    if (value == NULL)
        value = "";

#ifdef PY3
    op = PyUnicode_FromString(value);
#else
    op = PyBytes_FromString(value);
#endif
    if (op == NULL)
        return -1;

    rc = PyDict_SetItemString(dict, name, op);
    Py_DECREF(op);
    return rc;
}

static PyObject *
get_config(PyObject *self)
{
    PyObject *dict;

    dict = PyDict_New();
    if (dict == NULL)
        return NULL;

    if (_config_get_str(dict, "version", PYPCRE_CONFIG_VERSION) < 0
            || _config_get_bool(dict, "utf_8", PCRE_CONFIG_UTF8) < 0
            || _config_get_bool(dict, "unicode_properties", PCRE_CONFIG_UNICODE_PROPERTIES) < 0
            || _config_get_bool(dict, "jit", PCRE_CONFIG_JIT) < 0
            || _config_get_str(dict, "jit_target", PCRE_CONFIG_JITTARGET) < 0
            || _config_get_int(dict, "newline", PCRE_CONFIG_NEWLINE) < 0
            || _config_get_bool(dict, "bsr", PCRE_CONFIG_BSR) < 0
            || _config_get_int(dict, "link_size", PCRE_CONFIG_LINK_SIZE) < 0
            || _config_get_int(dict, "parens_limit", PCRE_CONFIG_PARENS_LIMIT) < 0
            || _config_get_ulong(dict, "match_limit", PCRE_CONFIG_MATCH_LIMIT) < 0
            || _config_get_ulong(dict, "match_limit_recursion", PCRE_CONFIG_MATCH_LIMIT_RECURSION) < 0
            || _config_get_bool(dict, "stack_recurse", PCRE_CONFIG_STACKRECURSE) < 0) {
        Py_DECREF(dict);
        return NULL;
    }

    return dict;
}

static const PyMethodDef pypcre_methods[] = {
    {"get_config",  (PyCFunction)get_config,    METH_NOARGS},
    {NULL}          /* sentinel */
};

#ifdef PY3
static PyModuleDef pypcre_module = {
    PyModuleDef_HEAD_INIT,
    "_pcre",
    NULL,
    -1,
    (PyMethodDef *)pypcre_methods
};
#endif

static PyObject *
pypcre_init(void)
{
    PyObject *m;

    /* Use Python memory manager for PCRE allocations. */
    pcre_malloc = PyMem_Malloc;
    pcre_free = PyMem_Free;

    /* _pcre */
#ifdef PY3
    m = PyModule_Create(&pypcre_module);
#else
    m = Py_InitModule("_pcre", (PyMethodDef *)pypcre_methods);
#endif
    if (m == NULL)
        return NULL;

    /* Pattern */
    PyPattern_Type.tp_new = PyType_GenericNew;
    PyType_Ready(&PyPattern_Type);
    Py_INCREF(&PyPattern_Type);
    PyModule_AddObject(m, "Pattern", (PyObject *)&PyPattern_Type);

    /* Match */
    PyMatch_Type.tp_new = PyType_GenericNew;
    PyType_Ready(&PyMatch_Type);
    Py_INCREF(&PyMatch_Type);
    PyModule_AddObject(m, "Match", (PyObject *)&PyMatch_Type);

    /* NoMatch exception */
    PyExc_NoMatch = PyErr_NewException("pcre.NoMatch",
            PyExc_Exception, NULL);
    Py_INCREF(PyExc_NoMatch);
    PyModule_AddObject(m, "NoMatch", PyExc_NoMatch);

    /* PCREError exception */
    PyExc_PCREError = PyErr_NewException("pcre.PCREError",
            PyExc_EnvironmentError, NULL);
    Py_INCREF(PyExc_PCREError);
    PyModule_AddObject(m, "PCREError", PyExc_PCREError);

    /* pcre_compile and/or pcre_exec flags */
    PyModule_AddIntConstant(m, "IGNORECASE", PCRE_CASELESS);
    PyModule_AddIntConstant(m, "MULTILINE", PCRE_MULTILINE);
    PyModule_AddIntConstant(m, "DOTALL", PCRE_DOTALL);
    PyModule_AddIntConstant(m, "UNICODE", PCRE_UCP);
    PyModule_AddIntConstant(m, "VERBOSE", PCRE_EXTENDED);
    PyModule_AddIntConstant(m, "ANCHORED", PCRE_ANCHORED);
    PyModule_AddIntConstant(m, "NOTBOL", PCRE_NOTBOL);
    PyModule_AddIntConstant(m, "NOTEOL", PCRE_NOTEOL);
    PyModule_AddIntConstant(m, "NOTEMPTY", PCRE_NOTEMPTY);
    PyModule_AddIntConstant(m, "NOTEMPTY_ATSTART", PCRE_NOTEMPTY_ATSTART);
    PyModule_AddIntConstant(m, "UTF8", PCRE_UTF8);
    PyModule_AddIntConstant(m, "NO_UTF8_CHECK", PCRE_NO_UTF8_CHECK);

    /* pcre_study flags */
    PyModule_AddIntConstant(m, "STUDY_JIT", PCRE_STUDY_JIT_COMPILE);

    return m;
}

#ifdef PY3
PyMODINIT_FUNC
PyInit__pcre(void)
{
    return pypcre_init();
}
#else
PyMODINIT_FUNC
init_pcre(void)
{
    pypcre_init();
}
#endif
