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
#include "config.h"
#include "crc.h"
#include "pool.h"
#include "device.h"
#include "str_list.h"
#include "toolcontext.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

enum {
	TOK_INT,
	TOK_FLOAT,
	TOK_STRING,
	TOK_EQ,
	TOK_SECTION_B,
	TOK_SECTION_E,
	TOK_ARRAY_B,
	TOK_ARRAY_E,
	TOK_IDENTIFIER,
	TOK_COMMA,
	TOK_EOF
};

struct parser {
	char *fb, *fe;		/* file limits */

	int t;			/* token limits and type */
	char *tb, *te;

	int fd;			/* descriptor for file being parsed */
	int line;		/* line number we are on */

	struct pool *mem;
};

struct cs {
	struct config_tree cft;
	struct pool *mem;
	time_t timestamp;
	char *filename;
	int exists;
};

static void _get_token(struct parser *p, int tok_prev);
static void _eat_space(struct parser *p);
static struct config_node *_file(struct parser *p);
static struct config_node *_section(struct parser *p);
static struct config_value *_value(struct parser *p);
static struct config_value *_type(struct parser *p);
static int _match_aux(struct parser *p, int t);
static struct config_value *_create_value(struct parser *p);
static struct config_node *_create_node(struct parser *p);
static char *_dup_tok(struct parser *p);

static const int sep = '/';

#define MAX_INDENT 32

#define match(t) do {\
   if (!_match_aux(p, (t))) {\
	log_error("Parse error at line %d: unexpected token", p->line); \
      return 0;\
   } \
} while(0);

static int _tok_match(const char *str, const char *b, const char *e)
{
	while (*str && (b != e)) {
		if (*str++ != *b++)
			return 0;
	}

	return !(*str || (b != e));
}

/*
 * public interface
 */
struct config_tree *create_config_tree(const char *filename)
{
	struct cs *c;
	struct pool *mem = pool_create(10 * 1024);

	if (!mem) {
		stack;
		return 0;
	}

	if (!(c = pool_zalloc(mem, sizeof(*c)))) {
		stack;
		pool_destroy(mem);
		return 0;
	}

	c->mem = mem;
	c->cft.root = (struct config_node *) NULL;
	c->timestamp = 0;
	c->exists = 0;
	if (filename)
		c->filename = pool_strdup(c->mem, filename);
	return &c->cft;
}

void destroy_config_tree(struct config_tree *cft)
{
	pool_destroy(((struct cs *) cft)->mem);
}

int read_config_fd(struct config_tree *cft, struct device *dev,
		   off_t offset, size_t size, off_t offset2, size_t size2,
		   checksum_fn_t checksum_fn, uint32_t checksum)
{
	struct cs *c = (struct cs *) cft;
	struct parser *p;
	int r = 0;
	int use_mmap = 1;
	off_t mmap_offset = 0;

	if (!(p = pool_alloc(c->mem, sizeof(*p)))) {
		stack;
		return 0;
	}
	p->mem = c->mem;

	/* Only use mmap with regular files */
	if (!(dev->flags & DEV_REGULAR) || size2)
		use_mmap = 0;

	if (use_mmap) {
		mmap_offset = offset % getpagesize();
		/* memory map the file */
		p->fb = mmap((caddr_t) 0, size + mmap_offset, PROT_READ,
			     MAP_PRIVATE, dev_fd(dev), offset - mmap_offset);
		if (p->fb == (caddr_t) (-1)) {
			log_sys_error("mmap", dev_name(dev));
			goto out;
		}
		p->fb = p->fb + mmap_offset;
	} else {
		if (!(p->fb = dbg_malloc(size + size2))) {
			stack;
			return 0;
		}
		if (!dev_read(dev, (uint64_t) offset, size, p->fb)) {
			log_error("Read from %s failed", dev_name(dev));
			goto out;
		}
		if (size2) {
			if (!dev_read(dev, (uint64_t) offset2, size2,
				      p->fb + size)) {
				log_error("Circular read from %s failed",
					  dev_name(dev));
				goto out;
			}
		}
	}

	if (checksum_fn && checksum !=
	    (checksum_fn(checksum_fn(INITIAL_CRC, p->fb, size),
			 p->fb + size, size2))) {
		log_error("%s: Checksum error", dev_name(dev));
		goto out;
	}

	p->fe = p->fb + size + size2;

	/* parse */
	p->tb = p->te = p->fb;
	p->line = 1;
	_get_token(p, TOK_SECTION_E);
	if (!(cft->root = _file(p))) {
		stack;
		goto out;
	}

	r = 1;

      out:
	if (!use_mmap)
		dbg_free(p->fb);
	else {
		/* unmap the file */
		if (munmap((char *) (p->fb - mmap_offset), size + mmap_offset)) {
			log_sys_error("munmap", dev_name(dev));
			r = 0;
		}
	}

	return r;
}

int read_config_file(struct config_tree *cft)
{
	struct cs *c = (struct cs *) cft;
	struct stat info;
	struct device *dev;
	int r = 1;

	if (stat(c->filename, &info)) {
		log_sys_error("stat", c->filename);
		c->exists = 0;
		return 0;
	}

	if (!S_ISREG(info.st_mode)) {
		log_error("%s is not a regular file", c->filename);
		c->exists = 0;
		return 0;
	}

	c->exists = 1;

	if (info.st_size == 0) {
		log_verbose("%s is empty", c->filename);
		return 1;
	}

	if (!(dev = dev_create_file(c->filename, NULL, NULL))) {
		stack;
		return 0;
	}

	if (!dev_open_flags(dev, O_RDONLY, 0, 0)) {
		stack;
		return 0;
	}

	r = read_config_fd(cft, dev, 0, (size_t) info.st_size, 0, 0,
			   (checksum_fn_t) NULL, 0);

	dev_close(dev);

	c->timestamp = info.st_mtime;

	return r;
}

time_t config_file_timestamp(struct config_tree *cft)
{
	struct cs *c = (struct cs *) cft;

	return c->timestamp;
}

/*
 * Return 1 if config files ought to be reloaded
 */
int config_file_changed(struct config_tree *cft)
{
	struct cs *c = (struct cs *) cft;
	struct stat info;

	if (!c->filename)
		return 0;

	if (stat(c->filename, &info) == -1) {
		/* Ignore a deleted config file: still use original data */
		if (errno == ENOENT) {
			if (!c->exists)
				return 0;
			log_very_verbose("Config file %s has disappeared!",
					 c->filename);
			goto reload;
		}
		log_sys_error("stat", c->filename);
		log_error("Failed to reload configuration files");
		return 0;
	}

	if (!S_ISREG(info.st_mode)) {
		log_error("Configuration file %s is not a regular file",
			  c->filename);
		goto reload;
	}

	/* Unchanged? */
	if (c->timestamp == info.st_mtime)
		return 0;

      reload:
	log_verbose("Detected config file change to %s", c->filename);
	return 1;
}

static void _write_value(FILE *fp, struct config_value *v)
{
	switch (v->type) {
	case CFG_STRING:
		fprintf(fp, "\"%s\"", v->v.str);
		break;

	case CFG_FLOAT:
		fprintf(fp, "%f", v->v.r);
		break;

	case CFG_INT:
		fprintf(fp, "%d", v->v.i);
		break;

	case CFG_EMPTY_ARRAY:
		fprintf(fp, "[]");
		break;

	default:
		log_error("_write_value: Unknown value type: %d", v->type);

	}
}

static int _write_config(struct config_node *n, FILE *fp, int level)
{
	char space[MAX_INDENT + 1];
	int l = (level < MAX_INDENT) ? level : MAX_INDENT;
	int i;

	if (!n)
		return 1;

	for (i = 0; i < l; i++)
		space[i] = '\t';
	space[i] = '\0';

	while (n) {
		fprintf(fp, "%s%s", space, n->key);
		if (!n->v) {
			/* it's a sub section */
			fprintf(fp, " {\n");
			_write_config(n->child, fp, level + 1);
			fprintf(fp, "%s}", space);
		} else {
			/* it's a value */
			struct config_value *v = n->v;
			fprintf(fp, "=");
			if (v->next) {
				fprintf(fp, "[");
				while (v) {
					_write_value(fp, v);
					v = v->next;
					if (v)
						fprintf(fp, ", ");
				}
				fprintf(fp, "]");
			} else
				_write_value(fp, v);
		}
		fprintf(fp, "\n");
		n = n->sib;
	}
	/* FIXME: add error checking */
	return 1;
}

int write_config_file(struct config_tree *cft, const char *file)
{
	int r = 1;
	FILE *fp;

	if (!file) {
		fp = stdout;
		file = "stdout";
	} else if (!(fp = fopen(file, "w"))) {
		log_sys_error("open", file);
		return 0;
	}

	log_verbose("Dumping configuration to %s", file);
	if (!_write_config(cft->root, fp, 0)) {
		log_error("Failure while writing configuration");
		r = 0;
	}

	if (fp != stdout)
		fclose(fp);

	return r;
}

/*
 * parser
 */
static struct config_node *_file(struct parser *p)
{
	struct config_node *root = NULL, *n, *l = NULL;
	while (p->t != TOK_EOF) {
		if (!(n = _section(p))) {
			stack;
			return 0;
		}

		if (!root)
			root = n;
		else
			l->sib = n;
		l = n;
	}
	return root;
}

static struct config_node *_section(struct parser *p)
{
	/* IDENTIFIER '{' VALUE* '}' */
	struct config_node *root, *n, *l = NULL;
	if (!(root = _create_node(p))) {
		stack;
		return 0;
	}

	if (!(root->key = _dup_tok(p))) {
		stack;
		return 0;
	}

	match(TOK_IDENTIFIER);

	if (p->t == TOK_SECTION_B) {
		match(TOK_SECTION_B);
		while (p->t != TOK_SECTION_E) {
			if (!(n = _section(p))) {
				stack;
				return 0;
			}

			if (!root->child)
				root->child = n;
			else
				l->sib = n;
			l = n;
		}
		match(TOK_SECTION_E);
	} else {
		match(TOK_EQ);
		if (!(root->v = _value(p))) {
			stack;
			return 0;
		}
	}

	return root;
}

static struct config_value *_value(struct parser *p)
{
	/* '[' TYPE* ']' | TYPE */
	struct config_value *h = NULL, *l, *ll = NULL;
	if (p->t == TOK_ARRAY_B) {
		match(TOK_ARRAY_B);
		while (p->t != TOK_ARRAY_E) {
			if (!(l = _type(p))) {
				stack;
				return 0;
			}

			if (!h)
				h = l;
			else
				ll->next = l;
			ll = l;

			if (p->t == TOK_COMMA)
				match(TOK_COMMA);
		}
		match(TOK_ARRAY_E);
		/*
		 * Special case for an empty array.
		 */
		if (!h) {
			if (!(h = _create_value(p)))
				return NULL;

			h->type = CFG_EMPTY_ARRAY;
		}

	} else
		h = _type(p);

	return h;
}

static struct config_value *_type(struct parser *p)
{
	/* [+-]{0,1}[0-9]+ | [0-9]*\.[0-9]* | ".*" */
	struct config_value *v = _create_value(p);

	if (!v)
		return NULL;

	switch (p->t) {
	case TOK_INT:
		v->type = CFG_INT;
		v->v.i = strtol(p->tb, NULL, 0);	/* FIXME: check error */
		match(TOK_INT);
		break;

	case TOK_FLOAT:
		v->type = CFG_FLOAT;
		v->v.r = strtod(p->tb, NULL);	/* FIXME: check error */
		match(TOK_FLOAT);
		break;

	case TOK_STRING:
		v->type = CFG_STRING;

		p->tb++, p->te--;	/* strip "'s */
		if (!(v->v.str = _dup_tok(p))) {
			stack;
			return 0;
		}
		p->te++;
		match(TOK_STRING);
		break;

	default:
		log_error("Parse error at line %d: expected a value", p->line);
		return 0;
	}
	return v;
}

static int _match_aux(struct parser *p, int t)
{
	if (p->t != t)
		return 0;

	_get_token(p, t);
	return 1;
}

/*
 * tokeniser
 */
static void _get_token(struct parser *p, int tok_prev)
{
	int values_allowed = 0;

	p->tb = p->te;
	_eat_space(p);
	if (p->tb == p->fe || !*p->tb) {
		p->t = TOK_EOF;
		return;
	}

	/* Should next token be interpreted as value instead of identifier? */
	if (tok_prev == TOK_EQ || tok_prev == TOK_ARRAY_B ||
	    tok_prev == TOK_COMMA)
		values_allowed = 1;

	p->t = TOK_INT;		/* fudge so the fall through for
				   floats works */
	switch (*p->te) {
	case '{':
		p->t = TOK_SECTION_B;
		p->te++;
		break;

	case '}':
		p->t = TOK_SECTION_E;
		p->te++;
		break;

	case '[':
		p->t = TOK_ARRAY_B;
		p->te++;
		break;

	case ']':
		p->t = TOK_ARRAY_E;
		p->te++;
		break;

	case ',':
		p->t = TOK_COMMA;
		p->te++;
		break;

	case '=':
		p->t = TOK_EQ;
		p->te++;
		break;

	case '"':
		p->t = TOK_STRING;
		p->te++;
		while ((p->te != p->fe) && (*p->te) && (*p->te != '"')) {
			if ((*p->te == '\\') && (p->te + 1 != p->fe) &&
			    *(p->te + 1))
				p->te++;
			p->te++;
		}

		if ((p->te != p->fe) && (*p->te))
			p->te++;
		break;

	case '\'':
		p->t = TOK_STRING;
		p->te++;
		while ((p->te != p->fe) && (*p->te) && (*p->te != '\''))
			p->te++;

		if ((p->te != p->fe) && (*p->te))
			p->te++;
		break;

	case '.':
		p->t = TOK_FLOAT;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case '+':
	case '-':
		if (values_allowed) {
			p->te++;
			while ((p->te != p->fe) && (*p->te)) {
				if (*p->te == '.') {
					if (p->t == TOK_FLOAT)
						break;
					p->t = TOK_FLOAT;
				} else if (!isdigit((int) *p->te))
					break;
				p->te++;
			}
			break;
		}

	default:
		p->t = TOK_IDENTIFIER;
		while ((p->te != p->fe) && (*p->te) && !isspace(*p->te) &&
		       (*p->te != '#') && (*p->te != '=') && (*p->te != '{') &&
		       (*p->te != '}'))
			p->te++;
		break;
	}
}

static void _eat_space(struct parser *p)
{
	while ((p->tb != p->fe) && (*p->tb)) {
		if (*p->te == '#') {
			while ((p->te != p->fe) && (*p->te) && (*p->te != '\n'))
				p->te++;
			p->line++;
		}

		else if (isspace(*p->te)) {
			while ((p->te != p->fe) && (*p->te) && isspace(*p->te)) {
				if (*p->te == '\n')
					p->line++;
				p->te++;
			}
		}

		else
			return;

		p->tb = p->te;
	}
}

/*
 * memory management
 */
static struct config_value *_create_value(struct parser *p)
{
	struct config_value *v = pool_alloc(p->mem, sizeof(*v));
	memset(v, 0, sizeof(*v));
	return v;
}

static struct config_node *_create_node(struct parser *p)
{
	struct config_node *n = pool_alloc(p->mem, sizeof(*n));
	memset(n, 0, sizeof(*n));
	return n;
}

static char *_dup_tok(struct parser *p)
{
	size_t len = p->te - p->tb;
	char *str = pool_alloc(p->mem, len + 1);
	if (!str) {
		stack;
		return 0;
	}
	strncpy(str, p->tb, len);
	str[len] = '\0';
	return str;
}

/*
 * utility functions
 */
struct config_node *find_config_node(const struct config_node *cn,
				     const char *path)
{
	const char *e;

	while (cn) {
		/* trim any leading slashes */
		while (*path && (*path == sep))
			path++;

		/* find the end of this segment */
		for (e = path; *e && (*e != sep); e++) ;

		/* hunt for the node */
		while (cn) {
			if (_tok_match(cn->key, path, e))
				break;

			cn = cn->sib;
		}

		if (cn && *e)
			cn = cn->child;
		else
			break;	/* don't move into the last node */

		path = e;
	}

	return (struct config_node *) cn;
}

const char *find_config_str(const struct config_node *cn,
			    const char *path, const char *fail)
{
	const struct config_node *n = find_config_node(cn, path);

	if (n && n->v->type == CFG_STRING) {
		if (*n->v->v.str)
			log_very_verbose("Setting %s to %s", path, n->v->v.str);
		return n->v->v.str;
	}

	if (fail)
		log_very_verbose("%s not found in config: defaulting to %s",
				 path, fail);
	return fail;
}

int find_config_int(const struct config_node *cn, const char *path, int fail)
{
	const struct config_node *n = find_config_node(cn, path);

	if (n && n->v->type == CFG_INT) {
		log_very_verbose("Setting %s to %d", path, n->v->v.i);
		return n->v->v.i;
	}

	log_very_verbose("%s not found in config: defaulting to %d",
			 path, fail);
	return fail;
}

float find_config_float(const struct config_node *cn, const char *path,
			float fail)
{
	const struct config_node *n = find_config_node(cn, path);

	if (n && n->v->type == CFG_FLOAT) {
		log_very_verbose("Setting %s to %f", path, n->v->v.r);
		return n->v->v.r;
	}

	log_very_verbose("%s not found in config: defaulting to %f",
			 path, fail);

	return fail;

}

static int _str_in_array(const char *str, const char *values[])
{
	int i;

	for (i = 0; values[i]; i++)
		if (!strcasecmp(str, values[i]))
			return 1;

	return 0;
}

static int _str_to_bool(const char *str, int fail)
{
	static const char *_true_values[] = { "y", "yes", "on", "true", NULL };
	static const char *_false_values[] =
	    { "n", "no", "off", "false", NULL };

	if (_str_in_array(str, _true_values))
		return 1;

	if (_str_in_array(str, _false_values))
		return 0;

	return fail;
}

int find_config_bool(const struct config_node *cn, const char *path, int fail)
{
	const struct config_node *n = find_config_node(cn, path);
	struct config_value *v;

	if (!n)
		return fail;

	v = n->v;

	switch (v->type) {
	case CFG_INT:
		return v->v.i ? 1 : 0;

	case CFG_STRING:
		return _str_to_bool(v->v.str, fail);
	}

	return fail;
}

int get_config_uint32(const struct config_node *cn, const char *path,
		      uint32_t *result)
{
	const struct config_node *n;

	n = find_config_node(cn, path);

	if (!n || !n->v || n->v->type != CFG_INT)
		return 0;

	*result = n->v->v.i;
	return 1;
}

int get_config_uint64(const struct config_node *cn, const char *path,
		      uint64_t *result)
{
	const struct config_node *n;

	n = find_config_node(cn, path);

	if (!n || !n->v || n->v->type != CFG_INT)
		return 0;

	/* FIXME Support 64-bit value! */
	*result = (uint64_t) n->v->v.i;
	return 1;
}

int get_config_str(const struct config_node *cn, const char *path,
		   char **result)
{
	const struct config_node *n;

	n = find_config_node(cn, path);

	if (!n || !n->v || n->v->type != CFG_STRING)
		return 0;

	*result = n->v->v.str;
	return 1;
}

/* Insert cn2 after cn1 */
static void _insert_config_node(struct config_node **cn1,
				struct config_node *cn2)
{
	if (!*cn1) {
		*cn1 = cn2;
		cn2->sib = NULL;
	} else {
		cn2->sib = (*cn1)->sib;
		(*cn1)->sib = cn2;
	}
}

/*
 * Merge section cn2 into section cn1 (which has the same name)
 * overwriting any existing cn1 nodes with matching names.
 */
static void _merge_section(struct config_node *cn1, struct config_node *cn2)
{
	struct config_node *cn, *nextn, *oldn;
	struct config_value *cv;

	for (cn = cn2->child; cn; cn = nextn) {
		nextn = cn->sib;

		/* Skip "tags" */
		if (!strcmp(cn->key, "tags"))
			continue;

		/* Subsection? */
		if (!cn->v)
			/* Ignore - we don't have any of these yet */
			continue;
		/* Not already present? */
		if (!(oldn = find_config_node(cn1->child, cn->key))) {
			_insert_config_node(&cn1->child, cn);
			continue;
		}
		/* Merge certain value lists */
		if ((!strcmp(cn1->key, "activation") &&
		     !strcmp(cn->key, "volume_list")) ||
		    (!strcmp(cn1->key, "devices") &&
		     (!strcmp(cn->key, "filter") || !strcmp(cn->key, "types")))) {
			cv = cn->v;
			while (cv->next)
				cv = cv->next;
			cv->next = oldn->v;
		}

		/* Replace values */
		oldn->v = cn->v;
	}
}

static int _match_host_tags(struct list *tags, struct config_node *tn)
{
	struct config_value *tv;
	const char *str;

	for (tv = tn->v; tv; tv = tv->next) {
		if (tv->type != CFG_STRING)
			continue;
		str = tv->v.str;
		if (*str == '@')
			str++;
		if (!*str)
			continue;
		if (str_list_match_item(tags, str))
			return 1;
	}

	return 0;
}

/* Destructively merge a new config tree into an existing one */
int merge_config_tree(struct cmd_context *cmd, struct config_tree *cft,
		      struct config_tree *newdata)
{
	struct config_node *root = cft->root;
	struct config_node *cn, *nextn, *oldn, *tn, *cn2;

	for (cn = newdata->root; cn; cn = nextn) {
		nextn = cn->sib;
		/* Ignore tags section */
		if (!strcmp(cn->key, "tags"))
			continue;
		/* If there's a tags node, skip if host tags don't match */
		if ((tn = find_config_node(cn->child, "tags"))) {
			if (!_match_host_tags(&cmd->tags, tn))
				continue;
		}
		if (!(oldn = find_config_node(root, cn->key))) {
			_insert_config_node(&cft->root, cn);
			/* Remove any "tags" nodes */
			for (cn2 = cn->child; cn2; cn2 = cn2->sib) {
				if (!strcmp(cn2->key, "tags")) {
					cn->child = cn2->sib;
					continue;
				}
				if (cn2->sib && !strcmp(cn2->sib->key, "tags")) {
					cn2->sib = cn2->sib->sib;
					continue;
				}
			}
			continue;
		}
		_merge_section(oldn, cn);
	}

	return 1;
}
