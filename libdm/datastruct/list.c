/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"

/*
 * Initialise a list before use.
 * The list head's next and previous pointers point back to itself.
 */
void list_init(struct list *head)
{
	head->n = head->p = head;
}

/*
 * Insert an element before 'head'.
 * If 'head' is the list head, this adds an element to the end of the list.
 */
void list_add(struct list *head, struct list *elem)
{
	assert(head->n);

	elem->n = head;
	elem->p = head->p;

	head->p->n = elem;
	head->p = elem;
}

/*
 * Insert an element after 'head'.
 * If 'head' is the list head, this adds an element to the front of the list.
 */
void list_add_h(struct list *head, struct list *elem)
{
	assert(head->n);

	elem->n = head->n;
	elem->p = head;

	head->n->p = elem;
	head->n = elem;
}

/*
 * Delete an element from its list.
 * Note that this doesn't change the element itself - it may still be safe
 * to follow its pointers.
 */
void list_del(struct list *elem)
{
	elem->n->p = elem->p;
	elem->p->n = elem->n;
}

/*
 * Is the list empty?
 */
int list_empty(struct list *head)
{
	return head->n == head;
}

/*
 * Is this the first element of the list?
 */
int list_start(struct list *head, struct list *elem)
{
	return elem->p == head;
}

/*
 * Is this the last element of the list?
 */
int list_end(struct list *head, struct list *elem)
{
	return elem->n == head;
}

/*
 * Return first element of the list or NULL if empty
 */
struct list *list_first(struct list *head)
{
	return (list_empty(head) ? NULL : head->n);
}

/*
 * Return last element of the list or NULL if empty
 */
struct list *list_last(struct list *head)
{
	return (list_empty(head) ? NULL : head->p);
}

/*
 * Return the previous element of the list, or NULL if we've reached the start.
 */
struct list *list_prev(struct list *head, struct list *elem)
{
	return (list_start(head, elem) ? NULL : elem->p);
}

/*
 * Return the next element of the list, or NULL if we've reached the end.
 */
struct list *list_next(struct list *head, struct list *elem)
{
	return (list_end(head, elem) ? NULL : elem->n);
}

/*
 * Return the number of elements in a list by walking it.
 */
unsigned int list_size(const struct list *head)
{
	unsigned int s = 0;
	const struct list *v;

	list_iterate(v, head)
	    s++;

	return s;
}
