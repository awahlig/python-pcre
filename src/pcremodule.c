/* python-pcre

Copyright (c) 2012, Arkadiusz Wahlig
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

static PyObject *PyExc_PCREError;

typedef struct {
    PyObject_HEAD
    PyObject *pattern; /* str or unicode as passed in */
    pcre16 *code;
    int options;
    int groups; /* capture count */
    PyObject *groupindex; /* name->index dict */
} PyPatternObject;

typedef struct {
    PyObject_HEAD
    PyPatternObject *pattern;
    PyObject *string; /* str or unicode as passed in */
    int *ovector;
    int lastindex;
    int startpos; /* after boundary checks */
    int endpos; /* after boundary checks */
} PyMatchObject;

/* Returns a new reference to a str object with UTF-16 data.
 * The str always has a BOM at the beginning.
 */
static PyObject *
as_utf16(PyObject *op)
{
    if (PyUnicode_Check(op))
        return PyUnicode_AsUTF16String(op);

    if (PyString_Check(op)) {
        PyObject *t = PyUnicode_DecodeLatin1(PyString_AS_STRING(op),
                PyString_GET_SIZE(op), NULL);
        if (t == NULL)
            return NULL;
        op = PyUnicode_AsUTF16String(t);
        Py_DECREF(t);
        return op;
    }

    PyErr_SetString(PyExc_TypeError, "unicode or str expected");
    return NULL;
}

/* Returns a new reference to a unicode object from UTF16 data.
 */
static PyObject *
from_utf16(PCRE_SPTR16 s)
{
    Py_ssize_t i;

    for (i = 0; s[i]; ++i) {}
    return PyUnicode_DecodeUTF16((char *)s, i << 1, NULL, NULL);
}

/* Translates a group index into a group name (unicode).
 * If not found, returns the default object.  Returns new reference.
 */
static PyObject *
group_name_by_index(pcre16 *code, int index, PyObject *def)
{
    int i, count, size;
    PCRE_SPTR16 table;

    if (pcre16_fullinfo(code, NULL, PCRE_INFO_NAMECOUNT, &count) == 0
            && pcre16_fullinfo(code, NULL, PCRE_INFO_NAMEENTRYSIZE, &size) == 0
            && pcre16_fullinfo(code, NULL, PCRE_INFO_NAMETABLE, &table) == 0)
    {
        for (i = 0; i < count; ++i) {
            if (table[0] == index)
                return from_utf16(table + 1);
            table += size;
        }
    }

    Py_INCREF(def);
    return def;
}

static PyObject *
set_pcre_error(int rc)
{
    if (rc == PCRE_ERROR_NOMEMORY)
        PyErr_NoMemory();
    else {
        PyObject *v = PyInt_FromLong(rc);
        if (v) {
            PyErr_SetObject(PyExc_PCREError, v);
            Py_DECREF(v);
        }
    }
    return NULL;
}

/*
 * Match
 */

/* Slices the subject string using indexes from given group.
 * If group has no match, returns the default object.  Returns new reference.
 */
static PyObject *
getsubstr(PyMatchObject *op, Py_ssize_t index, PyObject *def)
{
    int pos;

    if (index < 0 || index > op->pattern->groups) {
        PyErr_SetString(PyExc_IndexError, "no such group");
        return NULL;
    }

    pos = op->ovector[index * 2];
    if (pos < 0) {
        Py_INCREF(def);
        return def;
    }

    return PySequence_GetSlice(op->string, pos, op->ovector[index * 2 + 1]);
}

/* Same as getsubstr() but group index is specifed as an object.
 * This can be an int/long index or str/unicode group name.
 */
static PyObject *
getsubstro(PyMatchObject *op, PyObject *index, PyObject *def)
{
    Py_ssize_t i = -1;

    if (PyInt_Check(index) || PyLong_Check(index))
        i = PyInt_AsSsize_t(index);
    else {
        index = PyDict_GetItem(op->pattern->groupindex, index);
        if (index)
            return getsubstro(op, index, def);
    }

    return getsubstr(op, i, def);
}

static void
match_dealloc(PyMatchObject *self)
{
    Py_XDECREF(self->pattern);
    Py_XDECREF(self->string);
    pcre16_free(self->ovector);
    Py_TYPE(self)->tp_free(self);
}

static PyObject *
match_group(PyMatchObject *self, PyObject *args)
{
    PyObject *result;
    Py_ssize_t i, size;

    size = PyTuple_GET_SIZE(args);
    switch (size) {
        case 0: /* no args -- return the whole match */
            result = getsubstr(self, 0, Py_None);
            break;
        case 1: /* one arg -- return a single slice */
            result = getsubstro(self, PyTuple_GET_ITEM(args, 0), Py_None);
            break;
        default: /* more than one arg -- return a tuple of slices */
            result = PyTuple_New(size);
            if (result == NULL)
                return NULL;
            for (i = 0; i < size; ++i) {
                PyObject *item = getsubstro(self,
                        PyTuple_GET_ITEM(args, i), Py_None);
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
    int index = 0;

    if (!PyArg_ParseTuple(args, "|i:start", &index))
        return NULL;

    if (index < 0 || index > self->pattern->groups) {
        PyErr_SetString(PyExc_IndexError, "no such group");
        return NULL;
    }

    return PyInt_FromLong(self->ovector[index * 2]);
}

static PyObject *
match_end(PyMatchObject *self, PyObject *args)
{
    int index = 0;

    if (!PyArg_ParseTuple(args, "|i:end", &index))
        return NULL;

    if (index < 0 || index > self->pattern->groups) {
        PyErr_SetString(PyExc_IndexError, "no such group");
        return NULL;
    }

    return PyInt_FromLong(self->ovector[index * 2 + 1]);
}

static PyObject *
match_span(PyMatchObject *self, PyObject *args)
{
    int index = 0;

    if (!PyArg_ParseTuple(args, "|i:span", &index))
        return NULL;

    if (index < 0 || index > self->pattern->groups) {
        PyErr_SetString(PyExc_IndexError, "no such group");
        return NULL;
    }

    return Py_BuildValue("(ii)", self->ovector[index * 2],
            self->ovector[index * 2 + 1]);
}

static PyObject *
match_groups(PyMatchObject *self, PyObject *args)
{
    PyObject *result;
    PyObject *def = Py_None;
    Py_ssize_t index;

    if (!PyArg_ParseTuple(args, "|O:groups", &def))
        return NULL;

    result = PyTuple_New(self->pattern->groups);
    if (result == NULL)
        return NULL;

    for (index = 1; index <= self->pattern->groups; ++index) {
        PyObject *item = getsubstr(self, index, def);
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

    if (!PyArg_ParseTuple(args, "|O:groupdict", &def))
        return NULL;

    dict = PyDict_New();
    if (dict == NULL)
        return NULL;

    pos = 0;
    while (PyDict_Next(self->pattern->groupindex, &pos, &key, &value)) {
        value = getsubstro(self, value, def);
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
match_lastgroup_getter(PyMatchObject *self, void *closure)
{
    return group_name_by_index(self->pattern->code, self->lastindex, Py_None);
}

static const PyMethodDef match_methods[] = {
    {"group",       (PyCFunction)match_group,       METH_VARARGS},
    {"start",       (PyCFunction)match_start,       METH_VARARGS},
    {"end",         (PyCFunction)match_end,         METH_VARARGS},
    {"span",        (PyCFunction)match_span,        METH_VARARGS},
    {"groups",      (PyCFunction)match_groups,      METH_VARARGS},
    {"groupdict",   (PyCFunction)match_groupdict,   METH_VARARGS},
    {NULL}      // sentinel
};

static const PyGetSetDef match_getset[] = {
    {"lastgroup",   (getter)match_lastgroup_getter},
    {NULL}      // sentinel
};

static const PyMemberDef match_members[] = {
    {"string",      T_OBJECT,   offsetof(PyMatchObject, string),    READONLY},
    {"re",          T_OBJECT,   offsetof(PyMatchObject, pattern),   READONLY},
    {"pos",         T_INT,      offsetof(PyMatchObject, startpos),  READONLY},
    {"endpos",      T_INT,      offsetof(PyMatchObject, endpos),    READONLY},
    {"lastindex",   T_INT,      offsetof(PyMatchObject, lastindex), READONLY},
    {NULL}      // sentinel
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
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,     /* tp_flags */
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
    0,                                  /* tp_init */
    0,                                  /* tp_alloc */
    0,                                  /* tp_new */
    0,                                  /* tp_free */
};

/* Creates a new Match instance.  Takes ownership of ovector. */
static PyObject *
new_match(PyPatternObject *pattern, PyObject *string, int startpos, int endpos,
    int *ovector, int captures)
{
    PyTypeObject *type = &PyMatch_Type;
    PyMatchObject *self;
    PyObject *usertype;
    int rc;

    /* Get the Match type to use. */
    usertype = PyObject_GetAttrString((PyObject *)pattern, "_match_type");
    if (usertype) {
        /* Must inherit PyMatch_Type. */
        rc = PyObject_IsSubclass(usertype, (PyObject *)type);
        if (rc < 0) { /* check failed */
            Py_DECREF(usertype);
            return NULL;
        }
        if (!rc) { /* not a PyMatch_Type subtype */
            Py_DECREF(usertype);
            PyErr_SetString(PyExc_TypeError, "_match_class must be a Match subtype");
            return NULL;
        }
        type = (PyTypeObject *)usertype; /* override */
    }
    else if (PyErr_ExceptionMatches(PyExc_AttributeError))
        PyErr_Clear();
    else
        return NULL;

    self = (PyMatchObject *)type->tp_alloc(type, 0);
    if (self) {
        self->pattern = pattern;
        Py_INCREF(pattern);

        self->string = string;
        Py_INCREF(string);

        self->ovector = ovector;
        self->lastindex = captures - 1;
        self->startpos = startpos;
        self->endpos = endpos;

        /* Call __init__() with no args if defined by subtype. */
        if (type->tp_init) {
            PyObject *args = PyTuple_New(0);
            if (args) {
                rc = type->tp_init((PyObject *)self, args, NULL);
                Py_DECREF(args);
                if (rc < 0)
                    Py_CLEAR(self);
            }
            else
                Py_CLEAR(self);
        }
    }
    Py_XDECREF(usertype);
    return (PyObject *)self;
}

/*
 * Pattern
 */

/* Create a mapping from group names to group indexes. */
static PyObject *
make_groupindex(pcre16 *code)
{
    PyObject *dict;
    int rc, index, count, size;
    PCRE_SPTR16 table;
    PyObject *key, *value;

    dict = PyDict_New();
    if (dict == NULL)
        return NULL;

    if (pcre16_fullinfo(code, NULL, PCRE_INFO_NAMECOUNT, &count) != 0
            || pcre16_fullinfo(code, NULL, PCRE_INFO_NAMEENTRYSIZE, &size) != 0
            || pcre16_fullinfo(code, NULL, PCRE_INFO_NAMETABLE, &table) != 0)
        return dict;

    for (index = 0; index < count; ++index) {
        key = from_utf16(table + 1);
        if (key == NULL) {
            Py_DECREF(dict);
            return NULL;
        }
        value = PyInt_FromLong(table[0]);
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

static PyObject *
pattern_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *pattern, *utf16;
    int options = 0;
    const char *err = NULL;
    int rc;
    PyPatternObject *self;

    static const char *const kwlist[] = {"pattern", "flags", NULL};
    
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|i:__new__", (char **)kwlist,
            &pattern, &options))
        return NULL;

    /* If pattern is already an instance of this type, check its options.  If they
     * are the same as options requested for this pattern, simply return this instance
     * instead of creating a new one.  If options differ, extract the regex string
     * and create a new instance using this string and requested options.
     */
    if (Py_TYPE(pattern) == type) {
        self = (PyPatternObject *)pattern;
        if (self->options == options) {
            Py_INCREF(pattern);
            return pattern;
        }
        pattern = self->pattern;
    }

    utf16 = as_utf16(pattern);
    if (utf16 == NULL)
        return NULL;

    self = (PyPatternObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        Py_DECREF(utf16);
        return NULL;
    }

    self->pattern = pattern;
    Py_INCREF(pattern);

    /* Compile the regex. */
    self->code = pcre16_compile((PCRE_SPTR16)PyString_AS_STRING(utf16) + 1, options,
            &err, &rc, NULL);

    Py_DECREF(utf16);

    if (self->code == NULL) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_PCREError, err);
        return NULL;
    }
    
    /* get effective options and number of capturing groups */
    rc = pcre16_fullinfo(self->code, NULL, PCRE_INFO_OPTIONS, &self->options);
    if (rc == 0)
        rc = pcre16_fullinfo(self->code, NULL, PCRE_INFO_CAPTURECOUNT, &self->groups);
    if (rc != 0) {
        Py_DECREF(self);
        return set_pcre_error(rc);
    }

    /* create a dict mapping named group names to their indexes */
    self->groupindex = make_groupindex(self->code);
    if (self->groupindex == NULL) {
        Py_DECREF(self);
        return NULL;
    }

    return (PyObject *)self;
}

static void
pattern_dealloc(PyPatternObject *self)
{
    Py_XDECREF(self->groupindex);
    Py_XDECREF(self->pattern);
    pcre16_free(self->code);
    Py_TYPE(self)->tp_free(self);
}

/* Pattern.__call__ function performing the search.
 * Additional flags argument lets PCRE specific options to be set.
 */
static PyObject *
pattern_call(PyPatternObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *string;
    PyObject *utf16;
    int *ovector;
    int ovecsize;
    int length;
    int rc;
    int pos = -1;
    int endpos = -1;
    int options = 0;

    static const char *const kwlist[] = {"string", "pos", "endpos", "flags", NULL};
    
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|iii:__call__", (char **)kwlist,
            &string, &pos, &endpos, &options))
        return NULL;

    length = PySequence_Length(string);
    if (length < 0)
        return NULL;
    if (pos < 0)
        pos = 0;
    if (endpos < 0)
        endpos = length;
    if (pos > endpos || endpos > length)
        Py_RETURN_NONE;

    utf16 = as_utf16(string);
    if (utf16 == NULL)
        return NULL;

    ovecsize = (self->groups + 1) * 6;
    ovector = pcre16_malloc(ovecsize * sizeof(int));
    if (ovector == NULL) {
        Py_DECREF(utf16);
        PyErr_NoMemory();
        return NULL;
    }

    /* Perform the search. */
    rc = pcre16_exec(self->code, NULL, (PCRE_SPTR16)PyString_AS_STRING(utf16) + 1,
            endpos, pos, options, ovector, ovecsize);

    Py_DECREF(utf16);

    if (rc > 0) {
        PyObject *match = new_match(self, string, pos, endpos, ovector, rc);
        if (match == NULL)
            pcre16_free(ovector);
        return match;
    }

    pcre16_free(ovector);

    if (rc == PCRE_ERROR_NOMATCH)
        Py_RETURN_NONE;
    else if (rc == 0)
        PyErr_SetString(PyExc_RuntimeError, "vector overflow");
    else
        set_pcre_error(rc);
    return NULL;
}

/* Dumps the compiled regex into a string. */
static PyObject *
pattern_dumps(PyPatternObject *self)
{
    size_t size;
    int rc;

    rc = pcre16_fullinfo(self->code, NULL, PCRE_INFO_SIZE, &size);
    if (rc != 0)
        return set_pcre_error(rc);
    return PyString_FromStringAndSize((char *)self->code, size);
}

/* Loads a compiled regex from a string. */
static PyObject *
pattern_loads(PyTypeObject *type, PyObject *args)
{
    char *data;
    Py_ssize_t length;
    PyPatternObject *self;
    int rc;

    if (!PyArg_ParseTuple(args, "s#:loads", &data, &length))
        return NULL;

    self = (PyPatternObject *)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;

    self->code = pcre16_malloc(length);
    if (self->code == NULL) {
        Py_DECREF(self);
        PyErr_NoMemory();
        return NULL;
    }

    /* Copy the regex. */
    memcpy(self->code, data, length);

    /* Pattern string is not available so use None. */
    self->pattern = Py_None;
    Py_INCREF(Py_None);

    /* get options and number of capturing groups */
    rc = pcre16_fullinfo(self->code, NULL, PCRE_INFO_OPTIONS, &self->options);
    if (rc == 0)
        rc = pcre16_fullinfo(self->code, NULL, PCRE_INFO_CAPTURECOUNT, &self->groups);
    if (rc != 0) {
        Py_DECREF(self);
        return set_pcre_error(rc);
    }

    /* create a dict mapping named group names to their indexes */
    self->groupindex = make_groupindex(self->code);
    if (self->groupindex == NULL) {
        Py_DECREF(self);
        return NULL;
    }

    return (PyObject *)self;
}

static const PyMethodDef pattern_methods[] = {
    {"dumps",   (PyCFunction)pattern_dumps,     METH_NOARGS},
    {"loads",   (PyCFunction)pattern_loads,     METH_CLASS|METH_VARARGS},
    {NULL}      // sentinel
};

static const PyMemberDef pattern_members[] = {
    {"pattern",     T_OBJECT,   offsetof(PyPatternObject, pattern),     READONLY},
    {"flags",       T_INT,      offsetof(PyPatternObject, options),     READONLY},
    {"groups",      T_INT,      offsetof(PyPatternObject, groups),      READONLY},
    {"groupindex",  T_OBJECT,   offsetof(PyPatternObject, groupindex),  READONLY},
    {NULL}      // sentinel
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
    (ternaryfunc)pattern_call,          /* tp_call */
    0,                                  /* tp_str */
    0,                                  /* tp_getattro */
    0,                                  /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,     /* tp_flags */
    0,                                  /* tp_doc */
    0,                                  /* tp_traverse */
    0,                                  /* tp_clear */
    0,                                  /* tp_richcompare */
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
    0,                                  /* tp_init */
    0,                                  /* tp_alloc */
    pattern_new,                        /* tp_new */
    0,                                  /* tp_free */
};

/*
 * module
 */

static PyObject *
version(PyObject *self)
{
    return PyString_FromString(pcre16_version());
}

static const PyMethodDef methods[] = {
    {"version", (PyCFunction)version, METH_NOARGS},
    {NULL}          /* sentinel */
};

PyMODINIT_FUNC
init_pcre(void)
{
    PyObject *mod = Py_InitModule("_pcre", (PyMethodDef *)methods);

    /* Pattern */
    PyType_Ready(&PyPattern_Type);
    Py_INCREF(&PyPattern_Type);
    PyModule_AddObject(mod, "Pattern", (PyObject *)&PyPattern_Type);

    /* Match */
    PyType_Ready(&PyMatch_Type);
    Py_INCREF(&PyMatch_Type);
    PyModule_AddObject(mod, "Match", (PyObject *)&PyMatch_Type);

    /* PCREError */
    PyExc_PCREError = PyErr_NewException("_pcre.PCREError",
            PyExc_Exception, NULL);
    Py_INCREF(PyExc_PCREError);
    PyModule_AddObject(mod, "PCREError", PyExc_PCREError);

    /* flags */
    PyModule_AddIntConstant(mod, "IGNORECASE", PCRE_CASELESS);
    PyModule_AddIntConstant(mod, "MULTILINE", PCRE_MULTILINE);
    PyModule_AddIntConstant(mod, "DOTALL", PCRE_DOTALL);
    PyModule_AddIntConstant(mod, "UNICODE", PCRE_UCP);
    PyModule_AddIntConstant(mod, "ANCHORED", PCRE_ANCHORED);
}
