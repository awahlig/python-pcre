/* python-pcre

Copyright (c) 2012-2014, Arkadiusz Wahlig
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

/* JIT was added in PCRE 8.20. */
#ifdef PCRE_STUDY_JIT_COMPILE
#define PCRE_HAS_JIT_API
#else
#define PCRE_STUDY_JIT_COMPILE (0)
#define pcre_free_study pcre_free
#endif

#define PYPCRE_ERROR_STUDY (-50)

static PyObject *PyExc_PCREError;
static PyObject *PyExc_NoMatch;

/* Extract UTF-8 data from <op>.
 * Returns a new string object containing the encoded UTF-8 data or
 * a new reference to <op> if no encoding was required.
 * If PCRE_UTF8 option is set, string objects are assumed to be UTF-8.
 * Sets PCRE_NO_UTF8_CHECK option if output is guaranteed to be UTF-8.
 */
static PyObject *
get_string(PyObject *op, int *options, const char **string, Py_ssize_t *length)
{
    PyBufferProcs *buffer;

    /* Unicode doesn't always provide buffer interface. */
    if (PyUnicode_Check(op)) {
        op = PyUnicode_AsUTF8String(op);
        if (op == NULL)
            return NULL;

        *string = PyString_AS_STRING(op);
        *length = PyString_GET_SIZE(op);
        *options |= PCRE_NO_UTF8_CHECK;
        return op;
    }

    /* Try the buffer interface. */
    buffer = Py_TYPE(op)->tp_as_buffer;
    if (buffer && buffer->bf_getreadbuffer && buffer->bf_getsegcount
            /* Must have exactly 1 segment. */
            && buffer->bf_getsegcount(op, NULL) == 1) {

        void *ptr;
        Py_ssize_t bytes, size;

        /* Read the segment. */
        bytes = buffer->bf_getreadbuffer(op, 0, &ptr);
        if (bytes < 0)
            return NULL;

        /* Get object length. */
        size = PyObject_Size(op);
        if (size < 0)
            return NULL;

        if (PyString_Check(op) || bytes == size) {
            /* Segment contains bytes. */
            if (*options & PCRE_UTF8)
                Py_INCREF(op);

            else {
                const unsigned char *data = (const unsigned char *)ptr;
                const unsigned char *p, *end = data + size;
                unsigned char c, *q;

                /* Count non-ascii bytes. */
                for (p = data, bytes = 0; p < end; ++p) {
                    if (*p > 127)
                        ++bytes;
                }

                /* If all bytes ascii then valid UTF-8, use as-is. */
                if (bytes == 0)
                    Py_INCREF(op);

                else {
                    /* Non-ascii characters will take two bytes. */
                    size += bytes;
                    op = PyString_FromStringAndSize(NULL, size);
                    if (op == NULL)
                        return NULL;

                    /* Inline Latin1 -> UTF-8 conversion. */
                    q = (unsigned char *)PyString_AS_STRING(op);
                    ptr = q;
                    for (p = data; p < end; ++p) {
                        if ((c = *p) > 127) {
                            *q++ = 0xc0 | (c >> 6);
                            *q++ = 0x80 | (c & 0x3f);
                        }
                        else
                            *q++ = c;
                    }
                }

                /* Either ascii or encoded internally -- no check needed. */
                *options |= PCRE_NO_UTF8_CHECK;
            }

            *string = (const char *)ptr;
            *length = size;
            return op;
        }

        else if (bytes == sizeof(Py_UNICODE) * size) {
            /* Segment contains unicode. */
            op = PyUnicode_EncodeUTF8((const Py_UNICODE *)ptr, size, NULL);
            if (op == NULL)
                return NULL;
            *string = PyString_AS_STRING(op);
            *length = PyString_GET_SIZE(op);
            *options |= PCRE_NO_UTF8_CHECK;
            return op;
        }

        /* Segment contains neither bytes nor unicode. */
        PyErr_SetString(PyExc_TypeError, "buffer size mismatch");
        return NULL;
    }

    /* Unsupported object type. */
    PyErr_Format(PyExc_TypeError, "expected string or buffer, not %.200s",
            Py_TYPE(op)->tp_name);
    return NULL;
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
utf8_byte_to_char_offsets(const char *s, Py_ssize_t length, int *pos, int *endpos)
{
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
utf8_char_to_byte_offsets(const char *s, Py_ssize_t length, int *pos, int *endpos)
{
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

/*
 * Pattern
 */

typedef struct {
    PyObject_HEAD
    PyObject *pattern; /* as passed in */
    PyObject *groupindex; /* name->index dict */
    pcre *code; /* compiled pattern */
    pcre_extra *extra; /* pcre_study result */
#ifdef PCRE_HAS_JIT_API
    pcre_jit_stack *jit_stack; /* user-allocated jit stack */
#endif
    int options; /* effective options */
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

    if (PyInt_Check(index) || PyLong_Check(index))
        i = PyInt_AsSsize_t(index);

    else {
        index = PyDict_GetItem(op->groupindex, index);
        if (index && (PyInt_Check(index) || PyLong_Check(index)))
            i = PyInt_AsSsize_t(index);
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
        if (unicode)
            key = PyUnicode_FromString((const char *)(table + 2));
        else
            key = PyString_FromString((const char *)(table + 2));
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
    int rc, groups, options = 0;
    pcre *code;

    static const char *const kwlist[] = {"pattern", "flags", "loads", NULL};
    
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|iS:__init__", (char **)kwlist,
            &pattern, &options, &loads))
        return -1;

    /* Patterns can be serialized using dumps() and then unserialized
     * using the "loads" argument.  The "pattern" argument is then used
     * only to initialize the "pattern" attribute.
     */
    if (loads) {
        Py_ssize_t size;

        if (options != 0) {
            PyErr_SetString(PyExc_ValueError, "cannot process flags argument "
                "with a serialized pattern");
            return -1;
        }

        size = PyString_GET_SIZE(loads);
        code = pcre_malloc(size);
        if (code == NULL) {
            PyErr_NoMemory();
            return -1;
        }

        memcpy(code, PyString_AS_STRING(loads), size);
    }
    else {
        PyObject *temp;
        const char *data;
        Py_ssize_t length;
        const char *err = NULL;
        int o;

        /* Extract UTF-8 string from the pattern object.  Encode if needed. */
        temp = get_string(pattern, &options, &data, &length);
        if (temp == NULL)
            return -1;

        /* Compile the regex. */
        code = pcre_compile2(data, options | PCRE_UTF8, &rc, &err, &o, NULL);
        if (code == NULL) {
            /* Convert byte offset into character offset if needed. */
            if (temp != pattern)
                utf8_byte_to_char_offsets(data, length, &o, NULL);
            Py_DECREF(temp);

            temp = PyString_FromFormat("%.200s at position %d", err, o);
            if (temp) {
                /* Note.  Compilation error codes are positive. */
                set_pcre_error(rc, PyString_AS_STRING(temp));
                Py_DECREF(temp);
            }
            return -1;
        }
        Py_DECREF(temp);
    }

    /* Get effective options and number of capturing groups. */
    if ((rc = pcre_fullinfo(code, NULL, PCRE_INFO_OPTIONS, &options)) != 0
            || (rc = pcre_fullinfo(code, NULL, PCRE_INFO_CAPTURECOUNT, &groups)) != 0) {
        pcre_free(code);
        set_pcre_error(rc, "failed to query pattern properties");
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

    self->options = options;
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
#ifdef PCRE_HAS_JIT_API
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
#ifdef PCRE_HAS_JIT_API
    int rc, jit;
    pcre_jit_stack *stack;
#endif

    if (!PyArg_ParseTuple(args, "ii", &startsize, &maxsize))
        return NULL;

#ifdef PCRE_HAS_JIT_API
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
    return PyString_FromStringAndSize((char *)self->code, size);
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
    {"flags",       T_INT,      offsetof(PyPatternObject, options),     READONLY},
    {"groups",      T_INT,      offsetof(PyPatternObject, groups),      READONLY},
    {"groupindex",  T_OBJECT,   offsetof(PyPatternObject, groupindex),  READONLY},
    {NULL}      /* sentinel */
};

static PyTypeObject PyPattern_Type = {
    PyObject_HEAD_INIT(NULL)
    0,                                  /* ob_size */
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
    PyObject *data; /* owns UTF-8 data for PCRE */
    int *ovector; /* matched spans */
    int startpos; /* after boundary checks */
    int endpos; /* after boundary checks */
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
    if (op->subject != op->data)
        utf8_byte_to_char_offsets(PyString_AS_STRING(op->data),
                PyString_GET_SIZE(op->data), pos, endpos);

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
    PyObject *subject, *temp;
    int pos = -1, endpos = -1, options = 0, *ovector, ovecsize, startoffset, size, rc;
    const char *data;
    Py_ssize_t length;

    static const char *const kwlist[] = {"pattern", "string", "pos", "endpos", "flags", NULL};
    
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!O|iii:__init__", (char **)kwlist,
            &PyPattern_Type, &pattern, &subject, &pos, &endpos, &options))
        return -1;

    if (assert_pattern_ready(pattern) < 0)
        return -1;

    /* Extract UTF-8 string from the subject object.  Encode if needed. */
    temp = get_string(subject, &options, &data, &length);
    if (temp == NULL)
        return -1;

    /* Check bounds. */
    if (pos < 0)
        pos = 0;
    if (endpos < 0 || endpos > length)
        endpos = length;
    if (pos > endpos) {
        Py_DECREF(temp);
        PyErr_SetNone(PyExc_NoMatch);
        return -1;
    }

    /* If subject has been encoded internally, convert provided character offsets
     * into byte offsets.
     */
    startoffset = pos;
    size = endpos;
    if (temp != subject)
        utf8_char_to_byte_offsets(data, length, &startoffset, &size);

    /* Create ovector array. */
    ovecsize = (pattern->groups + 1) * 3;
    ovector = pcre_malloc(ovecsize * sizeof(int));
    if (ovector == NULL) {
        Py_DECREF(temp);
        PyErr_NoMemory();
        return -1;
    }

    /* Perform the match. */
    rc = pcre_exec(pattern->code, pattern->extra, data, size, startoffset,
            options & ~PCRE_UTF8, ovector, ovecsize);
    if (rc < 0) {
        Py_DECREF(temp);
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

    Py_CLEAR(self->data);
    self->data = temp;

    pcre_free(self->ovector);
    self->ovector = ovector;

    self->startpos = pos;
    self->endpos = endpos;
    self->lastindex = rc - 1;

    return 0;
}

static void
match_dealloc(PyMatchObject *self)
{
    Py_XDECREF(self->pattern);
    Py_XDECREF(self->subject);
    Py_XDECREF(self->data);
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
        if (PyInt_Check(value) && PyInt_AS_LONG(value) == self->lastindex) {
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
    {NULL}      /* sentinel */
};

static PyTypeObject PyMatch_Type = {
    PyObject_HEAD_INIT(NULL)
    0,                                  /* ob_size */
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
 * module
 */

static PyObject *
get_version(PyObject *self)
{
    return PyString_FromString(pcre_version());
}

static PyObject *
get_jit_target(PyObject *self)
{
#ifdef PCRE_HAS_JIT_API
    int rc;
    const char *target;

    /* Query the JIT target.  Sets to NULL if no JIT support. */
    if ((rc = pcre_config(PCRE_CONFIG_JITTARGET, &target)) != 0) {
        set_pcre_error(rc, "failed to query JIT target");
        return NULL;
    }

    if (target)
        return PyString_FromString(target);
#endif

    /* Empty string if PCRE library built without JIT support. */
    return PyString_FromString("");
}

static const PyMethodDef methods[] = {
    {"get_version",     (PyCFunction)get_version,       METH_NOARGS},
    {"get_jit_target",  (PyCFunction)get_jit_target,    METH_NOARGS},
    {NULL}          /* sentinel */
};

PyMODINIT_FUNC
init_pcre(void)
{
    PyObject *mod = Py_InitModule("_pcre", (PyMethodDef *)methods);

    /* Pattern */
    PyPattern_Type.tp_new = PyType_GenericNew;
    PyType_Ready(&PyPattern_Type);
    Py_INCREF(&PyPattern_Type);
    PyModule_AddObject(mod, "Pattern", (PyObject *)&PyPattern_Type);

    /* Match */
    PyMatch_Type.tp_new = PyType_GenericNew;
    PyType_Ready(&PyMatch_Type);
    Py_INCREF(&PyMatch_Type);
    PyModule_AddObject(mod, "Match", (PyObject *)&PyMatch_Type);

    /* NoMatch exception */
    PyExc_NoMatch = PyErr_NewException("pcre.NoMatch",
            PyExc_Exception, NULL);
    Py_INCREF(PyExc_NoMatch);
    PyModule_AddObject(mod, "NoMatch", PyExc_NoMatch);

    /* PCREError exception */
    PyExc_PCREError = PyErr_NewException("pcre.PCREError",
            PyExc_EnvironmentError, NULL);
    Py_INCREF(PyExc_PCREError);
    PyModule_AddObject(mod, "PCREError", PyExc_PCREError);

    /* pcre_compile and/or pcre_exec flags */
    PyModule_AddIntConstant(mod, "IGNORECASE", PCRE_CASELESS);
    PyModule_AddIntConstant(mod, "MULTILINE", PCRE_MULTILINE);
    PyModule_AddIntConstant(mod, "DOTALL", PCRE_DOTALL);
    PyModule_AddIntConstant(mod, "UNICODE", PCRE_UCP);
    PyModule_AddIntConstant(mod, "VERBOSE", PCRE_EXTENDED);
    PyModule_AddIntConstant(mod, "ANCHORED", PCRE_ANCHORED);
    PyModule_AddIntConstant(mod, "NOTBOL", PCRE_NOTBOL);
    PyModule_AddIntConstant(mod, "NOTEOL", PCRE_NOTEOL);
    PyModule_AddIntConstant(mod, "NOTEMPTY", PCRE_NOTEMPTY);
    PyModule_AddIntConstant(mod, "NOTEMPTY_ATSTART", PCRE_NOTEMPTY_ATSTART);
    PyModule_AddIntConstant(mod, "UTF8", PCRE_UTF8);
    PyModule_AddIntConstant(mod, "NO_UTF8_CHECK", PCRE_NO_UTF8_CHECK);

    /* pcre_study flags */
    PyModule_AddIntConstant(mod, "STUDY_JIT", PCRE_STUDY_JIT_COMPILE);
}
