/* PCRE - Perl-Compatible Regular Expressions
 */

#include <Python.h>
#include <structmember.h>
#include "pcre.h"

static PyObject *PyExc_PCREError;

typedef struct {
	PyObject_HEAD
	PyObject *pattern; /* as passed in */
	pcre16 *code;
	int options;
	int groups; /* capture count */
	PyObject *groupindex; /* name->index */
} PyPatternObject;

typedef struct {
	PyObject_HEAD
	PyPatternObject *pattern;
	PyObject *string; /* as passed in */
	int *ovector;
	int lastindex;
	int startpos;
	int endpos;
} PyMatchObject;

static PyObject *
as_unicode(PyObject *op)
{
	if (PyUnicode_Check(op)) {
		Py_INCREF(op);
		return op;
	}

	if (PyString_Check(op))
		return PyUnicode_DecodeLatin1(PyString_AS_STRING(op),
				PyString_GET_SIZE(op), NULL);

	PyErr_SetString(PyExc_TypeError, "unicode or str expected");
	return NULL;
}

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
				return PyUnicode_FromUnicode(table + 1, wcslen(table + 1));
			table += size;
		}
	}

	Py_INCREF(def);
	return def;
}

/*
 * Match
 */

static void
match_dealloc(PyMatchObject *self)
{
	Py_XDECREF(self->pattern);
	Py_XDECREF(self->string);
	pcre16_free(self->ovector);
	Py_TYPE(self)->tp_free(self);
}

static PyObject *
match_getsubstr(PyMatchObject *self, Py_ssize_t index, PyObject *def)
{
	int pos;

	if (index < 0 || index > self->pattern->groups) {
		PyErr_SetString(PyExc_IndexError, "no such group");
		return NULL;
	}

	pos = self->ovector[index * 2];
	if (pos < 0) {
		Py_INCREF(def);
		return def;
	}

	return PySequence_GetSlice(self->string, pos, self->ovector[index * 2 + 1]);
}

static PyObject *
match_getsubstr_o(PyMatchObject *self, PyObject *index, PyObject *def)
{
	Py_ssize_t i = -1;

	if (PyInt_Check(index) || PyLong_Check(index))
		i = PyInt_AsSsize_t(index);
	else if (self->pattern->groupindex) {
		index = PyDict_GetItem(self->pattern->groupindex, index);
		if (index)
			return match_getsubstr_o(self, index, def);
	}

	return match_getsubstr(self, i, def);
}

static PyObject *
match_group(PyMatchObject *self, PyObject *args)
{
	PyObject *result;
	Py_ssize_t i, size;

	size = PyTuple_GET_SIZE(args);
	switch (size) {
		case 0:
			result = match_getsubstr_o(self, Py_False, Py_None);
			break;
		case 1:
			result = match_getsubstr_o(self, PyTuple_GET_ITEM(args, 0), Py_None);
			break;
		default:
			result = PyTuple_New(size);
			if (result == NULL)
				return NULL;
			for (i = 0; i < size; ++i) {
				PyObject *item = match_getsubstr_o(self,
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
		PyObject *item = match_getsubstr(self, index, def);
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

	if (self->pattern->groupindex) {
		pos = 0;
		while (PyDict_Next(self->pattern->groupindex, &pos, &key, &value)) {
			value = match_getsubstr_o(self, value, def);
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
	}

	return dict;
}

static PyObject *
match_lastgroup_getter(PyMatchObject *self, void *closure)
{
	return group_name_by_index(self->pattern->code, self->lastindex, Py_None);
}

static const PyMethodDef match_methods[] = {
	{"group",		(PyCFunction)match_group,		METH_VARARGS},
	{"start",		(PyCFunction)match_start,		METH_VARARGS},
	{"end",			(PyCFunction)match_end,			METH_VARARGS},
	{"span",		(PyCFunction)match_span,		METH_VARARGS},
	{"groups",		(PyCFunction)match_groups,		METH_VARARGS},
	{"groupdict",	(PyCFunction)match_groupdict,	METH_VARARGS},
	{NULL}		// sentinel
};

static const PyGetSetDef match_getset[] = {
	{"lastgroup",	(getter)match_lastgroup_getter},
	{NULL}		// sentinel
};

static const PyMemberDef match_members[] = {
	{"string",		T_OBJECT,	offsetof(PyMatchObject, string),	READONLY},
	{"re",			T_OBJECT,	offsetof(PyMatchObject, pattern),	READONLY},
	{"pos",			T_INT,		offsetof(PyMatchObject, startpos),	READONLY},
	{"endpos",		T_INT,		offsetof(PyMatchObject, endpos),	READONLY},
	{"lastindex",	T_INT,		offsetof(PyMatchObject, lastindex),	READONLY},
	{NULL}		// sentinel
};

static PyTypeObject PyMatch_Type = {
	PyObject_HEAD_INIT(NULL)
	0,									/* ob_size */
	"_pcre.Match",						/* tp_name */
	sizeof(PyMatchObject),				/* tp_basicsize */
	0,									/* tp_itemsize */
	(destructor)match_dealloc,			/* tp_dealloc */
	0,									/* tp_print */
	0,									/* tp_getattr */
	0,									/* tp_setattr */
	0,									/* tp_compare */
	0,									/* tp_repr */
	0,									/* tp_as_number */
	0,									/* tp_as_sequence */
	0,									/* tp_as_mapping */
	0,									/* tp_hash */
	0,									/* tp_call */
	0,									/* tp_str */
	0,									/* tp_getattro */
	0,									/* tp_setattro */
	0,									/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,		/* tp_flags */
	0,									/* tp_doc */
	0,									/* tp_traverse */
	0,									/* tp_clear */
	0,									/* tp_richcompare */
	0,									/* tp_weaklistoffset */
	0,									/* tp_iter */
	0,									/* tp_iternext */
	(PyMethodDef *)match_methods,		/* tp_methods */
	(PyMemberDef *)match_members,		/* tp_members */
	(PyGetSetDef *)match_getset,		/* tp_getset */
	0,									/* tp_base */
	0,									/* tp_dict */
	0,									/* tp_descr_get */
	0,									/* tp_descr_set */
	0,									/* tp_dictoffset */
	0,									/* tp_init */
	0,									/* tp_alloc */
	0,									/* tp_new */
	0,									/* tp_free */
};

static PyObject *
new_match(PyPatternObject *pattern, PyObject *string, int startpos, int endpos,
	int *ovector, int captures)
{
	PyTypeObject *type = &PyMatch_Type;
	PyMatchObject *self;
	PyObject *usertype;
	int rc;

	/* Get the Match class to use. */
	usertype = PyObject_GetAttrString((PyObject *)pattern, "_match_class");
	if (usertype) {
		rc = PyObject_IsSubclass(usertype, (PyObject *)type);
		if (rc < 0) {
			Py_DECREF(usertype);
			return NULL;
		}
		if (rc)
			type = (PyTypeObject *)usertype;
		else {
			Py_DECREF(usertype);
			PyErr_SetString(PyExc_TypeError, "_match_class must be a Match subclass");
			return NULL;
		}
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

		/* Call __init__ if defined by subtype. */
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
		key = PyUnicode_FromUnicode(table + 1, wcslen(table + 1));
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
	PyObject *pattern, *ustr;
	int options = 0;
	const char *err = NULL;
	int erroffset;
	PyPatternObject *self;

	static const char *const kwlist[] = {"pattern", "flags", NULL};
	
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|i:__new__", (char **)kwlist,
			&pattern, &options))
		return NULL;

	if (PyObject_TypeCheck(pattern, type))
		pattern = ((PyPatternObject *)pattern)->pattern;

	ustr = as_unicode(pattern);
	if (ustr == NULL)
		return NULL;

	self = (PyPatternObject *)type->tp_alloc(type, 0);
	if (self == NULL) {
		Py_DECREF(ustr);
		return NULL;
	}

	self->pattern = pattern;
	Py_INCREF(pattern);

	self->code = pcre16_compile(PyUnicode_AS_UNICODE(ustr), options,
			&err, &erroffset, NULL);

	Py_DECREF(ustr);

	if (self->code == NULL) {
		Py_DECREF(self);
		PyErr_SetString(PyExc_PCREError, err);
		return NULL;
	}
	
	/* get some info */
	pcre16_fullinfo(self->code, NULL, PCRE_INFO_OPTIONS, &self->options);
	pcre16_fullinfo(self->code, NULL, PCRE_INFO_CAPTURECOUNT, &self->groups);

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

static PyObject *
pattern_call(PyPatternObject *self, PyObject *args, PyObject *kwds)
{
	PyObject *string;
	PyObject *ustr;
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

	ustr = as_unicode(string);
	if (ustr == NULL)
		return NULL;

	length = PyUnicode_GET_SIZE(ustr);
	if (pos < 0)
		pos = 0;
	if (endpos < 0)
		endpos = length;
	if (pos > endpos || endpos > length) {
		Py_DECREF(ustr);
		Py_RETURN_NONE;
	}

	ovecsize = (self->groups + 1) * 6;
	ovector = pcre16_malloc(ovecsize * sizeof(int));
	if (ovector == NULL) {
		Py_DECREF(ustr);
		PyErr_NoMemory();
		return NULL;
	}

	rc = pcre16_exec(self->code, NULL, PyUnicode_AS_UNICODE(ustr),
			endpos, pos, options, ovector, ovecsize);

	Py_DECREF(ustr);

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
	else if (rc == PCRE_ERROR_NOMEMORY)
		PyErr_NoMemory();
	else {
		PyObject *v = PyInt_FromLong(rc);
		if (v == NULL)
			return NULL;
		PyErr_SetObject(PyExc_PCREError, v);
		Py_DECREF(v);
	}
	return NULL;
}

static const PyMemberDef pattern_members[] = {
	{"pattern",		T_OBJECT,	offsetof(PyPatternObject, pattern),		READONLY},
	{"flags",		T_INT,		offsetof(PyPatternObject, options),		READONLY},
	{"groups",		T_INT,		offsetof(PyPatternObject, groups),		READONLY},
	{"groupindex",	T_OBJECT,	offsetof(PyPatternObject, groupindex),	READONLY},
	{NULL}		// sentinel
};

static PyTypeObject PyPattern_Type = {
	PyObject_HEAD_INIT(NULL)
	0,									/* ob_size */
	"_pcre.Pattern",					/* tp_name */
	sizeof(PyPatternObject),			/* tp_basicsize */
	0,									/* tp_itemsize */
	(destructor)pattern_dealloc,		/* tp_dealloc */
	0,									/* tp_print */
	0,									/* tp_getattr */
	0,									/* tp_setattr */
	0,									/* tp_compare */
	0,									/* tp_repr */
	0,									/* tp_as_number */
	0,									/* tp_as_sequence */
	0,									/* tp_as_mapping */
	0,									/* tp_hash */
	pattern_call,						/* tp_call */
	0,									/* tp_str */
	0,									/* tp_getattro */
	0,									/* tp_setattro */
	0,									/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,		/* tp_flags */
	0,									/* tp_doc */
	0,									/* tp_traverse */
	0,									/* tp_clear */
	0,									/* tp_richcompare */
	0,									/* tp_weaklistoffset */
	0,									/* tp_iter */
	0,									/* tp_iternext */
	0,									/* tp_methods */
	(PyMemberDef *)pattern_members,		/* tp_members */
	0,									/* tp_getset */
	0,									/* tp_base */
	0,									/* tp_dict */
	0,									/* tp_descr_get */
	0,									/* tp_descr_set */
	0,									/* tp_dictoffset */
	0,									/* tp_init */
	0,									/* tp_alloc */
	pattern_new,						/* tp_new */
	0,									/* tp_free */
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
	{NULL}			/* sentinel */
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
