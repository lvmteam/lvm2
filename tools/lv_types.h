/*
 * Copyright (C) 2016-2024 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * LV types used in command definitions.  The type strings are used
 * as LV suffixes, e.g. LV_type or LV_type1_type2.
 *
 * Update toollib.c:_lv_is_type() when adding new XXXXX_LVT test
 */

/* enum value 0 means none */
lvt(linear)
lvt(striped)
lvt(snapshot) /* lv_is_cow, lv_is_thick_snapshot */
lvt(cache)
lvt(cachepool)
lvt(integrity)
lvt(mirror)
lvt(raid) /* any raid type */
lvt(raid0)
lvt(raid1)
lvt(raid10)
lvt(raid4)
lvt(raid5)
lvt(raid6)
lvt(thin)
lvt(thinpool)
lvt(vdo)
lvt(vdopool)
lvt(vdopooldata)
lvt(writecache)
lvt(zero)
lvt(error)
