/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_LVM1_LABEL_H
#define _LVM_LVM1_LABEL_H

/*
 * This is what the 'extra_info' field of the label will point to
 * if the label type is lvm1.
 */
struct lvm_label_info {
	char volume_group[0];
};


struct labeller *lvm1_labeller_create(void);

#endif
