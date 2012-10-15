#
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 2.1 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
#-----------------------------
# Python example code:
#-----------------------------

import lvm

# Note: This example will create a logical unit, tag it and
# 	delete it, don't run this on production box!

#Dump information about PV
def print_pv(pv):
    print 'PV name: ', pv.getName(), ' ID: ', pv.getUuid(), 'Size: ', pv.getSize()


#Dump some information about a specific volume group
def print_vg(vg_name):
    #Open read only
    vg = lvm.vgOpen(vg_name, 'r')

    print 'Volume group:', vg_name, 'Size: ', vg.getSize()

    #Retrieve a list of Physical volumes for this volume group
    pv_list = vg.listPVs()

    #Print out the physical volumes
    for p in pv_list:
        print_pv(p)

    #Get a list of logical volumes in this volume group
    lv_list = vg.listLVs()
    if len(lv_list):
        for l in lv_list:
            print 'LV name: ', l.getName(), ' ID: ', l.getUuid()
    else:
        print 'No logical volumes present!'

    vg.close()

#Returns the name of a vg with space available
def find_vg_with_free_space():
    free_space = 0
    rc = None

    vg_names = lvm.listVgNames()
    for v in vg_names:
        vg = lvm.vgOpen(v, 'r')
        c_free = vg.getFreeSize()
        if c_free > free_space:
            free_space = c_free
            rc = v
        vg.close()

    return rc

#Walk through the volume groups and fine one with space in which we can
#create a new logical volume
def create_delete_logical_volume():
    vg_name = find_vg_with_free_space()

    print 'Using volume group ', vg_name, ' for example'

    if vg_name:
        vg = lvm.vgOpen(vg_name, 'w')
        lv = vg.createLvLinear('python_lvm_ok_to_delete', vg.getFreeSize())

        if lv:
            print 'New lv, id= ', lv.getUuid()

            #Create a tag
            lv.addTag('Demo_tag')

            #Get the tags
            tags = lv.getTags()
            for t in tags:
                #Remove tag
                lv.removeTag(t)

            lv.deactivate()

            #Try to rename
            lv.rename("python_lvm_renamed")
            print 'LV name= ', lv.getName()
            lv.remove()

        vg.close()
    else:
        print 'No free space available to create demo lv!'

if __name__ == '__main__':
    #What version
    print 'lvm version=', lvm.getVersion()

    #Get a list of volume group names
    vg_names = lvm.listVgNames()

    #For each volume group display some information about each of them
    for vg_i in vg_names:
        print_vg(vg_i)

    #Demo creating a logical volume
    create_delete_logical_volume()

