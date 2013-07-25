/*
 * Liblvm -- Python interface to LVM2 API.
 *
 * Copyright (C) 2010, 2013 Red Hat, Inc. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Lars Sjostrom (lars sjostrom redhat com)
 *	    Andy Grover (agrover redhat com)
 *	    Tony Asleson (tasleson redhat com)
 */

#include <Python.h>
#include "lvm2app.h"

static lvm_t _libh;


typedef struct {
	PyObject_HEAD
	vg_t vg;		/* vg handle */
} vgobject;

typedef struct {
	PyObject_HEAD
	struct dm_list *pvslist;
} pvslistobject;

typedef struct {
	PyObject_HEAD
	lv_t lv;		/* lv handle */
	vgobject *parent_vgobj;
} lvobject;

typedef struct {
	PyObject_HEAD
	pv_t pv;		/* pv handle */
	vgobject *parent_vgobj;
	pvslistobject *parent_pvslistobj;
} pvobject;

typedef struct {
	PyObject_HEAD
	lvseg_t lv_seg;		/* lv segment handle */
	lvobject *parent_lvobj;
} lvsegobject;

typedef struct {
	PyObject_HEAD
	pvseg_t pv_seg;		/* pv segment handle */
	pvobject *parent_pvobj;
} pvsegobject;

static PyTypeObject _LibLVMvgType;
static PyTypeObject _LibLVMlvType;
static PyTypeObject _LibLVMpvlistType;
static PyTypeObject _LibLVMpvType;
static PyTypeObject _LibLVMlvsegType;
static PyTypeObject _LibLVMpvsegType;

static PyObject *_LibLVMError;

#define LVM_VALID() \
	do { \
		if (!_libh) { \
			PyErr_SetString(PyExc_UnboundLocalError, "LVM handle invalid"); \
			return NULL; \
		} \
	} while (0)

/**
 * Ensure that we initialize all the bits to a sane state.
 */
static pvobject *_create_py_pv(void)
{
	pvobject * pvobj = PyObject_New(pvobject, &_LibLVMpvType);

	if (pvobj) {
		pvobj->pv = NULL;
		pvobj->parent_vgobj = NULL;
		pvobj->parent_pvslistobj = NULL;
	}

	return pvobj;
}

static vgobject *_create_py_vg(void)
{
	vgobject *vgobj = PyObject_New(vgobject, &_LibLVMvgType);

	if (vgobj)
		vgobj->vg = NULL;

	return vgobj;
}

static PyObject *_liblvm_get_last_error(void)
{
	PyObject *info;

	LVM_VALID();

	if (!(info = PyTuple_New(2)))
		return NULL;

	PyTuple_SetItem(info, 0, PyInt_FromLong((long) lvm_errno(_libh)));
	PyTuple_SetItem(info, 1, PyString_FromString(lvm_errmsg(_libh)));

	return info;
}

static PyObject *_liblvm_library_get_version(void)
{
	LVM_VALID();

	return Py_BuildValue("s", lvm_library_get_version());
}

static const char _gc_doc[] = "Garbage collect the C library";

static PyObject *_liblvm_lvm_gc(void)
{
	LVM_VALID();

	lvm_quit(_libh);

	if (!(_libh = lvm_init(NULL))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_list_vg_names(void)
{
	struct dm_list *vgnames;
	struct lvm_str_list *strl;
	PyObject * pytuple;
	int i = 0;

	LVM_VALID();

	if (!(vgnames = lvm_list_vg_names(_libh))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	if (!(pytuple = PyTuple_New(dm_list_size(vgnames))))
		return NULL;

	dm_list_iterate_items(strl, vgnames) {
		PyTuple_SET_ITEM(pytuple, i, PyString_FromString(strl->str));
		i++;
	}

	return pytuple;
}

static PyObject *_liblvm_lvm_list_vg_uuids(void)
{
	struct dm_list *uuids;
	struct lvm_str_list *strl;
	PyObject * pytuple;
	int i = 0;

	LVM_VALID();

	if (!(uuids = lvm_list_vg_uuids(_libh))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	if (!(pytuple = PyTuple_New(dm_list_size(uuids))))
		return NULL;

	dm_list_iterate_items(strl, uuids) {
		PyTuple_SET_ITEM(pytuple, i, PyString_FromString(strl->str));
		i++;
	}

	return pytuple;
}

static PyObject *_liblvm_lvm_pvlist_get(pvslistobject *pvsobj)
{
	struct lvm_pv_list *pvl;
	PyObject * pytuple;
	pvobject * pvobj;
	int i = 0;

	/* unlike other LVM api calls, if there are no results, we get NULL */
	pvsobj->pvslist = lvm_list_pvs(_libh);

	if (!pvsobj->pvslist)
		return Py_BuildValue("()");

	if (!(pytuple = PyTuple_New(dm_list_size(pvsobj->pvslist))))
		return NULL;

	dm_list_iterate_items(pvl, pvsobj->pvslist) {
		/* Create and initialize the object */
		if (!(pvobj = _create_py_pv())) {
			Py_DECREF(pytuple);
			return NULL;
		}

		/* We don't have a parent vg object to be concerned about */
		pvobj->parent_vgobj = NULL;
		pvobj->parent_pvslistobj = pvsobj;
		Py_INCREF(pvobj->parent_pvslistobj);

		pvobj->pv = pvl->pv;
		PyTuple_SET_ITEM(pytuple, i, (PyObject *) pvobj);
		i++;
	}

	return pytuple;
}

static PyObject *_liblvm_lvm_pvlist_put(pvslistobject *self)
{
	if (self->pvslist) {
		if (lvm_list_pvs_free(self->pvslist)) {
			PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
			return NULL;
		}

		self->pvslist = NULL;
		Py_INCREF(Py_None);

		return Py_None;
	}

	return NULL;
}

static PyObject *_liblvm_pvlist_dealloc(pvslistobject *self)
{
	if (self->pvslist)
		_liblvm_lvm_pvlist_put(self);

	PyObject_Del(self);
	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_list_pvs(void)
{
	pvslistobject *pvslistobj;

	LVM_VALID();

	if (!(pvslistobj = PyObject_New(pvslistobject, &_LibLVMpvlistType)))
		return NULL;

	pvslistobj->pvslist = NULL;

	return (PyObject *)pvslistobj;
}

static PyObject *_liblvm_lvm_pv_remove(PyObject *self, PyObject *arg)
{
	const char *pv_name;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "s", &pv_name))
		return NULL;

	if (lvm_pv_remove(_libh, pv_name) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_pv_create(PyObject *self, PyObject *arg)
{
	const char *pv_name;
	unsigned long long size;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "sK", &pv_name, &size))
		return NULL;

	if (lvm_pv_create(_libh, pv_name, size) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_percent_to_float(PyObject *self, PyObject *arg)
{
	double converted;
	int percent;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "i", &percent))
		return NULL;

	converted = lvm_percent_to_float(percent);

	return Py_BuildValue("d", converted);
}

static PyObject *_liblvm_lvm_vgname_from_pvid(PyObject *self, PyObject *arg)
{
	const char *pvid;
	const char *vgname;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "s", &pvid))
		return NULL;

	if (!(vgname = lvm_vgname_from_pvid(_libh, pvid))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	return Py_BuildValue("s", vgname);
}

static PyObject *_liblvm_lvm_vgname_from_device(PyObject *self, PyObject *arg)
{
	const char *device;
	const char *vgname;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "s", &device))
		return NULL;

	if (!(vgname = lvm_vgname_from_device(_libh, device))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	return Py_BuildValue("s", vgname);
}


static PyObject *_liblvm_lvm_config_find_bool(PyObject *self, PyObject *arg)
{
	const char *config;
	int rval;
	PyObject *rc;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "s", &config))
		return NULL;

	if ((rval = lvm_config_find_bool(_libh, config, -10)) == -10) {
		/* Retrieving error information yields no error in this case */
		PyErr_Format(PyExc_ValueError, "config path not found");
		return NULL;
	}

	rc = (rval) ? Py_True: Py_False;

	Py_INCREF(rc);

	return rc;
}

static PyObject *_liblvm_lvm_config_reload(void)
{
	LVM_VALID();

	if (lvm_config_reload(_libh) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}


static PyObject *_liblvm_lvm_scan(void)
{
	LVM_VALID();

	if (lvm_scan(_libh) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_config_override(PyObject *self, PyObject *arg)
{
	const char *config;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "s", &config))
		return NULL;

	if (lvm_config_override(_libh, config) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}
/* ----------------------------------------------------------------------
 * VG object initialization/deallocation
 */


static PyObject *_liblvm_lvm_vg_open(PyObject *self, PyObject *args)
{
	const char *vgname;
	const char *mode = NULL;

	vgobject *vgobj;

	LVM_VALID();

	if (!PyArg_ParseTuple(args, "s|s", &vgname, &mode))
		return NULL;

	if (mode == NULL)
		mode = "r";

	if (!(vgobj = _create_py_vg()))
		return NULL;

	if (!(vgobj->vg = lvm_vg_open(_libh, vgname, mode, 0))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		Py_DECREF(vgobj);
		return NULL;
	}

	return (PyObject *)vgobj;
}

static PyObject *_liblvm_lvm_vg_create(PyObject *self, PyObject *args)
{
	const char *vgname;
	vgobject *vgobj;

	LVM_VALID();

	if (!PyArg_ParseTuple(args, "s", &vgname))
		return NULL;

	if (!(vgobj = _create_py_vg()))
		return NULL;

	if (!(vgobj->vg = lvm_vg_create(_libh, vgname))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		Py_DECREF(vgobj);
		return NULL;
	}

	return (PyObject *)vgobj;
}

static void liblvm_vg_dealloc(vgobject *self)
{
	/* if already closed, don't reclose it */
	if (self->vg != NULL) {
		lvm_vg_close(self->vg);
		self->vg = NULL;
	}

	PyObject_Del(self);
}

/* VG Methods */

#define VG_VALID(vgobject) \
	do { \
		LVM_VALID(); \
		if (!vgobject->vg) { \
			PyErr_SetString(PyExc_UnboundLocalError, "VG object invalid"); \
			return NULL; \
		} \
	} while (0)

#define PVSLIST_VALID(pvslistobject) \
	do { \
		LVM_VALID(); \
		if (!pvslistobject->pvslist) { \
			PyErr_SetString(PyExc_UnboundLocalError, "PVS object invalid"); \
			return NULL; \
		} \
	} while (0)

static PyObject *_liblvm_lvm_vg_close(vgobject *self)
{
	/* if already closed, don't reclose it */
	if (self->vg) {
		lvm_vg_close(self->vg);
		self->vg = NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_vg_get_name(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("s", lvm_vg_get_name(self->vg));
}


static PyObject *_liblvm_lvm_vg_get_uuid(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("s", lvm_vg_get_uuid(self->vg));
}

static PyObject *_liblvm_lvm_vg_remove(vgobject *self)
{
	VG_VALID(self);

	if (lvm_vg_remove(self->vg) == -1)
		goto error;

	if (lvm_vg_write(self->vg) == -1)
		goto error;

	/* Not much you can do with a vg that is removed so close it */
	if (lvm_vg_close(self->vg) == -1)
		goto error;

	self->vg = NULL;

	Py_INCREF(Py_None);

	return Py_None;

error:
	PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());

	return NULL;
}

static PyObject *_liblvm_lvm_vg_extend(vgobject *self, PyObject *args)
{
	const char *device;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &device)) {
		return NULL;
	}

	if (lvm_vg_extend(self->vg, device) == -1)
		goto error;

	if (lvm_vg_write(self->vg) == -1)
		goto error;

	Py_INCREF(Py_None);
	return Py_None;

error:
	PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());

	return NULL;
}

static PyObject *_liblvm_lvm_vg_reduce(vgobject *self, PyObject *args)
{
	const char *device;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &device))
		return NULL;

	if (lvm_vg_reduce(self->vg, device) == -1)
		goto error;

	if (lvm_vg_write(self->vg) == -1)
		goto error;

	Py_INCREF(Py_None);

	return Py_None;

error:
	PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());

	return NULL;
}

static PyObject *_liblvm_lvm_vg_add_tag(vgobject *self, PyObject *args)
{
	const char *tag;
	int rval;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &tag)) {
		return NULL;
	}
	if ((rval = lvm_vg_add_tag(self->vg, tag)) == -1)
		goto error;

	if (lvm_vg_write(self->vg) == -1)
		goto error;

	return Py_BuildValue("i", rval);

error:
	PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());

	return NULL;
}

static PyObject *_liblvm_lvm_vg_remove_tag(vgobject *self, PyObject *args)
{
	const char *tag;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &tag))
		return NULL;

	if (lvm_vg_remove_tag(self->vg, tag) == -1)
		goto error;

	if (lvm_vg_write(self->vg) == -1)
		goto error;

	Py_INCREF(Py_None);

	return Py_None;

error:
	PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());

	return NULL;
}

static PyObject *_liblvm_lvm_vg_is_clustered(vgobject *self)
{
	PyObject *rval;

	VG_VALID(self);

	rval = ( lvm_vg_is_clustered(self->vg) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);

	return rval;
}

static PyObject *_liblvm_lvm_vg_is_exported(vgobject *self)
{
	PyObject *rval;

	VG_VALID(self);

	rval = ( lvm_vg_is_exported(self->vg) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);

	return rval;
}

static PyObject *_liblvm_lvm_vg_is_partial(vgobject *self)
{
	PyObject *rval;

	VG_VALID(self);

	rval = ( lvm_vg_is_partial(self->vg) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);

	return rval;
}

static PyObject *_liblvm_lvm_vg_get_seqno(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_vg_get_seqno(self->vg));
}

static PyObject *_liblvm_lvm_vg_get_size(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_vg_get_size(self->vg));
}

static PyObject *_liblvm_lvm_vg_get_free_size(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_vg_get_free_size(self->vg));
}

static PyObject *_liblvm_lvm_vg_get_extent_size(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_vg_get_extent_size(self->vg));
}

static PyObject *_liblvm_lvm_vg_get_extent_count(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_vg_get_extent_count(self->vg));
}

static PyObject *_liblvm_lvm_vg_get_free_extent_count(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_vg_get_free_extent_count(self->vg));
}

/* Builds a python tuple ([string|number], bool) from a struct lvm_property_value */
static PyObject *get_property(struct lvm_property_value *prop)
{
	PyObject *pytuple;
	PyObject *setable;

	if (!prop->is_valid) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	if (!(pytuple = PyTuple_New(2)))
		return NULL;

	if (prop->is_integer)
		PyTuple_SET_ITEM(pytuple, 0, Py_BuildValue("K", prop->value.integer));
	else
		PyTuple_SET_ITEM(pytuple, 0, PyString_FromString(prop->value.string));

	if (prop->is_settable)
		setable = Py_True;
	else
		setable = Py_False;

	Py_INCREF(setable);
	PyTuple_SET_ITEM(pytuple, 1, setable);

	return pytuple;
}

/* This will return a tuple of (value, bool) with the value being a string or
   integer and bool indicating if property is settable */
static PyObject *_liblvm_lvm_vg_get_property(vgobject *self, PyObject *args)
{
	const char *name;
	struct lvm_property_value prop_value;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;

	prop_value = lvm_vg_get_property(self->vg, name);

	return get_property(&prop_value);
}

static PyObject *_liblvm_lvm_vg_set_property(vgobject *self, PyObject *args)
{
	const char *property_name = NULL;
	PyObject *variant_type_arg = NULL;
	struct lvm_property_value lvm_property;
	char *string_value = NULL;
	int temp_py_int;
	unsigned long long temp_py_long;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "sO", &property_name, &variant_type_arg))
		return NULL;

	lvm_property = lvm_vg_get_property(self->vg, property_name);
	if (!lvm_property.is_valid)
		goto lvmerror;

	if (PyObject_IsInstance(variant_type_arg, (PyObject*)&PyString_Type)) {

		if (!lvm_property.is_string) {
			PyErr_Format(PyExc_ValueError, "Property requires string value");
			goto bail;
		}

		if (!(string_value = PyString_AsString(variant_type_arg))) {
			PyErr_NoMemory();
			goto bail;
		}

		lvm_property.value.string = string_value;
	} else {

		if (!lvm_property.is_integer) {
			PyErr_Format(PyExc_ValueError, "Property requires numeric value");
			goto bail;
		}

		if (PyObject_IsInstance(variant_type_arg, (PyObject*)&PyInt_Type)) {
			temp_py_int = PyInt_AsLong(variant_type_arg);

			/* -1 could be valid, need to see if an exception was gen. */
			if (temp_py_int == -1 && PyErr_Occurred())
				goto bail;

			if (temp_py_int < 0) {
				PyErr_Format(PyExc_ValueError, "Positive integers only!");
				goto bail;
			}

			lvm_property.value.integer = temp_py_int;
		} else if (PyObject_IsInstance(variant_type_arg, (PyObject*)&PyLong_Type)){
			/* If PyLong_AsUnsignedLongLong function fails an OverflowError is
			 * raised and (unsigned long long)-1 is returned
			 */
			if ((temp_py_long = PyLong_AsUnsignedLongLong(variant_type_arg)) == ~0ULL)
				goto bail;

			lvm_property.value.integer = temp_py_long;
		} else {
			PyErr_Format(PyExc_ValueError, "supported value types are numeric and string");
			goto bail;
		}
	}

	if (lvm_vg_set_property(self->vg, property_name, &lvm_property) == -1)
		goto lvmerror;

	if (lvm_vg_write(self->vg) == -1)
		goto lvmerror;

	Py_INCREF(Py_None);

	return Py_None;

lvmerror:
	PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
bail:
	return NULL;
}

static PyObject *_liblvm_lvm_vg_get_pv_count(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_vg_get_pv_count(self->vg));
}

static PyObject *_liblvm_lvm_vg_get_max_pv(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_vg_get_max_pv(self->vg));
}

static PyObject *_liblvm_lvm_vg_get_max_lv(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_vg_get_max_lv(self->vg));
}

static PyObject *_liblvm_lvm_vg_set_extent_size(vgobject *self, PyObject *args)
{
	unsigned int new_size;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "I", &new_size))
		return NULL;

	if (lvm_vg_set_extent_size(self->vg, new_size) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_vg_list_lvs(vgobject *self)
{
	struct dm_list *lvs;
	struct lvm_lv_list *lvl;
	PyObject * pytuple;
	lvobject * lvobj;
	int i = 0;

	VG_VALID(self);

	/* unlike other LVM api calls, if there are no results, we get NULL */
	if (!(lvs = lvm_vg_list_lvs(self->vg)))
		return Py_BuildValue("()");

	if (!(pytuple = PyTuple_New(dm_list_size(lvs))))
		return NULL;

	dm_list_iterate_items(lvl, lvs) {
		/* Create and initialize the object */
		if (!(lvobj = PyObject_New(lvobject, &_LibLVMlvType))) {
			Py_DECREF(pytuple);
			return NULL;
		}

		lvobj->parent_vgobj = self;
		Py_INCREF(lvobj->parent_vgobj);

		lvobj->lv = lvl->lv;
		PyTuple_SET_ITEM(pytuple, i, (PyObject *) lvobj);
		i++;
	}

	return pytuple;
}

static PyObject *_liblvm_lvm_vg_get_tags(vgobject *self)
{
	struct dm_list *tags;
	struct lvm_str_list *strl;
	PyObject * pytuple;
	int i = 0;

	VG_VALID(self);

	if (!(tags = lvm_vg_get_tags(self->vg))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	if (!(pytuple = PyTuple_New(dm_list_size(tags))))
		return NULL;

	dm_list_iterate_items(strl, tags) {
		PyTuple_SET_ITEM(pytuple, i, PyString_FromString(strl->str));
		i++;
	}

	return pytuple;
}

static PyObject *_liblvm_lvm_vg_create_lv_linear(vgobject *self, PyObject *args)
{
	const char *vgname;
	unsigned long long size;
	lvobject *lvobj;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "sK", &vgname, &size))
		return NULL;

	if (!(lvobj = PyObject_New(lvobject, &_LibLVMlvType)))
		return NULL;

	/* Initialize the parent ptr in case lv create fails and we dealloc lvobj */
	lvobj->parent_vgobj = NULL;

	if (!(lvobj->lv = lvm_vg_create_lv_linear(self->vg, vgname, size))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		Py_DECREF(lvobj);
		return NULL;
	}

	lvobj->parent_vgobj = self;
	Py_INCREF(lvobj->parent_vgobj);

	return (PyObject *)lvobj;
}

static PyObject *_liblvm_lvm_vg_create_lv_thinpool(vgobject *self, PyObject *args)
{
	unsigned long long size = 0;
	unsigned long long meta_size = 0;
	const char *pool_name;
	unsigned long chunk_size = 0;
	int skip_zero = 0;
	lvm_thin_discards_t discard = LVM_THIN_DISCARDS_PASSDOWN;
	lvobject *lvobj;
	lv_create_params_t lvp = NULL;
	struct lvm_property_value prop_value;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "sK|kKii", &pool_name, &size, &chunk_size,
			      &meta_size, &discard, &skip_zero))
		return NULL;

	if (!(lvobj = PyObject_New(lvobject, &_LibLVMlvType)))
		return NULL;

	/* Initialize the parent ptr in case lv create fails and we dealloc lvobj */
	lvobj->parent_vgobj = NULL;

	if (!(lvp = lvm_lv_params_create_thin_pool(self->vg, pool_name, size, chunk_size,
						   meta_size, discard))) {

		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		Py_DECREF(lvobj);
		return NULL;
	}

	if (skip_zero) {
		prop_value = lvm_lv_params_get_property(lvp, "skip_zero");

		if (prop_value.is_valid) {
			prop_value.value.integer = 1;

			if (lvm_lv_params_set_property(lvp, "skip_zero",
						       &prop_value) == -1) {
				PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
				Py_DECREF(lvobj);
				return NULL;
			}
		}
	}

	if (!(lvobj->lv = lvm_lv_create(lvp))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		Py_DECREF(lvobj);
		return NULL;
	}

	lvobj->parent_vgobj = self;
	Py_INCREF(lvobj->parent_vgobj);

	return (PyObject *)lvobj;
}

static PyObject *_liblvm_lvm_vg_create_lv_thin(vgobject *self, PyObject *args)
{
	const char *pool_name;
	const char *lv_name;
	unsigned long long size = 0;
	lvobject *lvobj;
	lv_create_params_t lvp = NULL;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "ssK", &pool_name, &lv_name, &size))
		return NULL;

	if (!(lvobj = PyObject_New(lvobject, &_LibLVMlvType)))
		return NULL;

	/* Initialize the parent ptr in case lv create fails and we dealloc lvobj */
	lvobj->parent_vgobj = NULL;

	if (!(lvp = lvm_lv_params_create_thin(self->vg, pool_name, lv_name,size))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		Py_DECREF(lvobj);
		return NULL;
	}

	if (!(lvobj->lv = lvm_lv_create(lvp))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		Py_DECREF(lvobj);
		return NULL;
	}

	lvobj->parent_vgobj = self;
	Py_INCREF(lvobj->parent_vgobj);

	return (PyObject *)lvobj;
}

static void liblvm_lv_dealloc(lvobject *self)
{
	/* We can dealloc an object that didn't get fully created */
	if (self->parent_vgobj)
		Py_DECREF(self->parent_vgobj);

	PyObject_Del(self);
}

static PyObject *_liblvm_lvm_vg_list_pvs(vgobject *self)
{
	struct dm_list *pvs;
	struct lvm_pv_list *pvl;
	PyObject * pytuple;
	pvobject * pvobj;
	int i = 0;

	VG_VALID(self);

	/* unlike other LVM api calls, if there are no results, we get NULL */
	if (!(pvs = lvm_vg_list_pvs(self->vg)))
		return Py_BuildValue("()");

	if (!(pytuple = PyTuple_New(dm_list_size(pvs))))
		return NULL;

	dm_list_iterate_items(pvl, pvs) {
		/* Create and initialize the object */
		if (!(pvobj = _create_py_pv())) {
			Py_DECREF(pytuple);
			return NULL;
		}

		pvobj->parent_vgobj = self;
		Py_INCREF(pvobj->parent_vgobj);

		pvobj->pv = pvl->pv;
		PyTuple_SET_ITEM(pytuple, i, (PyObject *) pvobj);
		i++;
	}

	return pytuple;
}

typedef lv_t (*lv_fetch_by_N)(vg_t vg, const char *id);
typedef pv_t (*pv_fetch_by_N)(vg_t vg, const char *id);

static PyObject *_liblvm_lvm_lv_from_N(vgobject *self, PyObject *arg, lv_fetch_by_N method)
{
	const char *id;
	lvobject *lvobj;
	lv_t lv = NULL;

	VG_VALID(self);

	if (!PyArg_ParseTuple(arg, "s", &id))
		return NULL;

	if (!(lv = method(self->vg, id))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	if (!(lvobj = PyObject_New(lvobject, &_LibLVMlvType)))
		return NULL;

	lvobj->parent_vgobj = self;
	Py_INCREF(lvobj->parent_vgobj);

	lvobj->lv = lv;

	return (PyObject *)lvobj;
}

static PyObject *_liblvm_lvm_lv_from_name(vgobject *self, PyObject *arg)
{
	return _liblvm_lvm_lv_from_N(self, arg, lvm_lv_from_name);
}

static PyObject *_liblvm_lvm_lv_from_uuid(vgobject *self, PyObject *arg)
{
	return _liblvm_lvm_lv_from_N(self, arg, lvm_lv_from_uuid);
}

static PyObject *_liblvm_lvm_pv_from_N(vgobject *self, PyObject *arg, pv_fetch_by_N method)
{
	const char *id;
	pvobject *rc;
	pv_t pv = NULL;

	VG_VALID(self);

	if (!PyArg_ParseTuple(arg, "s", &id))
		return NULL;

	if (!(pv = method(self->vg, id))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	if (!(rc = _create_py_pv()))
		return NULL;

	Py_INCREF(self);
	rc->pv = pv;

	return (PyObject *)rc;
}

static PyObject *_liblvm_lvm_pv_from_name(vgobject *self, PyObject *arg)
{
	return _liblvm_lvm_pv_from_N(self, arg, lvm_pv_from_name);
}

static PyObject *_liblvm_lvm_pv_from_uuid(vgobject *self, PyObject *arg)
{
	return _liblvm_lvm_pv_from_N(self, arg, lvm_pv_from_uuid);
}

static void _liblvm_pv_dealloc(pvobject *self)
{
	if (self->parent_vgobj)
		Py_DECREF(self->parent_vgobj);

	if (self->parent_pvslistobj)
		Py_DECREF(self->parent_pvslistobj);

	self->parent_vgobj = NULL;
	self->parent_pvslistobj = NULL;
	PyObject_Del(self);
}

/* LV Methods */

#define LV_VALID(lvobject) \
	do { \
		VG_VALID(lvobject->parent_vgobj); \
		if (!lvobject->lv) { \
			PyErr_SetString(PyExc_UnboundLocalError, "LV object invalid"); \
			return NULL; \
		} \
	} while (0)

static PyObject *_liblvm_lvm_lv_get_attr(lvobject *self)
{
	LV_VALID(self);

	return Py_BuildValue("s", lvm_lv_get_attr(self->lv));
}

static PyObject *_liblvm_lvm_lv_get_origin(lvobject *self)
{
	LV_VALID(self);

	return Py_BuildValue("s", lvm_lv_get_origin(self->lv));
}

static PyObject *_liblvm_lvm_lv_get_name(lvobject *self)
{
	LV_VALID(self);

	return Py_BuildValue("s", lvm_lv_get_name(self->lv));
}

static PyObject *_liblvm_lvm_lv_get_uuid(lvobject *self)
{
	LV_VALID(self);

	return Py_BuildValue("s", lvm_lv_get_uuid(self->lv));
}

static PyObject *_liblvm_lvm_lv_activate(lvobject *self)
{
	LV_VALID(self);

	if (lvm_lv_activate(self->lv) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_lv_deactivate(lvobject *self)
{
	LV_VALID(self);

	if (lvm_lv_deactivate(self->lv) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_vg_remove_lv(lvobject *self)
{
	LV_VALID(self);

	if (lvm_vg_remove_lv(self->lv) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	self->lv = NULL;

	Py_INCREF(Py_None);

	return Py_None;
}

/* This will return a tuple of (value, bool) with the value being a string or
   integer and bool indicating if property is settable */
static PyObject * _liblvm_lvm_lv_get_property(lvobject *self, PyObject *args)
{
	const char *name;
	struct lvm_property_value prop_value;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;

	prop_value = lvm_lv_get_property(self->lv, name);

	return get_property(&prop_value);
}

static PyObject *_liblvm_lvm_lv_get_size(lvobject *self)
{
	LV_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_lv_get_size(self->lv));
}

static PyObject *_liblvm_lvm_lv_is_active(lvobject *self)
{
	PyObject *rval;

	LV_VALID(self);

	rval = (lvm_lv_is_active(self->lv) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);

	return rval;
}

static PyObject *_liblvm_lvm_lv_is_suspended(lvobject *self)
{
	PyObject *rval;

	LV_VALID(self);

	rval = (lvm_lv_is_suspended(self->lv) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);

	return rval;
}

static PyObject *_liblvm_lvm_lv_add_tag(lvobject *self, PyObject *args)
{
	const char *tag;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &tag))
		return NULL;

	if (lvm_lv_add_tag(self->lv, tag) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *_liblvm_lvm_lv_remove_tag(lvobject *self, PyObject *args)
{
	const char *tag;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &tag))
		return NULL;

	if (lvm_lv_remove_tag(self->lv, tag) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_lv_get_tags(lvobject *self)
{
	struct dm_list *tags;
	struct lvm_str_list *strl;
	PyObject * pytuple;
	int i = 0;

	LV_VALID(self);

	if (!(tags = lvm_lv_get_tags(self->lv))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	if (!(pytuple = PyTuple_New(dm_list_size(tags))))
		return NULL;

	dm_list_iterate_items(strl, tags) {
		PyTuple_SET_ITEM(pytuple, i, PyString_FromString(strl->str));
		i++;
	}

	return pytuple;
}

static PyObject *_liblvm_lvm_lv_rename(lvobject *self, PyObject *args)
{
	const char *new_name;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &new_name))
		return NULL;

	if (lvm_lv_rename(self->lv, new_name) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_lv_resize(lvobject *self, PyObject *args)
{
	unsigned long long new_size;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "K", &new_size))
		return NULL;

	if (lvm_lv_resize(self->lv, new_size) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_lv_list_lvsegs(lvobject *self)
{
	struct dm_list *lvsegs;
	lvseg_list_t *lvsegl;
	PyObject * pytuple;
	lvsegobject *lvsegobj;
	int i = 0;

	LV_VALID(self);

	if (!(lvsegs = lvm_lv_list_lvsegs(self->lv)))
		return Py_BuildValue("()");

	if (!(pytuple = PyTuple_New(dm_list_size(lvsegs))))
		return NULL;

	dm_list_iterate_items(lvsegl, lvsegs) {
		/* Create and initialize the object */
		if (!(lvsegobj = PyObject_New(lvsegobject, &_LibLVMlvsegType))) {
			Py_DECREF(pytuple);
			return NULL;
		}

		lvsegobj->parent_lvobj = self;
		Py_INCREF(lvsegobj->parent_lvobj);

		lvsegobj->lv_seg = lvsegl->lvseg;
		PyTuple_SET_ITEM(pytuple, i, (PyObject *) lvsegobj);
		i++;
	}

	return pytuple;
}

static PyObject *_liblvm_lvm_lv_snapshot(lvobject *self, PyObject *args)
{
	const char *snap_name;
	unsigned long long size = 0;
	lvobject *lvobj;
	lv_create_params_t lvp = NULL;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "s|K", &snap_name, &size))
		return NULL;

	if (!(lvobj = PyObject_New(lvobject, &_LibLVMlvType)))
		return NULL;

	lvobj->parent_vgobj = NULL;

	if (!(lvp = lvm_lv_params_create_snapshot(self->lv, snap_name, size))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		Py_DECREF(lvobj);
		return NULL;
	}

	if (!(lvobj->lv = lvm_lv_create(lvp))) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		Py_DECREF(lvobj);
		return NULL;
	}

	lvobj->parent_vgobj = self->parent_vgobj;
	Py_INCREF(lvobj->parent_vgobj);

	return (PyObject *)lvobj;
}

/* PV Methods */

#define PV_VALID(pvobject) \
	do { \
		if (pvobject->parent_vgobj) { \
			VG_VALID(pvobject->parent_vgobj); \
		} \
		if (pvobject->parent_pvslistobj) { \
			PVSLIST_VALID(pvobject->parent_pvslistobj); \
		} \
		if (!pvobject->pv) { \
			PyErr_SetString(PyExc_UnboundLocalError, "PV object invalid"); \
			return NULL; \
		} \
	} while (0)

static PyObject *_liblvm_lvm_pv_get_name(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("s", lvm_pv_get_name(self->pv));
}

static PyObject *_liblvm_lvm_pv_get_uuid(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("s", lvm_pv_get_uuid(self->pv));
}

static PyObject *_liblvm_lvm_pv_get_mda_count(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_pv_get_mda_count(self->pv));
}

static PyObject *_liblvm_lvm_pv_get_property(pvobject *self, PyObject *args)
{
	const char *name;
	struct lvm_property_value prop_value;

	PV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;

	prop_value = lvm_pv_get_property(self->pv, name);

	return get_property(&prop_value);
}

static PyObject *_liblvm_lvm_pv_get_dev_size(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_pv_get_dev_size(self->pv));
}

static PyObject *_liblvm_lvm_pv_get_size(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_pv_get_size(self->pv));
}

static PyObject *_liblvm_lvm_pv_get_free(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("K", (unsigned long long)lvm_pv_get_free(self->pv));
}

static PyObject *_liblvm_lvm_pv_resize(pvobject *self, PyObject *args)
{
	unsigned long long new_size;

	PV_VALID(self);

	if (!PyArg_ParseTuple(args, "K", &new_size))
		return NULL;

	if (lvm_pv_resize(self->pv, new_size) == -1) {
		PyErr_SetObject(_LibLVMError, _liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);

	return Py_None;
}

static PyObject *_liblvm_lvm_pv_list_pvsegs(pvobject *self)
{
	struct dm_list *pvsegs;
	pvseg_list_t *pvsegl;
	PyObject *pytuple;
	pvsegobject *pvsegobj;
	int i = 0;

	PV_VALID(self);

	if (!(pvsegs = lvm_pv_list_pvsegs(self->pv)))
		return Py_BuildValue("()");

	if (!(pytuple = PyTuple_New(dm_list_size(pvsegs))))
		return NULL;

	dm_list_iterate_items(pvsegl, pvsegs) {
		/* Create and initialize the object */
		if (!(pvsegobj = PyObject_New(pvsegobject, &_LibLVMpvsegType))) {
			Py_DECREF(pytuple);
			return NULL;
		}

		pvsegobj->parent_pvobj = self;
		Py_INCREF(pvsegobj->parent_pvobj);

		pvsegobj->pv_seg = pvsegl->pvseg;
		PyTuple_SET_ITEM(pytuple, i, (PyObject *) pvsegobj);
		i++;
	}

	return pytuple;
}

/* LV seg methods */

/*
 * No way to close/destroy an lvseg, just need to make sure parents are
 * still good
 */
#define LVSEG_VALID(lvsegobject) LV_VALID(lvsegobject->parent_lvobj)

static void _liblvm_lvseg_dealloc(lvsegobject *self)
{
	Py_DECREF(self->parent_lvobj);
	PyObject_Del(self);
}

static PyObject *_liblvm_lvm_lvseg_get_property(lvsegobject *self, PyObject *args)
{
	const char *name;
	struct lvm_property_value prop_value;

	LVSEG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;

	prop_value = lvm_lvseg_get_property(self->lv_seg, name);

	return get_property(&prop_value);
}

/* PV seg methods */

/*
 * No way to close/destroy a pvseg, just need to make sure parents are
 * still good
 */
#define PVSEG_VALID(pvsegobject) PV_VALID(pvsegobject->parent_pvobj)

static void _liblvm_pvseg_dealloc(pvsegobject *self)
{
	Py_DECREF(self->parent_pvobj);
	PyObject_Del(self);
}

static PyObject *_liblvm_lvm_pvseg_get_property(pvsegobject *self, PyObject *args)
{
	const char *name;
	struct lvm_property_value prop_value;

	PVSEG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;

	prop_value = lvm_pvseg_get_property(self->pv_seg, name);

	return get_property(&prop_value);
}

/* ----------------------------------------------------------------------
 * Method tables and other bureaucracy
 */

static PyMethodDef _Liblvm_methods[] = {
	/* LVM methods */
	{ "getVersion",		(PyCFunction)_liblvm_library_get_version, METH_NOARGS },
	{ "gc",			(PyCFunction)_liblvm_lvm_gc, METH_NOARGS, _gc_doc },
	{ "vgOpen",		(PyCFunction)_liblvm_lvm_vg_open, METH_VARARGS },
	{ "vgCreate",		(PyCFunction)_liblvm_lvm_vg_create, METH_VARARGS },
	{ "configFindBool",	(PyCFunction)_liblvm_lvm_config_find_bool, METH_VARARGS },
	{ "configReload",	(PyCFunction)_liblvm_lvm_config_reload, METH_NOARGS },
	{ "configOverride",	(PyCFunction)_liblvm_lvm_config_override, METH_VARARGS },
	{ "scan",		(PyCFunction)_liblvm_lvm_scan, METH_NOARGS },
	{ "listVgNames",	(PyCFunction)_liblvm_lvm_list_vg_names, METH_NOARGS },
	{ "listVgUuids",	(PyCFunction)_liblvm_lvm_list_vg_uuids, METH_NOARGS },
	{ "listPvs",		(PyCFunction)_liblvm_lvm_list_pvs, METH_NOARGS },
	{ "pvCreate",		(PyCFunction)_liblvm_lvm_pv_create, METH_VARARGS },
	{ "pvRemove",		(PyCFunction)_liblvm_lvm_pv_remove, METH_VARARGS },
	{ "percentToFloat",	(PyCFunction)_liblvm_lvm_percent_to_float, METH_VARARGS },
	{ "vgNameFromPvid",	(PyCFunction)_liblvm_lvm_vgname_from_pvid, METH_VARARGS },
	{ "vgNameFromDevice",	(PyCFunction)_liblvm_lvm_vgname_from_device, METH_VARARGS },
	{ NULL, NULL }		/* sentinel */
};

static PyMethodDef _liblvm_vg_methods[] = {
	/* vg methods */
	{ "getName",		(PyCFunction)_liblvm_lvm_vg_get_name, METH_NOARGS },
	{ "getUuid",		(PyCFunction)_liblvm_lvm_vg_get_uuid, METH_NOARGS },
	{ "close",		(PyCFunction)_liblvm_lvm_vg_close, METH_NOARGS },
	{ "remove",		(PyCFunction)_liblvm_lvm_vg_remove, METH_NOARGS },
	{ "extend",		(PyCFunction)_liblvm_lvm_vg_extend, METH_VARARGS },
	{ "reduce",		(PyCFunction)_liblvm_lvm_vg_reduce, METH_VARARGS },
	{ "addTag",		(PyCFunction)_liblvm_lvm_vg_add_tag, METH_VARARGS },
	{ "removeTag",		(PyCFunction)_liblvm_lvm_vg_remove_tag, METH_VARARGS },
	{ "setExtentSize",	(PyCFunction)_liblvm_lvm_vg_set_extent_size, METH_VARARGS },
	{ "isClustered",	(PyCFunction)_liblvm_lvm_vg_is_clustered, METH_NOARGS },
	{ "isExported",		(PyCFunction)_liblvm_lvm_vg_is_exported, METH_NOARGS },
	{ "isPartial",		(PyCFunction)_liblvm_lvm_vg_is_partial, METH_NOARGS },
	{ "getSeqno",		(PyCFunction)_liblvm_lvm_vg_get_seqno, METH_NOARGS },
	{ "getSize",		(PyCFunction)_liblvm_lvm_vg_get_size, METH_NOARGS },
	{ "getFreeSize",	(PyCFunction)_liblvm_lvm_vg_get_free_size, METH_NOARGS },
	{ "getExtentSize",	(PyCFunction)_liblvm_lvm_vg_get_extent_size, METH_NOARGS },
	{ "getExtentCount",	(PyCFunction)_liblvm_lvm_vg_get_extent_count, METH_NOARGS },
	{ "getFreeExtentCount",	(PyCFunction)_liblvm_lvm_vg_get_free_extent_count, METH_NOARGS },
	{ "getProperty",	(PyCFunction)_liblvm_lvm_vg_get_property, METH_VARARGS },
	{ "setProperty",	(PyCFunction)_liblvm_lvm_vg_set_property, METH_VARARGS },
	{ "getPvCount",		(PyCFunction)_liblvm_lvm_vg_get_pv_count, METH_NOARGS },
	{ "getMaxPv",		(PyCFunction)_liblvm_lvm_vg_get_max_pv, METH_NOARGS },
	{ "getMaxLv",		(PyCFunction)_liblvm_lvm_vg_get_max_lv, METH_NOARGS },
	{ "listLVs",		(PyCFunction)_liblvm_lvm_vg_list_lvs, METH_NOARGS },
	{ "listPVs",		(PyCFunction)_liblvm_lvm_vg_list_pvs, METH_NOARGS },
	{ "lvFromName", 	(PyCFunction)_liblvm_lvm_lv_from_name, METH_VARARGS },
	{ "lvFromUuid", 	(PyCFunction)_liblvm_lvm_lv_from_uuid, METH_VARARGS },
	{ "pvFromName", 	(PyCFunction)_liblvm_lvm_pv_from_name, METH_VARARGS },
	{ "pvFromUuid", 	(PyCFunction)_liblvm_lvm_pv_from_uuid, METH_VARARGS },
	{ "getTags",		(PyCFunction)_liblvm_lvm_vg_get_tags, METH_NOARGS },
	{ "createLvLinear",	(PyCFunction)_liblvm_lvm_vg_create_lv_linear, METH_VARARGS },
	{ "createLvThinpool",	(PyCFunction)_liblvm_lvm_vg_create_lv_thinpool, METH_VARARGS },
	{ "createLvThin", 	(PyCFunction)_liblvm_lvm_vg_create_lv_thin, METH_VARARGS },
	{ NULL, NULL }		/* sentinel */
};

static PyMethodDef _liblvm_lv_methods[] = {
	/* lv methods */
	{ "getAttr",		(PyCFunction)_liblvm_lvm_lv_get_attr, METH_NOARGS },
	{ "getName",		(PyCFunction)_liblvm_lvm_lv_get_name, METH_NOARGS },
	{ "getOrigin",		(PyCFunction)_liblvm_lvm_lv_get_origin, METH_NOARGS },
	{ "getUuid",		(PyCFunction)_liblvm_lvm_lv_get_uuid, METH_NOARGS },
	{ "activate",		(PyCFunction)_liblvm_lvm_lv_activate, METH_NOARGS },
	{ "deactivate",		(PyCFunction)_liblvm_lvm_lv_deactivate, METH_NOARGS },
	{ "remove",		(PyCFunction)_liblvm_lvm_vg_remove_lv, METH_NOARGS },
	{ "getProperty",	(PyCFunction)_liblvm_lvm_lv_get_property, METH_VARARGS },
	{ "getSize",		(PyCFunction)_liblvm_lvm_lv_get_size, METH_NOARGS },
	{ "isActive",		(PyCFunction)_liblvm_lvm_lv_is_active, METH_NOARGS },
	{ "isSuspended",	(PyCFunction)_liblvm_lvm_lv_is_suspended, METH_NOARGS },
	{ "addTag",		(PyCFunction)_liblvm_lvm_lv_add_tag, METH_VARARGS },
	{ "removeTag",		(PyCFunction)_liblvm_lvm_lv_remove_tag, METH_VARARGS },
	{ "getTags",		(PyCFunction)_liblvm_lvm_lv_get_tags, METH_NOARGS },
	{ "rename",		(PyCFunction)_liblvm_lvm_lv_rename, METH_VARARGS },
	{ "resize",		(PyCFunction)_liblvm_lvm_lv_resize, METH_VARARGS },
	{ "listLVsegs",		(PyCFunction)_liblvm_lvm_lv_list_lvsegs, METH_NOARGS },
	{ "snapshot",		(PyCFunction)_liblvm_lvm_lv_snapshot, METH_VARARGS },
	{ NULL, NULL }		/* sentinel */
};

static PyMethodDef _liblvm_pv_list_methods[] = {
	/* pv list methods */
	{ "__enter__", 		(PyCFunction)_liblvm_lvm_pvlist_get, METH_VARARGS },
	{ "__exit__", 		(PyCFunction)_liblvm_lvm_pvlist_put, METH_VARARGS },
	{ "open",		(PyCFunction)_liblvm_lvm_pvlist_get, METH_VARARGS },
	{ "close",		(PyCFunction)_liblvm_lvm_pvlist_put, METH_VARARGS },
	{ NULL, NULL }
};

static PyMethodDef _liblvm_pv_methods[] = {
	/* pv methods */
	{ "getName",		(PyCFunction)_liblvm_lvm_pv_get_name, METH_NOARGS },
	{ "getUuid",		(PyCFunction)_liblvm_lvm_pv_get_uuid, METH_NOARGS },
	{ "getMdaCount",	(PyCFunction)_liblvm_lvm_pv_get_mda_count, METH_NOARGS },
	{ "getProperty",	(PyCFunction)_liblvm_lvm_pv_get_property, METH_VARARGS },
	{ "getSize",		(PyCFunction)_liblvm_lvm_pv_get_size, METH_NOARGS },
	{ "getDevSize",		(PyCFunction)_liblvm_lvm_pv_get_dev_size, METH_NOARGS },
	{ "getFree",		(PyCFunction)_liblvm_lvm_pv_get_free, METH_NOARGS },
	{ "resize",		(PyCFunction)_liblvm_lvm_pv_resize, METH_VARARGS },
	{ "listPVsegs", 	(PyCFunction)_liblvm_lvm_pv_list_pvsegs, METH_NOARGS },
	{ NULL, NULL }		/* sentinel */
};

static PyMethodDef _liblvm_lvseg_methods[] = {
	{ "getProperty", 	(PyCFunction)_liblvm_lvm_lvseg_get_property, METH_VARARGS },
	{ NULL, NULL }		/* sentinel */
};

static PyMethodDef _liblvm_pvseg_methods[] = {
	{ "getProperty", 	(PyCFunction)_liblvm_lvm_pvseg_get_property, METH_VARARGS },
	{ NULL, NULL }		/* sentinel */
};

static PyTypeObject _LibLVMvgType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_vg",
	.tp_basicsize = sizeof(vgobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)liblvm_vg_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Volume Group object",
	.tp_methods = _liblvm_vg_methods,
};

static PyTypeObject _LibLVMlvType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_lv",
	.tp_basicsize = sizeof(lvobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)liblvm_lv_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Logical Volume object",
	.tp_methods = _liblvm_lv_methods,
};

static PyTypeObject _LibLVMpvlistType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_pvlist",
	.tp_basicsize = sizeof(pvslistobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)_liblvm_pvlist_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Physical Volume list object",
	.tp_methods = _liblvm_pv_list_methods,
};

static PyTypeObject _LibLVMpvType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_pv",
	.tp_basicsize = sizeof(pvobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)_liblvm_pv_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Physical Volume object",
	.tp_methods = _liblvm_pv_methods,
};

static PyTypeObject _LibLVMlvsegType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_lvseg",
	.tp_basicsize = sizeof(lvsegobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)_liblvm_lvseg_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Logical Volume Segment object",
	.tp_methods = _liblvm_lvseg_methods,
};

static PyTypeObject _LibLVMpvsegType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_pvseg",
	.tp_basicsize = sizeof(pvsegobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)_liblvm_pvseg_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Physical Volume Segment object",
	.tp_methods = _liblvm_pvseg_methods,
};

static void _liblvm_cleanup(void)
{
	if (_libh) {
		lvm_quit(_libh);
		_libh = NULL;
	}
}

PyMODINIT_FUNC initlvm(void)
{
	PyObject *m;

	_libh = lvm_init(NULL);

	if (PyType_Ready(&_LibLVMvgType) < 0)
		return;
	if (PyType_Ready(&_LibLVMlvType) < 0)
		return;
	if (PyType_Ready(&_LibLVMpvType) < 0)
		return;
	if (PyType_Ready(&_LibLVMlvsegType) < 0)
		return;
	if (PyType_Ready(&_LibLVMpvsegType) < 0)
		return;
	if (PyType_Ready(&_LibLVMpvlistType) < 0)
		return;

	if (!(m = Py_InitModule3("lvm", _Liblvm_methods, "Liblvm module")))
		return;

	if (PyModule_AddIntConstant(m, "THIN_DISCARDS_IGNORE",
				    LVM_THIN_DISCARDS_IGNORE) < 0)
		return;

	if (PyModule_AddIntConstant(m, "THIN_DISCARDS_NO_PASSDOWN",
				    LVM_THIN_DISCARDS_NO_PASSDOWN) < 0)
		return;

	if (PyModule_AddIntConstant(m, "THIN_DISCARDS_PASSDOWN",
				    LVM_THIN_DISCARDS_PASSDOWN) < 0)
		return;

	if ((_LibLVMError = PyErr_NewException("Liblvm._LibLVMError", NULL, NULL))) {
		/* Each call to PyModule_AddObject decrefs it; compensate: */
		Py_INCREF(_LibLVMError);
		Py_INCREF(_LibLVMError);
		PyModule_AddObject(m, "error", _LibLVMError);
		PyModule_AddObject(m, "LibLVMError", _LibLVMError);
	}

	Py_AtExit(_liblvm_cleanup);
}
