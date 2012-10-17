/*
 * Liblvm -- Python interface to LVM2 API.
 *
 * Copyright (C) 2010, 2012 Red Hat, Inc. All rights reserved.
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

static lvm_t libh;


typedef struct {
	PyObject_HEAD
	vg_t      vg;		    /* vg handle */
} vgobject;

typedef struct {
	PyObject_HEAD
	lv_t      lv;		    /* lv handle */
	vgobject  *parent_vgobj;
} lvobject;

typedef struct {
	PyObject_HEAD
	pv_t      pv;		    /* pv handle */
	vgobject  *parent_vgobj;
} pvobject;

typedef struct {
	PyObject_HEAD
	lvseg_t    lv_seg;	      /* lv segment handle */
	lvobject  *parent_lvobj;
} lvsegobject;

typedef struct {
	PyObject_HEAD
	pvseg_t    pv_seg;	      /* pv segment handle */
	pvobject   *parent_pvobj;
} pvsegobject;

static PyTypeObject LibLVMvgType;
static PyTypeObject LibLVMlvType;
static PyTypeObject LibLVMpvType;
static PyTypeObject LibLVMlvsegType;
static PyTypeObject LibLVMpvsegType;

static PyObject *LibLVMError;

#define LVM_VALID()							\
	do {								\
		if (!libh) {						\
			PyErr_SetString(PyExc_UnboundLocalError, "LVM handle invalid"); \
			return NULL;					\
		}							\
	} while (0)

static PyObject *
liblvm_get_last_error(void)
{
	PyObject *info;

	LVM_VALID();

	if ((info = PyTuple_New(2)) == NULL)
		return NULL;

	PyTuple_SetItem(info, 0, PyInt_FromLong((long) lvm_errno(libh)));
	PyTuple_SetItem(info, 1, PyString_FromString(lvm_errmsg(libh)));

	return info;
}

static PyObject *
liblvm_library_get_version(void)
{
	LVM_VALID();

	return Py_BuildValue("s", lvm_library_get_version());
}

static PyObject *
liblvm_lvm_list_vg_names(void)
{
	struct dm_list *vgnames;
	struct lvm_str_list *strl;
	PyObject * pytuple;
	int i = 0;

	LVM_VALID();

	vgnames = lvm_list_vg_names(libh);
	if (!vgnames) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	pytuple = PyTuple_New(dm_list_size(vgnames));
	if (!pytuple)
		return NULL;

	dm_list_iterate_items(strl, vgnames) {
		PyTuple_SET_ITEM(pytuple, i, PyString_FromString(strl->str));
		i++;
	}

	return pytuple;
}

static PyObject *
liblvm_lvm_list_vg_uuids(void)
{
	struct dm_list *uuids;
	struct lvm_str_list *strl;
	PyObject * pytuple;
	int i = 0;

	LVM_VALID();

	uuids = lvm_list_vg_uuids(libh);
	if (!uuids) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	pytuple = PyTuple_New(dm_list_size(uuids));
	if (!pytuple)
		return NULL;

	dm_list_iterate_items(strl, uuids) {
		PyTuple_SET_ITEM(pytuple, i, PyString_FromString(strl->str));
		i++;
	}

	return pytuple;
}

static PyObject *
liblvm_lvm_percent_to_float(PyObject *arg)
{
	double converted;
	int percent;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "i", &percent))
		return NULL;

	converted = lvm_percent_to_float(percent);
	return Py_BuildValue("d", converted);
}

static PyObject *
liblvm_lvm_vgname_from_pvid(PyObject *self, PyObject *arg)
{
	const char *pvid;
	const char *vgname;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "s", &pvid))
		return NULL;

	if ((vgname = lvm_vgname_from_pvid(libh, pvid)) == NULL) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	return Py_BuildValue("s", vgname);
}

static PyObject *
liblvm_lvm_vgname_from_device(PyObject *self, PyObject *arg)
{
	const char *device;
	const char *vgname;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "s", &device))
		return NULL;

	if ((vgname = lvm_vgname_from_device(libh, device)) == NULL) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	return Py_BuildValue("s", vgname);
}


static PyObject *
liblvm_lvm_config_find_bool(PyObject *self, PyObject *arg)
{
	const char *config;
	int rval;
	PyObject *rc;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "s", &config))
		return NULL;

	if ((rval = lvm_config_find_bool(libh, config, -10)) == -10) {
		/* Retrieving error information yields no error in this case */
		PyErr_Format(PyExc_ValueError, "config path not found");
		return NULL;
	}

	rc = (rval) ? Py_True: Py_False;

	Py_INCREF(rc);
	return rc;
}

static PyObject *
liblvm_lvm_config_reload(void)
{
	int rval;

	LVM_VALID();

	if ((rval = lvm_config_reload(libh)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}


static PyObject *
liblvm_lvm_scan(void)
{
	int rval;

	LVM_VALID();

	if ((rval = lvm_scan(libh)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_config_override(PyObject *self, PyObject *arg)
{
	const char *config;
	int rval;

	LVM_VALID();

	if (!PyArg_ParseTuple(arg, "s", &config))
		return NULL;

	if ((rval = lvm_config_override(libh, config)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}
/* ----------------------------------------------------------------------
 * VG object initialization/deallocation
 */


static PyObject *
liblvm_lvm_vg_open(PyObject *self, PyObject *args)
{
	const char *vgname;
	const char *mode = NULL;

	vgobject *vgobj;

	LVM_VALID();

	if (!PyArg_ParseTuple(args, "s|s", &vgname, &mode)) {
		return NULL;
	}

	if (mode == NULL)
		mode = "r";

	if ((vgobj = PyObject_New(vgobject, &LibLVMvgType)) == NULL)
		return NULL;

	if ((vgobj->vg = lvm_vg_open(libh, vgname, mode, 0))== NULL) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	return (PyObject *)vgobj;
}

static PyObject *
liblvm_lvm_vg_create(PyObject *self, PyObject *args)
{
	const char *vgname;
	vgobject *vgobj;

	LVM_VALID();

	if (!PyArg_ParseTuple(args, "s", &vgname)) {
		return NULL;
	}

	if ((vgobj = PyObject_New(vgobject, &LibLVMvgType)) == NULL)
		return NULL;

	if ((vgobj->vg = lvm_vg_create(libh, vgname))== NULL) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	return (PyObject *)vgobj;
}

static void
liblvm_vg_dealloc(vgobject *self)
{
	/* if already closed, don't reclose it */
	if (self->vg != NULL)
		lvm_vg_close(self->vg);
	PyObject_Del(self);
}

/* VG Methods */

#define VG_VALID(vgobject)						\
	do {								\
		LVM_VALID();						\
		if (!vgobject->vg) {					\
			PyErr_SetString(PyExc_UnboundLocalError, "VG object invalid"); \
			return NULL;					\
		}							\
	} while (0)

static PyObject *
liblvm_lvm_vg_close(vgobject *self)
{
	/* if already closed, don't reclose it */
	if (self->vg != NULL)
		lvm_vg_close(self->vg);

	self->vg = NULL;

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_vg_get_name(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("s", lvm_vg_get_name(self->vg));
}


static PyObject *
liblvm_lvm_vg_get_uuid(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("s", lvm_vg_get_uuid(self->vg));
}

static PyObject *
liblvm_lvm_vg_remove(vgobject *self)
{
	int rval;

	VG_VALID(self);

	if ((rval = lvm_vg_remove(self->vg)) == -1)
		goto error;

	if (lvm_vg_write(self->vg) == -1)
		goto error;

	self->vg = NULL;

	Py_INCREF(Py_None);
	return Py_None;

error:
	PyErr_SetObject(LibLVMError, liblvm_get_last_error());
	return NULL;
}

static PyObject *
liblvm_lvm_vg_extend(vgobject *self, PyObject *args)
{
	const char *device;
	int rval;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &device)) {
		return NULL;
	}

	if ((rval = lvm_vg_extend(self->vg, device)) == -1)
		goto error;

	if (lvm_vg_write(self->vg) == -1)
		goto error;

	Py_INCREF(Py_None);
	return Py_None;

error:
	PyErr_SetObject(LibLVMError, liblvm_get_last_error());
	return NULL;
}

static PyObject *
liblvm_lvm_vg_reduce(vgobject *self, PyObject *args)
{
	const char *device;
	int rval;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &device)) {
		return NULL;
	}

	if ((rval = lvm_vg_reduce(self->vg, device)) == -1)
		goto error;

	if (lvm_vg_write(self->vg) == -1)
		goto error;

	Py_INCREF(Py_None);
	return Py_None;

error:
	PyErr_SetObject(LibLVMError, liblvm_get_last_error());
	return NULL;
}

static PyObject *
liblvm_lvm_vg_add_tag(vgobject *self, PyObject *args)
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
	PyErr_SetObject(LibLVMError, liblvm_get_last_error());
	return NULL;
}

static PyObject *
liblvm_lvm_vg_remove_tag(vgobject *self, PyObject *args)
{
	const char *tag;
	int rval;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &tag)) {
		return NULL;
	}

	if ((rval = lvm_vg_remove_tag(self->vg, tag)) == -1)
		goto error;

	if (lvm_vg_write(self->vg) == -1)
		goto error;

	Py_INCREF(Py_None);
	return Py_None;

error:
	PyErr_SetObject(LibLVMError, liblvm_get_last_error());
	return NULL;

}

static PyObject *
liblvm_lvm_vg_is_clustered(vgobject *self)
{
	PyObject *rval;

	VG_VALID(self);

	rval = ( lvm_vg_is_clustered(self->vg) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);
	return rval;
}

static PyObject *
liblvm_lvm_vg_is_exported(vgobject *self)
{
	PyObject *rval;

	VG_VALID(self);

	rval = ( lvm_vg_is_exported(self->vg) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);
	return rval;
}

static PyObject *
liblvm_lvm_vg_is_partial(vgobject *self)
{
	PyObject *rval;

	VG_VALID(self);

	rval = ( lvm_vg_is_partial(self->vg) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);
	return rval;
}

static PyObject *
liblvm_lvm_vg_get_seqno(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("l", lvm_vg_get_seqno(self->vg));
}

static PyObject *
liblvm_lvm_vg_get_size(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("l", lvm_vg_get_size(self->vg));
}

static PyObject *
liblvm_lvm_vg_get_free_size(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("l", lvm_vg_get_free_size(self->vg));
}

static PyObject *
liblvm_lvm_vg_get_extent_size(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("l", lvm_vg_get_extent_size(self->vg));
}

static PyObject *
liblvm_lvm_vg_get_extent_count(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("l", lvm_vg_get_extent_count(self->vg));
}

static PyObject *
liblvm_lvm_vg_get_free_extent_count(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("l", lvm_vg_get_free_extent_count(self->vg));
}

/* Builds a python tuple ([string|number], bool) from a struct lvm_property_value */
static PyObject *
get_property(struct lvm_property_value *prop)
{
	PyObject *pytuple;
	PyObject *setable;

	if (!prop->is_valid) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	pytuple = PyTuple_New(2);
	if (!pytuple)
		return NULL;

	if (prop->is_integer) {
		PyTuple_SET_ITEM(pytuple, 0, Py_BuildValue("K", prop->value.integer));
	} else {
		PyTuple_SET_ITEM(pytuple, 0, PyString_FromString(prop->value.string));
	}

	if (prop->is_settable) {
		setable = Py_True;
	} else {
		setable = Py_False;
	}

	Py_INCREF(setable);
	PyTuple_SET_ITEM(pytuple, 1, setable);
	return pytuple;
}

/* This will return a tuple of (value, bool) with the value being a string or
   integer and bool indicating if property is settable */
static PyObject *
liblvm_lvm_vg_get_property(vgobject *self,  PyObject *args)
{
	const char *name;
	struct lvm_property_value prop_value;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;

	prop_value = lvm_vg_get_property(self->vg, name);
	return get_property(&prop_value);
}

static PyObject *
liblvm_lvm_vg_set_property(vgobject *self,  PyObject *args)
{
	const char *property_name = NULL;
	PyObject *variant_type_arg = NULL;
	struct lvm_property_value lvm_property;
	char *string_value = NULL;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "sO", &property_name, &variant_type_arg))
		return NULL;

	lvm_property = lvm_vg_get_property(self->vg, property_name);

	if (!lvm_property.is_valid ) {
		goto lvmerror;
	}

	if (PyObject_IsInstance(variant_type_arg, (PyObject*)&PyString_Type)) {

		if (!lvm_property.is_string) {
			PyErr_Format(PyExc_ValueError, "Property requires string value");
			goto bail;
		}

		/* Based on cursory code inspection this path may cause a memory
		   leak when calling into set_property, need to verify*/
		string_value = strdup(PyString_AsString(variant_type_arg));
		lvm_property.value.string = string_value;
		if (!lvm_property.value.string) {
			PyErr_NoMemory();
			goto bail;
		}

	} else {

		if (!lvm_property.is_integer) {
			PyErr_Format(PyExc_ValueError, "Property requires numeric value");
			goto bail;
		}

		if (PyObject_IsInstance(variant_type_arg, (PyObject*)&PyInt_Type)) {
			int temp_py_int = PyInt_AsLong(variant_type_arg);

			/* -1 could be valid, need to see if an exception was gen. */
			if (temp_py_int == -1 && PyErr_Occurred()) {
				goto bail;
			}

			if (temp_py_int < 0) {
				PyErr_Format(PyExc_ValueError, "Positive integers only!");
				goto bail;
			}

			lvm_property.value.integer = temp_py_int;
		} else if (PyObject_IsInstance(variant_type_arg, (PyObject*)&PyLong_Type)){
			/* This will fail on negative numbers */
			unsigned long long temp_py_long = PyLong_AsUnsignedLongLong(variant_type_arg);
			if (temp_py_long == (unsigned long long)-1) {
				goto bail;
			}

			lvm_property.value.integer = temp_py_long;
		} else {
			PyErr_Format(PyExc_ValueError, "supported value types are numeric and string");
			goto bail;
		}
	}

	if (lvm_vg_set_property(self->vg, property_name, &lvm_property) == -1) {
		goto lvmerror;
	}

	if (lvm_vg_write(self->vg) == -1) {
		goto lvmerror;
	}

	Py_DECREF(variant_type_arg);
	Py_INCREF(Py_None);
	return Py_None;

lvmerror:
	PyErr_SetObject(LibLVMError, liblvm_get_last_error());
bail:
	free(string_value);
	if (variant_type_arg) {
		Py_DECREF(variant_type_arg);
		variant_type_arg = NULL;
	}
	return NULL;
}

static PyObject *
liblvm_lvm_vg_get_pv_count(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("l", lvm_vg_get_pv_count(self->vg));
}

static PyObject *
liblvm_lvm_vg_get_max_pv(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("l", lvm_vg_get_max_pv(self->vg));
}

static PyObject *
liblvm_lvm_vg_get_max_lv(vgobject *self)
{
	VG_VALID(self);

	return Py_BuildValue("l", lvm_vg_get_max_lv(self->vg));
}

static PyObject *
liblvm_lvm_vg_set_extent_size(vgobject *self, PyObject *args)
{
	uint32_t new_size;
	int rval;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "l", &new_size)) {
		return NULL;
	}

	if ((rval = lvm_vg_set_extent_size(self->vg, new_size)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_vg_list_lvs(vgobject *self)
{
	struct dm_list *lvs;
	struct lvm_lv_list *lvl;
	PyObject * pytuple;
	lvobject * lvobj;
	int i = 0;

	VG_VALID(self);

	/* unlike other LVM api calls, if there are no results, we get NULL */
	lvs = lvm_vg_list_lvs(self->vg);
	if (!lvs)
		return Py_BuildValue("()");

	pytuple = PyTuple_New(dm_list_size(lvs));
	if (!pytuple)
		return NULL;

	dm_list_iterate_items(lvl, lvs) {
		/* Create and initialize the object */
		lvobj = PyObject_New(lvobject, &LibLVMlvType);
		if (!lvobj) {
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

static PyObject *
liblvm_lvm_vg_get_tags(vgobject *self)
{
	struct dm_list *tags;
	struct lvm_str_list *strl;
	PyObject * pytuple;
	int i = 0;

	VG_VALID(self);

	tags = lvm_vg_get_tags(self->vg);
	if (!tags) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	pytuple = PyTuple_New(dm_list_size(tags));
	if (!pytuple)
		return NULL;

	dm_list_iterate_items(strl, tags) {
		PyTuple_SET_ITEM(pytuple, i, PyString_FromString(strl->str));
		i++;
	}

	return pytuple;
}

static PyObject *
liblvm_lvm_vg_create_lv_linear(vgobject *self, PyObject *args)
{
	const char *vgname;
	uint64_t size;
	lvobject *lvobj;

	VG_VALID(self);

	if (!PyArg_ParseTuple(args, "sl", &vgname, &size)) {
		return NULL;
	}

	if ((lvobj = PyObject_New(lvobject, &LibLVMlvType)) == NULL)
		return NULL;

	if ((lvobj->lv = lvm_vg_create_lv_linear(self->vg, vgname, size)) == NULL) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		Py_DECREF(lvobj);
		return NULL;
	}

	lvobj->parent_vgobj = self;
	Py_INCREF(lvobj->parent_vgobj);

	return (PyObject *)lvobj;
}

static void
liblvm_lv_dealloc(lvobject *self)
{
	Py_DECREF(self->parent_vgobj);
	PyObject_Del(self);
}

static PyObject *
liblvm_lvm_vg_list_pvs(vgobject *self)
{
	struct dm_list *pvs;
	struct lvm_pv_list *pvl;
	PyObject * pytuple;
	pvobject * pvobj;
	int i = 0;

	VG_VALID(self);

	/* unlike other LVM api calls, if there are no results, we get NULL */
	pvs = lvm_vg_list_pvs(self->vg);
	if (!pvs)
		return Py_BuildValue("()");

	pytuple = PyTuple_New(dm_list_size(pvs));
	if (!pytuple)
		return NULL;

	dm_list_iterate_items(pvl, pvs) {
		/* Create and initialize the object */
		pvobj = PyObject_New(pvobject, &LibLVMpvType);
		if (!pvobj) {
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

static PyObject *
liblvm_lvm_lv_from_N(vgobject *self, PyObject *arg, lv_fetch_by_N method)
{
	const char *id;
	lvobject *lvobj;
	lv_t lv = NULL;

	VG_VALID(self);

	if (!PyArg_ParseTuple(arg, "s", &id))
		return NULL;

	lv = method(self->vg, id);
	if (!lv) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	lvobj = PyObject_New(lvobject, &LibLVMlvType);
	if (!lvobj) {
		return NULL;
	}

	lvobj->parent_vgobj = self;
	Py_INCREF(lvobj->parent_vgobj);

	lvobj->lv = lv;
	return (PyObject *)lvobj;
}

static PyObject *
liblvm_lvm_lv_from_name(vgobject *self, PyObject *arg)
{
	return liblvm_lvm_lv_from_N(self, arg, lvm_lv_from_name);
}

static PyObject *
liblvm_lvm_lv_from_uuid(vgobject *self, PyObject *arg)
{
	return liblvm_lvm_lv_from_N(self, arg, lvm_lv_from_uuid);
}

static PyObject *
liblvm_lvm_pv_from_N(vgobject *self, PyObject *arg, pv_fetch_by_N method)
{
	const char *id;
	pvobject *rc;
	pv_t pv = NULL;

	VG_VALID(self);

	if (!PyArg_ParseTuple(arg, "s", &id))
		return NULL;

	pv = method(self->vg, id);
	if (!pv) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	rc = PyObject_New(pvobject, &LibLVMpvType);
	if (!rc) {
		return NULL;
	}

	Py_INCREF(self);
	rc->pv = pv;
	return (PyObject *)rc;
}

static PyObject *
liblvm_lvm_pv_from_name(vgobject *self, PyObject *arg)
{
	return liblvm_lvm_pv_from_N(self, arg, lvm_pv_from_name);
}

static PyObject *
liblvm_lvm_pv_from_uuid(vgobject *self, PyObject *arg)
{
	return liblvm_lvm_pv_from_N(self, arg, lvm_pv_from_uuid);
}

static void
liblvm_pv_dealloc(pvobject *self)
{
	Py_DECREF(self->parent_vgobj);
	PyObject_Del(self);
}

/* LV Methods */

#define LV_VALID(lvobject)						\
	do {								\
		VG_VALID(lvobject->parent_vgobj);			\
		if (!lvobject->lv) {					\
			PyErr_SetString(PyExc_UnboundLocalError, "LV object invalid"); \
			return NULL;					\
		}							\
	} while (0)


static PyObject *
liblvm_lvm_lv_get_name(lvobject *self)
{
	LV_VALID(self);

	return Py_BuildValue("s", lvm_lv_get_name(self->lv));
}

static PyObject *
liblvm_lvm_lv_get_uuid(lvobject *self)
{
	LV_VALID(self);

	return Py_BuildValue("s", lvm_lv_get_uuid(self->lv));
}

static PyObject *
liblvm_lvm_lv_activate(lvobject *self)
{
	int rval;

	LV_VALID(self);

	if ((rval = lvm_lv_activate(self->lv)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_lv_deactivate(lvobject *self)
{
	int rval;

	LV_VALID(self);

	if ((rval = lvm_lv_deactivate(self->lv)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_vg_remove_lv(lvobject *self)
{
	int rval;

	LV_VALID(self);

	if ((rval = lvm_vg_remove_lv(self->lv)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	self->lv = NULL;

	Py_INCREF(Py_None);
	return Py_None;
}

/* This will return a tuple of (value, bool) with the value being a string or
   integer and bool indicating if property is settable */
static PyObject *
liblvm_lvm_lv_get_property(lvobject *self, PyObject *args)
{
	const char *name;
	struct lvm_property_value prop_value;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;

	prop_value = lvm_lv_get_property(self->lv, name);
	return get_property(&prop_value);
}

static PyObject *
liblvm_lvm_lv_get_size(lvobject *self)
{
	LV_VALID(self);

	return Py_BuildValue("l", lvm_lv_get_size(self->lv));
}

static PyObject *
liblvm_lvm_lv_is_active(lvobject *self)
{
	PyObject *rval;

	LV_VALID(self);

	rval = ( lvm_lv_is_active(self->lv) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);
	return rval;
}

static PyObject *
liblvm_lvm_lv_is_suspended(lvobject *self)
{
	PyObject *rval;

	LV_VALID(self);

	rval = ( lvm_lv_is_suspended(self->lv) == 1) ? Py_True : Py_False;

	Py_INCREF(rval);
	return rval;
}

static PyObject *
liblvm_lvm_lv_add_tag(lvobject *self, PyObject *args)
{
	const char *tag;
	int rval;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &tag)) {
		return NULL;
	}

	if ((rval = lvm_lv_add_tag(self->lv, tag)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_lv_remove_tag(lvobject *self, PyObject *args)
{
	const char *tag;
	int rval;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &tag)) {
		return NULL;
	}

	if ((rval = lvm_lv_remove_tag(self->lv, tag)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_lv_get_tags(lvobject *self)
{
	struct dm_list *tags;
	struct lvm_str_list *strl;
	PyObject * pytuple;
	int i = 0;

	LV_VALID(self);

	tags = lvm_lv_get_tags(self->lv);
	if (!tags) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	pytuple = PyTuple_New(dm_list_size(tags));
	if (!pytuple)
		return NULL;

	dm_list_iterate_items(strl, tags) {
		PyTuple_SET_ITEM(pytuple, i, PyString_FromString(strl->str));
		i++;
	}

	return pytuple;
}

static PyObject *
liblvm_lvm_lv_rename(lvobject *self, PyObject *args)
{
	const char *new_name;
	int rval;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &new_name))
		return NULL;

	if ((rval = lvm_lv_rename(self->lv, new_name)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_lv_resize(lvobject *self, PyObject *args)
{
	uint64_t new_size;
	int rval;

	LV_VALID(self);

	if (!PyArg_ParseTuple(args, "l", &new_size)) {
		return NULL;
	}

	if ((rval = lvm_lv_resize(self->lv, new_size)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_lv_list_lvsegs(lvobject *self)
{
	struct dm_list  *lvsegs;
	lvseg_list_t    *lvsegl;
	PyObject * pytuple;
	lvsegobject *lvsegobj;
	int i = 0;

	LV_VALID(self);

	lvsegs = lvm_lv_list_lvsegs(self->lv);
	if (!lvsegs) {
		return Py_BuildValue("()");
	}

	pytuple = PyTuple_New(dm_list_size(lvsegs));
	if (!pytuple)
		return NULL;

	dm_list_iterate_items(lvsegl, lvsegs) {
		/* Create and initialize the object */
		lvsegobj = PyObject_New(lvsegobject, &LibLVMlvsegType);
		if (!lvsegobj) {
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

/* PV Methods */

#define PV_VALID(pvobject)						\
	do {								\
		VG_VALID(pvobject->parent_vgobj);			\
		if (!pvobject->pv) {					\
			PyErr_SetString(PyExc_UnboundLocalError, "PV object invalid"); \
			return NULL;					\
		}							\
	} while (0)

static PyObject *
liblvm_lvm_pv_get_name(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("s", lvm_pv_get_name(self->pv));
}

static PyObject *
liblvm_lvm_pv_get_uuid(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("s", lvm_pv_get_uuid(self->pv));
}

static PyObject *
liblvm_lvm_pv_get_mda_count(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("l", lvm_pv_get_mda_count(self->pv));
}

static PyObject *
liblvm_lvm_pv_get_property(pvobject *self,  PyObject *args)
{
	const char *name;
	struct lvm_property_value prop_value;

	PV_VALID(self);

	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;

	prop_value = lvm_pv_get_property(self->pv, name);
	return get_property(&prop_value);
}

static PyObject *
liblvm_lvm_pv_get_dev_size(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("l", lvm_pv_get_dev_size(self->pv));
}

static PyObject *
liblvm_lvm_pv_get_size(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("l", lvm_pv_get_size(self->pv));
}

static PyObject *
liblvm_lvm_pv_get_free(pvobject *self)
{
	PV_VALID(self);

	return Py_BuildValue("l", lvm_pv_get_free(self->pv));
}

static PyObject *
liblvm_lvm_pv_resize(pvobject *self, PyObject *args)
{
	uint64_t new_size;
	int rval;

	PV_VALID(self);

	if (!PyArg_ParseTuple(args, "l", &new_size)) {
		return NULL;
	}

	if ((rval = lvm_pv_resize(self->pv, new_size)) == -1) {
		PyErr_SetObject(LibLVMError, liblvm_get_last_error());
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
liblvm_lvm_pv_list_pvsegs(pvobject *self)
{
	struct dm_list *pvsegs;
	pvseg_list_t *pvsegl;
	PyObject *pytuple;
	pvsegobject *pvsegobj;
	int i = 0;

	PV_VALID(self);

	pvsegs = lvm_pv_list_pvsegs(self->pv);
	if (!pvsegs) {
		return Py_BuildValue("()");
	}

	pytuple = PyTuple_New(dm_list_size(pvsegs));
	if (!pytuple)
		return NULL;

	dm_list_iterate_items(pvsegl, pvsegs) {
		/* Create and initialize the object */
		pvsegobj = PyObject_New(pvsegobject, &LibLVMpvsegType);
		if (!pvsegobj) {
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

static void
liblvm_lvseg_dealloc(lvsegobject *self)
{
	Py_DECREF(self->parent_lvobj);
	PyObject_Del(self);
}

static PyObject *
liblvm_lvm_lvseg_get_property(lvsegobject *self,  PyObject *args)
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

static void
liblvm_pvseg_dealloc(pvsegobject *self)
{
	Py_DECREF(self->parent_pvobj);
	PyObject_Del(self);
}

static PyObject *
liblvm_lvm_pvseg_get_property(pvsegobject *self,  PyObject *args)
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

static PyMethodDef Liblvm_methods[] = {
	/* LVM methods */
	{ "getVersion",		(PyCFunction)liblvm_library_get_version, METH_NOARGS },
	{ "vgOpen",		(PyCFunction)liblvm_lvm_vg_open, METH_VARARGS },
	{ "vgCreate",		(PyCFunction)liblvm_lvm_vg_create, METH_VARARGS },
	{ "configFindBool",	(PyCFunction)liblvm_lvm_config_find_bool, METH_VARARGS },
	{ "configReload",	(PyCFunction)liblvm_lvm_config_reload, METH_NOARGS },
	{ "configOverride",	(PyCFunction)liblvm_lvm_config_override, METH_VARARGS },
	{ "scan",		(PyCFunction)liblvm_lvm_scan, METH_NOARGS },
	{ "listVgNames",	(PyCFunction)liblvm_lvm_list_vg_names, METH_NOARGS },
	{ "listVgUuids",	(PyCFunction)liblvm_lvm_list_vg_uuids, METH_NOARGS },
	{ "percentToFloat",	(PyCFunction)liblvm_lvm_percent_to_float, METH_VARARGS },
	{ "vgNameFromPvid",	(PyCFunction)liblvm_lvm_vgname_from_pvid, METH_VARARGS },
	{ "vgNameFromDevice",	(PyCFunction)liblvm_lvm_vgname_from_device, METH_VARARGS },
	{ NULL,	     NULL}	   /* sentinel */
};

static PyMethodDef liblvm_vg_methods[] = {
	/* vg methods */
	{ "getName",		(PyCFunction)liblvm_lvm_vg_get_name, METH_NOARGS },
	{ "getUuid",		(PyCFunction)liblvm_lvm_vg_get_uuid, METH_NOARGS },
	{ "close",		(PyCFunction)liblvm_lvm_vg_close, METH_NOARGS },
	{ "remove",		(PyCFunction)liblvm_lvm_vg_remove, METH_NOARGS },
	{ "extend",		(PyCFunction)liblvm_lvm_vg_extend, METH_VARARGS },
	{ "reduce",		(PyCFunction)liblvm_lvm_vg_reduce, METH_VARARGS },
	{ "addTag",		(PyCFunction)liblvm_lvm_vg_add_tag, METH_VARARGS },
	{ "removeTag",		(PyCFunction)liblvm_lvm_vg_remove_tag, METH_VARARGS },
	{ "setExtentSize",	(PyCFunction)liblvm_lvm_vg_set_extent_size, METH_VARARGS },
	{ "isClustered",	(PyCFunction)liblvm_lvm_vg_is_clustered, METH_NOARGS },
	{ "isExported",		(PyCFunction)liblvm_lvm_vg_is_exported, METH_NOARGS },
	{ "isPartial",		(PyCFunction)liblvm_lvm_vg_is_partial, METH_NOARGS },
	{ "getSeqno",		(PyCFunction)liblvm_lvm_vg_get_seqno, METH_NOARGS },
	{ "getSize",		(PyCFunction)liblvm_lvm_vg_get_size, METH_NOARGS },
	{ "getFreeSize",	(PyCFunction)liblvm_lvm_vg_get_free_size, METH_NOARGS },
	{ "getExtentSize",	(PyCFunction)liblvm_lvm_vg_get_extent_size, METH_NOARGS },
	{ "getExtentCount",	(PyCFunction)liblvm_lvm_vg_get_extent_count, METH_NOARGS },
	{ "getFreeExtentCount",	(PyCFunction)liblvm_lvm_vg_get_free_extent_count, METH_NOARGS },
	{ "getProperty",	(PyCFunction)liblvm_lvm_vg_get_property, METH_VARARGS },
	{ "setProperty",	(PyCFunction)liblvm_lvm_vg_set_property, METH_VARARGS },
	{ "getPvCount",		(PyCFunction)liblvm_lvm_vg_get_pv_count, METH_NOARGS },
	{ "getMaxPv",		(PyCFunction)liblvm_lvm_vg_get_max_pv, METH_NOARGS },
	{ "getMaxLv",		(PyCFunction)liblvm_lvm_vg_get_max_lv, METH_NOARGS },
	{ "listLVs",		(PyCFunction)liblvm_lvm_vg_list_lvs, METH_NOARGS },
	{ "listPVs",		(PyCFunction)liblvm_lvm_vg_list_pvs, METH_NOARGS },
	{ "lvFromName", 	(PyCFunction)liblvm_lvm_lv_from_name, METH_VARARGS },
	{ "lvFromUuid", 	(PyCFunction)liblvm_lvm_lv_from_uuid, METH_VARARGS },
	{ "pvFromName", 	(PyCFunction)liblvm_lvm_pv_from_name, METH_VARARGS },
	{ "pvFromUuid", 	(PyCFunction)liblvm_lvm_pv_from_uuid, METH_VARARGS },
	{ "getTags",		(PyCFunction)liblvm_lvm_vg_get_tags, METH_NOARGS },
	{ "createLvLinear",	(PyCFunction)liblvm_lvm_vg_create_lv_linear, METH_VARARGS },
	{ NULL,	     NULL}   /* sentinel */
};

static PyMethodDef liblvm_lv_methods[] = {
	/* lv methods */
	{ "getName",		(PyCFunction)liblvm_lvm_lv_get_name, METH_NOARGS },
	{ "getUuid",		(PyCFunction)liblvm_lvm_lv_get_uuid, METH_NOARGS },
	{ "activate",		(PyCFunction)liblvm_lvm_lv_activate, METH_NOARGS },
	{ "deactivate",		(PyCFunction)liblvm_lvm_lv_deactivate, METH_NOARGS },
	{ "remove",		(PyCFunction)liblvm_lvm_vg_remove_lv, METH_NOARGS },
	{ "getProperty",	(PyCFunction)liblvm_lvm_lv_get_property, METH_VARARGS },
	{ "getSize",		(PyCFunction)liblvm_lvm_lv_get_size, METH_NOARGS },
	{ "isActive",		(PyCFunction)liblvm_lvm_lv_is_active, METH_NOARGS },
	{ "isSuspended",	(PyCFunction)liblvm_lvm_lv_is_suspended, METH_NOARGS },
	{ "addTag",		(PyCFunction)liblvm_lvm_lv_add_tag, METH_VARARGS },
	{ "removeTag",		(PyCFunction)liblvm_lvm_lv_remove_tag, METH_VARARGS },
	{ "getTags",		(PyCFunction)liblvm_lvm_lv_get_tags, METH_NOARGS },
	{ "rename",		(PyCFunction)liblvm_lvm_lv_rename, METH_VARARGS },
	{ "resize",		(PyCFunction)liblvm_lvm_lv_resize, METH_VARARGS },
	{ "listLVsegs",		(PyCFunction)liblvm_lvm_lv_list_lvsegs, METH_NOARGS },
	{ NULL,	     NULL}   /* sentinel */
};

static PyMethodDef liblvm_pv_methods[] = {
	/* pv methods */
	{ "getName",		(PyCFunction)liblvm_lvm_pv_get_name, METH_NOARGS },
	{ "getUuid",		(PyCFunction)liblvm_lvm_pv_get_uuid, METH_NOARGS },
	{ "getMdaCount",	(PyCFunction)liblvm_lvm_pv_get_mda_count, METH_NOARGS },
	{ "getProperty",	(PyCFunction)liblvm_lvm_pv_get_property, METH_VARARGS },
	{ "getSize",		(PyCFunction)liblvm_lvm_pv_get_size, METH_NOARGS },
	{ "getDevSize",		(PyCFunction)liblvm_lvm_pv_get_dev_size, METH_NOARGS },
	{ "getFree",		(PyCFunction)liblvm_lvm_pv_get_free, METH_NOARGS },
	{ "resize",		(PyCFunction)liblvm_lvm_pv_resize, METH_VARARGS },
	{ "listPVsegs", 	(PyCFunction)liblvm_lvm_pv_list_pvsegs, METH_NOARGS },
	{ NULL,	     NULL}   /* sentinel */
};

static PyMethodDef liblvm_lvseg_methods[] = {
	{ "getProperty", 	(PyCFunction)liblvm_lvm_lvseg_get_property, METH_VARARGS },
	{ NULL,	     NULL}   /* sentinel */
};

static PyMethodDef liblvm_pvseg_methods[] = {
	{ "getProperty", 	(PyCFunction)liblvm_lvm_pvseg_get_property, METH_VARARGS },
	{ NULL,	     NULL}   /* sentinel */
};

static PyTypeObject LibLVMvgType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_vg",
	.tp_basicsize = sizeof(vgobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)liblvm_vg_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Volume Group object",
	.tp_methods = liblvm_vg_methods,
};

static PyTypeObject LibLVMlvType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_lv",
	.tp_basicsize = sizeof(lvobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)liblvm_lv_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Logical Volume object",
	.tp_methods = liblvm_lv_methods,
};

static PyTypeObject LibLVMpvType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_pv",
	.tp_basicsize = sizeof(pvobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)liblvm_pv_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Physical Volume object",
	.tp_methods = liblvm_pv_methods,
};

static PyTypeObject LibLVMlvsegType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_lvseg",
	.tp_basicsize = sizeof(lvsegobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)liblvm_lvseg_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Logical Volume Segment object",
	.tp_methods = liblvm_lvseg_methods,
};

static PyTypeObject LibLVMpvsegType = {
	PyObject_HEAD_INIT(&PyType_Type)
	.tp_name = "liblvm.Liblvm_pvseg",
	.tp_basicsize = sizeof(pvsegobject),
	.tp_new = PyType_GenericNew,
	.tp_dealloc = (destructor)liblvm_pvseg_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "LVM Physical Volume Segment object",
	.tp_methods = liblvm_pvseg_methods,
};

static void
liblvm_cleanup(void)
{
	lvm_quit(libh);
	libh = NULL;
}

PyMODINIT_FUNC
initlvm(void)
{
	PyObject *m;

	libh = lvm_init(NULL);

	if (PyType_Ready(&LibLVMvgType) < 0)
		return;
	if (PyType_Ready(&LibLVMlvType) < 0)
		return;
	if (PyType_Ready(&LibLVMpvType) < 0)
		return;
	if (PyType_Ready(&LibLVMlvsegType) < 0)
		return;
	if (PyType_Ready(&LibLVMpvsegType) < 0)
		return;

	m = Py_InitModule3("lvm", Liblvm_methods, "Liblvm module");
	if (m == NULL)
		return;

	LibLVMError = PyErr_NewException("Liblvm.LibLVMError",
					 NULL, NULL);
	if (LibLVMError) {
		/* Each call to PyModule_AddObject decrefs it; compensate: */
		Py_INCREF(LibLVMError);
		Py_INCREF(LibLVMError);
		PyModule_AddObject(m, "error", LibLVMError);
		PyModule_AddObject(m, "LibLVMError", LibLVMError);
	}

	Py_AtExit(liblvm_cleanup);
}
